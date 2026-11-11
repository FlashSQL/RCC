#include "dl_detect.h"
#include <new>
#if CC_ALG == RCC && RCC_ADD && RCC_DETECT_EVENT
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#endif
#include "global.h"
#include "helper.h"
#include "txn.h"
#include "row.h"
#include "manager.h"
#include "mem_alloc.h"
#if CC_ALG == RCC
#include "row_rcc.h"   // RCC_NO_PRED
#include <mm_malloc.h>
#elif CC_ALG == BAMBOO && BB_FAKE_DETECT
// [BB_FAKE_DETECT] BAMBOO control: definitions for reading BBLockEntry lines + sentinel
#include "row_bamboo.h"
#include <mm_malloc.h>
#define RCC_NO_PRED ((txnid_t)-1)
#endif

/********************************************************/
// The current txn aborts itself only if it holds less
// locks than all the other txns on the loop. 
// In other words, the victim should be the txn that 
// performs the least amount of work
/********************************************************/
void DL_detect::init() {
	dependency = new DepThd[g_thread_cnt];
    for (int i = 0; i < g_thread_cnt; i++) {
        pthread_mutex_init(&dependency[i].lock, nullptr);
    }
	V = g_thread_cnt;
}

// Add edges
int
DL_detect::add_dep(uint64_t txnid1, uint64_t * txnids, int cnt, int num_locks) {
	if (g_no_dl)
		return 0;
	int thd1 = get_thdid_from_txnid(txnid1);
	pthread_mutex_lock( &dependency[thd1].lock );
	dependency[thd1].txnid = txnid1;
	dependency[thd1].num_locks = num_locks;
	
	for (int i = 0; i < cnt; i++) 
		dependency[thd1].adj.push_back(txnids[i]); // add an edge to each txnid in txnids
	
	pthread_mutex_unlock( &dependency[thd1].lock );
	return 0;
}

// [RCC]
int DL_detect::set_dep(uint64_t txnid, uint64_t * txnids, int cnt, int num_locks) {
    int thd = get_thdid_from_txnid(txnid);
    pthread_mutex_lock(&dependency[thd].lock);
    dependency[thd].adj.clear();                 // rebuilt from scratch every time
    dependency[thd].txnid = txnid;
    dependency[thd].num_locks = num_locks;

    for (int i = 0; i < cnt; i++)
		dependency[thd].adj.push_back(txnids[i]);

    pthread_mutex_unlock(&dependency[thd].lock);
    return 0;
}

bool 
DL_detect::nextNode(uint64_t txnid, DetectData * detect_data) {
	int thd = get_thdid_from_txnid(txnid);
	assert( !detect_data->visited[thd] );
	detect_data->visited[thd] = true;
	detect_data->recStack[thd] = true;
	
	pthread_mutex_lock( &dependency[thd].lock );
	
	int lock_num = dependency[thd].num_locks;
	int txnid_num = dependency[thd].adj.size();
	uint64_t txnids[ txnid_num ];
	int n = 0;
	
	if (dependency[thd].txnid != (SInt64)txnid) {
		detect_data->recStack[thd] = false;
		pthread_mutex_unlock( &dependency[thd].lock );
		return false;
	}
	
	for(list<uint64_t>::iterator i = dependency[thd].adj.begin(); i != dependency[thd].adj.end(); ++i) {
		txnids[n++] = *i;
	}
	
	pthread_mutex_unlock( &dependency[thd].lock );

	for (n = 0; n < txnid_num; n++) {
		int nextthd = get_thdid_from_txnid( txnids[n] );

		// next node not visited and txnid is not stale
		if ( detect_data->recStack[nextthd] ) {
			if ((SInt32)txnids[n] == dependency[nextthd].txnid) {
				detect_data->loop = true;
				detect_data->onloop = true;
				detect_data->loopstart = nextthd;
				break;
			}
		} 
		if ( !detect_data->visited[nextthd] && 
			dependency[nextthd].txnid == (SInt64) txnids[n] && 
			nextNode(txnids[n], detect_data)) 
		{
			break;
		}
	}
	detect_data->recStack[thd] = false;
	if (detect_data->loop 
			&& detect_data->onloop 
			&& lock_num < detect_data->min_lock_num) {
		detect_data->min_lock_num = lock_num;
		detect_data->min_txnid = txnid;
	}
	if (thd == detect_data->loopstart) {
		detect_data->onloop = false;
	}
	return detect_data->loop;
}

// isCycle returns true if there is a loop AND the current txn holds the least 
// number of locks on that loop.
bool DL_detect::isCyclic(uint64_t txnid, DetectData * detect_data) {
	return nextNode(txnid, detect_data);
}

// Victim == the txn with the smallest txn->row_cnt == the one that did the least work
int
DL_detect::detect_cycle(uint64_t txnid) {
	if (g_no_dl)
		return 0;
	uint64_t starttime = get_sys_clock();
	INC_GLOB_STATS(cycle_detect, 1);
	bool deadlock = false;

	int thd = get_thdid_from_txnid(txnid);
	DetectData * detect_data = (DetectData *)
		mem_allocator.alloc(sizeof(DetectData), thd);
	detect_data->visited = (bool * )
		mem_allocator.alloc(sizeof(bool) * V, thd);
	detect_data->recStack = (bool * )
		mem_allocator.alloc(sizeof(bool) * V, thd);	
	for(int i = 0; i < V; i++) {
        detect_data->visited[i] = false;
		detect_data->recStack[i] = false;
	}

	detect_data->min_lock_num = 1000;
	detect_data->min_txnid = -1;
	detect_data->loop = false;

	if ( isCyclic(txnid, detect_data) ){ // DFS
		deadlock = true;
		INC_GLOB_STATS(deadlock, 1);
		int thd_to_abort = get_thdid_from_txnid(detect_data->min_txnid);
		if (dependency[thd_to_abort].txnid == (SInt64) detect_data->min_txnid) {
			txn_man * txn = glob_manager->get_txn_man(thd_to_abort);
#if CC_ALG == RCC
			// [RCC] lock_abort alone does not release the commit-wait spin.
			// set_abort() also sets the status bits of commit_barriers to ABORTED
			// and sets lock_abort internally as well.
			txn->set_abort();
			INC_STATS(get_thdid_from_txnid(txnid), rcc_lock_deadlock_cnt, 1);
#else
			txn->lock_abort = true;
#endif
		}
	} 
	
	mem_allocator.free(detect_data->visited, sizeof(bool)*V);
	mem_allocator.free(detect_data->recStack, sizeof(bool)*V);
	mem_allocator.free(detect_data, sizeof(DetectData));
	uint64_t timespan = get_sys_clock() - starttime;
	INC_GLOB_STATS(dl_detect_time, timespan);
	if (deadlock) return 1;
	else return 0;
}

void DL_detect::clear_dep(uint64_t txnid) {
	if (g_no_dl)
		return;
	int thd = get_thdid_from_txnid(txnid);
	pthread_mutex_lock( &dependency[thd].lock );

	dependency[thd].adj.clear();
	dependency[thd].txnid = -1;
	dependency[thd].num_locks = 0;

	pthread_mutex_unlock( &dependency[thd].lock );
}

#if CC_ALG == RCC || (CC_ALG == BAMBOO && BB_FAKE_DETECT)
/**********************************************************************
 * [RCC] Round-based deadlock detector
 *
 * Fully separate from the DL_DETECT path (above). Splits publishing (per thread, lock-free) from
 * searching (global, once per period, by one representative thread).
 *
 * - Publish: rcc_publish(). Writes only its own slot under the seqlock; skipped if unchanged.
 *         A deadlocked thread's edges stop changing, so its slot is stable after one publish.
 * - Search: dedicated threads check the whole graph on event notifications / a retry timer.
 *         With RCC_DETECT_EVENT=false they check continuously. Without dedicated threads,
 *         rcc_try_round()'s time period + TTAS busy let a single thread check.
 *         Snapshot -> explicit-stack DFS O(V+E) -> version/txnid/edge revalidation.
 *         RCC dedicated threads abort the readers of a confirmed cycle (every member if it is all-dirty).
 **********************************************************************/

void DL_detect::rcc_init(DL_detect * shared, int detector_id) {
	V = g_thread_cnt;
	if (shared) {
		rcc_slots = shared->rcc_slots;
	} else {
	rcc_slots = (RccSlot *) _mm_malloc(sizeof(RccSlot) * V, 64);
	for (int i = 0; i < V; i++) {
		rcc_slots[i].version = 0;
		rcc_slots[i].txnid   = RCC_NO_PRED;
		rcc_slots[i].cnt     = 0;
	}
	}
	rcc_round_busy     = 0;
	rcc_last_round_end = 0;
	snap_txnid   = (uint64_t *) _mm_malloc(sizeof(uint64_t) * V, 64);
	snap_version = (uint64_t *) _mm_malloc(sizeof(uint64_t) * V, 64);
	snap_cnt     = (int *)      _mm_malloc(sizeof(int) * V, 64);
	snap_entries = (RCCLockEntry **) _mm_malloc(sizeof(RCCLockEntry *) * V * (MAX_ROW_PER_TXN+1), 64);
	snap_preds   = (uint64_t *) _mm_malloc(sizeof(uint64_t) * V * (MAX_ROW_PER_TXN+1), 64);
	rcc_color      = (char *) _mm_malloc(sizeof(char) * V, 64);
	rcc_stack_node = (int *)  _mm_malloc(sizeof(int) * V, 64);
	rcc_stack_it   = (int *)  _mm_malloc(sizeof(int) * V, 64);
	// [persistence filter]
	rcc_round_no = 0;
	rcc_susp_cap = 2 * V;
	rcc_susp = (RccSuspect *) _mm_malloc(sizeof(RccSuspect) * rcc_susp_cap, 64);
	for (int i = 0; i < rcc_susp_cap; i++) {
		rcc_susp[i].sig = 0;
		rcc_susp[i].last_round = 0;
		rcc_susp[i].first_ns = 0;
		rcc_susp[i].streak = 0;
	}
	rcc_round_ns = 0;
	// [measurement: pred cache feasibility]
	if (shared) {
		rcc_repoint_ctr = shared->rcc_repoint_ctr;
		rcc_predclear_ctr = shared->rcc_predclear_ctr;
	} else {
	rcc_repoint_ctr   = (RccEpochCtr *) _mm_malloc(sizeof(RccEpochCtr) * V, 64);
	rcc_predclear_ctr = (RccEpochCtr *) _mm_malloc(sizeof(RccEpochCtr) * V, 64);
	for (int i = 0; i < V; i++) {
		rcc_repoint_ctr[i].v   = 0;
		rcc_predclear_ctr[i].v = 0;
	}
	}
	rcc_last_repoint_sum   = 0;
	rcc_last_predclear_sum = 0;
#if CC_ALG == RCC && RCC_ADD
	rcc_detector_id = detector_id;
	rcc_root_begin = V * detector_id / RCC_ADD_THREADS;
	rcc_root_end = V * (detector_id + 1) / RCC_ADD_THREADS;
	rcc_cycle_stats = {};
	rcc_acc_snapshot_nodes = rcc_acc_cross_nodes = rcc_acc_guard_busy = 0;
	if (shared) rcc_cycle_guards = shared->rcc_cycle_guards;
	else {
		rcc_cycle_guards = (RccCycleGuard *) _mm_malloc(sizeof(RccCycleGuard) * V, 64);
		for (int i = 0; i < V; ++i) rcc_cycle_guards[i].held = 0;
	}
	__atomic_store_n(&rcc_detector_stop, false, __ATOMIC_RELEASE);
#if RCC_DETECT_EVENT
    rcc_event = 1; // initial scan; worker slots can be empty at startup
    rcc_event_waits = rcc_event_timeouts = rcc_event_idle_ns = 0;
#endif
	rcc_acc_rounds      = 0;
	rcc_acc_detect_time = 0;
	rcc_acc_derive_time = 0;
	rcc_acc_cache_sound = 0;
	rcc_acc_cache_exact = 0;
#endif
}

#if CC_ALG == RCC && RCC_ADD
// [dedicated detector thread] A worker signals a graph change after publishing its slot.
// Notifications during a round coalesce into one pending round; with none pending, the detector waits on a futex.
// The timer retries incomplete snapshots. No upper bound on OS scheduling delay is guaranteed.
// RCC_DETECT_EVENT=false keeps continuous checking. Per-thread stats are counted under thread 0.
#if RCC_DETECT_EVENT
void DL_detect::rcc_request_check() {
    uint32_t observed = __atomic_load_n(&rcc_event, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == 1) return; // coalesce; no RMW on the common pending path
        if (__atomic_compare_exchange_n(&rcc_event, &observed, 1, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (observed == 2)
                syscall(SYS_futex, &rcc_event, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
            return;
        }
    }
}
#endif

void DL_detect::rcc_detector_halt() {
    __atomic_store_n(&rcc_detector_stop, true, __ATOMIC_RELEASE);
#if RCC_DETECT_EVENT
    rcc_request_check(); // wake a sleeping detector so shutdown cannot hang
#endif
}

void DL_detect::rcc_detector_loop() {
    while (!__atomic_load_n(&rcc_detector_stop, __ATOMIC_ACQUIRE)) {
#if RCC_DETECT_EVENT
        // Consume BEFORE scanning: changes during the scan remain pending for
        // another round. CAS + futex's value check closes the lost-wakeup gap.
        if (__atomic_exchange_n(&rcc_event, 0, __ATOMIC_ACQ_REL) != 1) {
            uint32_t expected = 0;
            if (!__atomic_compare_exchange_n(&rcc_event, &expected, 2, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;
            const uint64_t retry = RCC_DETECT_EVENT_RETRY_NS;
            const timespec timeout = {time_t(retry / 1000000000UL),
                                      long(retry % 1000000000UL)};
            uint64_t idle_start = get_sys_clock();
            ++rcc_event_waits;
            int result = syscall(SYS_futex, &rcc_event, FUTEX_WAIT_PRIVATE, 2,
                                 &timeout, nullptr, 0);
            if (result == -1 && errno == ETIMEDOUT) ++rcc_event_timeouts;
            rcc_event_idle_ns += get_sys_clock() - idle_start;
            __atomic_exchange_n(&rcc_event, 0, __ATOMIC_ACQ_REL);
            // Timeout/spurious wakes also rescan: a bounded snapshot can have
            // skipped an entry, even after the graph subsequently stabilizes.
        }
        if (__atomic_load_n(&rcc_detector_stop, __ATOMIC_ACQUIRE)) break;
#endif
		uint64_t t0 = get_sys_clock();
		rcc_run_round(0);
		// No per-round atomic increments of global stats (see the rcc_acc_* comment in dl_detect.h).
		// rcc_last_round_end is not written either: in dedicated-thread mode it is a shared write nobody reads.
		rcc_acc_detect_time += get_sys_clock() - t0;
#if RCC_DETECT_PAUSE_NS > 0
		// [detector pause] idle between rounds without touching shared cache lines. Back-to-back rounds make the
		// detector re-read the workers' slot / lock-entry lines from another L3 millions of times per second;
		// every worker write to such a line then pays a cross-L3 transfer. Cost of the pause: a cycle is found up
		// to RCC_DETECT_PAUSE_NS later.
		{ uint64_t until = get_sys_clock() + RCC_DETECT_PAUSE_NS;
		  while (get_sys_clock() < until && !__atomic_load_n(&rcc_detector_stop, __ATOMIC_ACQUIRE)) PAUSE }
#endif
	}
#if RCC_DETECT_EVENT
    printf("[rcc_event] rounds=%lu waits=%lu timeouts=%lu active_ns=%lu idle_ns=%lu retry_ns=%lu\n",
           rcc_acc_rounds, rcc_event_waits, rcc_event_timeouts,
           rcc_acc_detect_time, rcc_event_idle_ns, (uint64_t)RCC_DETECT_EVENT_RETRY_NS);
#endif
    printf("[rcc_detector] id=%d root_begin=%d root_end=%d rounds=%lu snapshot_nodes=%lu cross_nodes=%lu guard_busy=%lu active_ns=%lu confirmed=%lu rv_cycles=%lu rv_victims=%lu rv_alldirty_members=%lu\n",
           rcc_detector_id, rcc_root_begin, rcc_root_end, rcc_acc_rounds,
           rcc_acc_snapshot_nodes, rcc_acc_cross_nodes, rcc_acc_guard_busy,
           rcc_acc_detect_time, rcc_cycle_stats.rcc_confirmed, rcc_cycle_stats.rv_cycles, rcc_cycle_stats.rv_victims, rcc_cycle_stats.rv_alldirty_members);
}

void DL_detect::rcc_detector_collect() {
	INC_GLOB_STATS(dl_detect_time,        rcc_acc_detect_time);
	INC_GLOB_STATS(cycle_detect,          rcc_acc_rounds);
	INC_GLOB_STATS(rcc_derive_time,       rcc_acc_derive_time);
	INC_GLOB_STATS(rcc_round_cache_sound, rcc_acc_cache_sound);
	INC_GLOB_STATS(rcc_round_cache_exact, rcc_acc_cache_exact);
#define RCC_MERGE(name) INC_GLOB_STATS(name, rcc_cycle_stats.name)
    RCC_MERGE(deadlock);
    RCC_MERGE(rcc_false_positive);
    RCC_MERGE(rcc_confirmed);
    RCC_MERGE(rcc_cycle_len_sum);
    RCC_MERGE(rcc_dl_lockwait);
    RCC_MERGE(rcc_dl_commitwait);
    RCC_MERGE(rcc_dl_running);
#undef RCC_MERGE
    if (STATS_ENABLE && stats.rcc_cycle_len_max < rcc_cycle_stats.rcc_cycle_len_max)
        stats.rcc_cycle_len_max = rcc_cycle_stats.rcc_cycle_len_max;
    INC_STATS(0, rcc_lock_deadlock_cnt, rcc_cycle_stats.rcc_lock_deadlock_cnt);

}
#endif

// Writes only the caller's own slot (txnid % V == its thd_id). No cross-thread write contention.
// [early publish] Publishes only the list of entry references; the round derives the edges.
void DL_detect::rcc_publish(uint64_t txnid, Access * const * accesses, int cnt, int & scan_idx) {
	// [publish only dependencies] Only entries that can carry an edge are published. rcc_entry_dep() yields an edge
	// only for a non-cohead entry, and a granted entry never turns cohead -> non-cohead. An entry whose pred is RCC_NO_PRED when it is offered therefore never has an
	// edge; leaving it out keeps the detector's input identical while the detectors stop polling its cache lines.
	if (scan_idx >= cnt)
		return; // nothing new to offer: do not even touch the slot line the detectors poll
	RCCLockEntry * fresh[MAX_ROW_PER_TXN + 1];
	int nfresh = 0;
	for (int i = scan_idx; i < cnt; i++) {
		// BB_FAKE_DETECT also publishes Access objects with BBLockEntry pointers.
		RCCLockEntry * e = (RCCLockEntry *)(void *) accesses[i]->lock_entry;
#if CC_ALG == RCC
		if (e->pred == RCC_NO_PRED) continue;
#endif
		fresh[nfresh++] = e;
	}
	scan_idx = cnt;
	if (nfresh == 0)
		return;
	RccSlot & s = rcc_slots[txnid % V];
	// One worker owns this slot; cleanup clears it before the next attempt, so a matching txnid means the published
	// prefix is this attempt's and is only appended to.
	int w = (s.txnid == txnid) ? s.cnt : 0;
	s.version++;              // odd: write in progress
	COMPILER_BARRIER
	s.txnid = txnid;
	for (int k = 0; k < nfresh; k++)
		s.entries[w++] = fresh[k];
	s.cnt   = w;
	COMPILER_BARRIER
	s.version++;              // even: stable
#if CC_ALG == RCC && RCC_ADD && RCC_DETECT_EVENT
    rcc_request_check();
#endif
}

// [pred edge] Derives the blocker from one published entry (round runner only).
// pred = txnid of the nearest conflicting predecessor. It is maintained under the row latch
// (set by UPDATE_RETIRE_INFO / repointed by rm_from_retired / cleared by DEC_BARRIER),
// and read here with a single volatile 8B load, so the read is atomic. Unlike deriving from head (cur_head_id),
// the target changes only when the immediately preceding conflicting neighbor leaves, so edges are stable
// -> torn snapshots (phantom cycles) drop sharply and persistence-filter matching works.
// Races on the value are absorbed by (1) the txnid match check on the edge's target slot and (2) cycle revalidation.
//
// Waiter edges are not added: the benchmark retires writes immediately, so no txn blocks
// while holding owner; a lock-wait-only deadlock is structurally impossible.
// (if this assumption breaks, the RCC_LOCK_TIMEOUT backstop catches it and it shows in rcc_lock_timeout_cnt)
static inline uint64_t rcc_entry_dep(RCCLockEntry * en) {
#if CC_ALG == RCC
	lock_status st = en->status;
	if ((st == LOCK_RETIRED || st == LOCK_OWNER) && !en->is_cohead)
		return en->pred;
	return RCC_NO_PRED;
#else
	// [BB_FAKE_DETECT] Cost control: actually reads the same cache lines as RCC (type on the first line,
	// status/is_cohead on the second line) but never creates an edge.
	// -> the DFS sees an empty graph; cycles/set_abort are structurally impossible.
	BBLockEntry * be = (BBLockEntry *)(void *) en;
	volatile lock_t ty = be->type; (void)ty;
	volatile lock_status st = be->status; (void)st;
	volatile bool ch = be->is_cohead; (void)ch;
	return RCC_NO_PRED;
#endif
}

void DL_detect::rcc_clear(uint64_t txnid) {
	RccSlot & s = rcc_slots[txnid % V];
	if (s.txnid == RCC_NO_PRED && s.cnt == 0)
		return;
	s.version++;
	COMPILER_BARRIER
	s.txnid = RCC_NO_PRED;
	s.cnt   = 0;
	COMPILER_BARRIER
	s.version++;
}

bool DL_detect::rcc_try_round(uint64_t caller_thd) {
	uint64_t now = get_sys_clock();
	// (a) Most calls return here when the period has not elapsed (no shared line touched)
	if (now - rcc_last_round_end < g_rcc_detect_intvl)
		return false;
	// (b) test: plain read of busy
	if (rcc_round_busy)
		return false;
	// (c) test-and-set: actually acquire it
	if (!ATOM_CAS(rcc_round_busy, 0, 1))
		return false;

	// --- From here on, this thread runs the round alone ---
	// Note: even if this thread becomes a victim or becomes committable mid-round, it must never
	// leave here. Leaving while holding busy would stop all later detection.
	// set_abort only sets flags; busy is released after the round finishes.
	uint64_t t0 = get_sys_clock();
	rcc_run_round(caller_thd);
	INC_GLOB_STATS(dl_detect_time, get_sys_clock() - t0);

	rcc_last_round_end = get_sys_clock();
	COMPILER_BARRIER
	assert(rcc_round_busy == 1);   // only this thread may have held it
	rcc_round_busy = 0;
	return true;
}

void DL_detect::rcc_snapshot_node(int i) {
    const int stride = MAX_ROW_PER_TXN + 1;
		RccSlot & s = rcc_slots[i];
		int tries = 0;
		while (true) {
			uint64_t v1 = s.version;
			if (v1 & 1UL) {           // write in progress
				if (++tries > RCC_SNAP_RETRY) {
					snap_txnid[i] = RCC_NO_PRED; snap_cnt[i] = 0; snap_version[i] = v1;
					break;
				}
				continue;
			}
			COMPILER_BARRIER
			uint64_t tid = s.txnid;
			int c = s.cnt;
			if (c < 0) c = 0;
			if (c > stride) c = stride;
			for (int k = 0; k < c; k++)
				snap_entries[i*stride + k] = s.entries[k];
			COMPILER_BARRIER
			uint64_t v2 = s.version;
			if (v1 == v2) {           // unchanged during the read
				snap_txnid[i] = tid; snap_cnt[i] = c; snap_version[i] = v1;
				break;
			}
			if (++tries > RCC_SNAP_RETRY) {
				// Slot changes too often -> treat as edgeless (harmless: the thread is making progress)
				snap_txnid[i] = RCC_NO_PRED; snap_cnt[i] = 0; snap_version[i] = v2;
				break;
			}
		}
    if (snap_txnid[i] != RCC_NO_PRED) {
        for (int k = 0; k < snap_cnt[i]; ++k)
            snap_preds[i*stride + k] = rcc_entry_dep(snap_entries[i*stride + k]);
    }
    rcc_color[i] = 0;
#if CC_ALG == RCC && RCC_ADD
    ++rcc_acc_snapshot_nodes;
    if (i < rcc_root_begin || i >= rcc_root_end) ++rcc_acc_cross_nodes;
#endif
}

void DL_detect::rcc_run_round(uint64_t caller_thd) {
#if CC_ALG == RCC && RCC_ADD
	rcc_acc_rounds++;                  // exactly once per round (flushed to global stats at exit)
#else
	INC_GLOB_STATS(cycle_detect, 1);   // exactly once per round (independent of the number of waiters)
#endif
	rcc_round_no++;                    // [persistence filter] round number
	rcc_round_ns = get_sys_clock();    // [persistence filter] reference point for elapsed time
	const int stride = MAX_ROW_PER_TXN + 1;

#if CC_ALG == RCC && RCC_ADD && RCC_ADD_THREADS > 1
    // -1 means not snapshotted this round; pred edges can cross root partitions.
    // Snapshot+derive time is included in round time, not separately timed here.
    for (int i = 0; i < V; ++i) rcc_color[i] = -1;
#else
	// --- Snapshot: copy every slot (entry reference list) lock-free via the seqlock ---
	for (int i = 0; i < V; i++) {
		RccSlot & s = rcc_slots[i];
		int tries = 0;
		while (true) {
			uint64_t v1 = s.version;
			if (v1 & 1UL) {           // write in progress
				if (++tries > RCC_SNAP_RETRY) {
					snap_txnid[i] = RCC_NO_PRED; snap_cnt[i] = 0; snap_version[i] = v1;
					break;
				}
				continue;
			}
			COMPILER_BARRIER
			uint64_t tid = s.txnid;
			int c = s.cnt;
			if (c < 0) c = 0;
			if (c > stride) c = stride;
			for (int k = 0; k < c; k++)
				snap_entries[i*stride + k] = s.entries[k];
			COMPILER_BARRIER
			uint64_t v2 = s.version;
			if (v1 == v2) {           // unchanged during the read
				snap_txnid[i] = tid; snap_cnt[i] = c; snap_version[i] = v1;
				break;
			}
			if (++tries > RCC_SNAP_RETRY) {
				// Slot changes too often -> treat as edgeless (harmless: the thread is making progress)
				snap_txnid[i] = RCC_NO_PRED; snap_cnt[i] = 0; snap_version[i] = v2;
				break;
			}
		}
		rcc_color[i] = 0;   // WHITE, initialized once per round
	}

	// --- Edge derivation: read the current blocker from each published entry ---
	// Separate from the snapshot: derivation reads global row state, independent of seqlock retries.
	// [measurement] This loop's share of the round cost = upper bound recoverable by a slot cache.
	uint64_t derive_t0 = get_sys_clock();
	for (int i = 0; i < V; i++) {
		if (snap_txnid[i] == RCC_NO_PRED) continue;
		for (int k = 0; k < snap_cnt[i]; k++)
			snap_preds[i*stride + k] = rcc_entry_dep(snap_entries[i*stride + k]);
	}
#if CC_ALG == RCC && RCC_ADD
	rcc_acc_derive_time += get_sys_clock() - derive_t0;
#else
	INC_GLOB_STATS(rcc_derive_time, get_sys_clock() - derive_t0);
#endif

#if CC_ALG == RCC && RCC_ADD
    rcc_acc_snapshot_nodes += V;
#endif
#endif // eager single-detector / lazy partitioned snapshots

	// [measurement] Could this round have used a slot pred cache? (behavior unchanged)
	//   sound = no repoint   -> cache is sound (no missed cycles; revalidation handles false positives)
	//   exact = neither      -> cache matches the truth exactly
	// These two ratios determine the payoff of the cache approach (materializing pred in the slot).
	// Skipped when RCC_PRED_CACHE_PROBE is off: the counters do not advance (and this sum
	// pulls in one worker-written line per thread every round).
#if RCC_PRED_CACHE_PROBE
	{
		uint64_t rp = 0, pc = 0;
		for (int i = 0; i < V; i++) {
			rp += rcc_repoint_ctr[i].v;
			pc += rcc_predclear_ctr[i].v;
		}
		if (rp == rcc_last_repoint_sum) {
#if CC_ALG == RCC && RCC_ADD
			rcc_acc_cache_sound++;
			if (pc == rcc_last_predclear_sum)
				rcc_acc_cache_exact++;
#else
			INC_GLOB_STATS(rcc_round_cache_sound, 1);
			if (pc == rcc_last_predclear_sum)
				INC_GLOB_STATS(rcc_round_cache_exact, 1);
#endif
		}
		rcc_last_repoint_sum   = rp;
		rcc_last_predclear_sum = pc;
	}
#endif

	// --- Explicit-stack DFS: start from every node. Total node visits O(V+E). ---
#if CC_ALG == RCC && RCC_ADD
	for (int start = rcc_root_begin; start < rcc_root_end; start++) {
#else
	for (int start = 0; start < V; start++) {
#endif
#if CC_ALG == RCC && RCC_ADD && RCC_ADD_THREADS > 1
        if (rcc_color[start] == -1) rcc_snapshot_node(start);
#endif
		if (rcc_color[start] != 0) continue;
		if (snap_txnid[start] == RCC_NO_PRED) { rcc_color[start] = 2; continue; }
		int sp = 0;
		rcc_stack_node[sp] = start;
		rcc_stack_it[sp]   = 0;
		rcc_color[start]   = 1;   // GRAY
		while (sp >= 0) {
			int u  = rcc_stack_node[sp];
			int it = rcc_stack_it[sp];
			if (it >= snap_cnt[u]) {  // neighbors exhausted -> BLACK, pop
				rcc_color[u] = 2;
				sp--;
				continue;
			}
			rcc_stack_it[sp] = it + 1;
			uint64_t p = snap_preds[u*stride + it];
			if (p == RCC_NO_PRED) continue;
			int w = p % V;
			if (w == u) continue;                 // ignore self-edges
#if CC_ALG == RCC && RCC_ADD && RCC_ADD_THREADS > 1
            if (rcc_color[w] == -1) rcc_snapshot_node(w);
#endif
			if (snap_txnid[w] != p) continue;     // stale edge: the target slot no longer holds that txn
			if (rcc_color[w] == 1) {              // GRAY -> back-edge -> cycle
				rcc_handle_cycle(sp, w, caller_thd);
			} else if (rcc_color[w] == 0) {      // WHITE -> descend
				sp++;
				rcc_stack_node[sp] = w;
				rcc_stack_it[sp]   = 0;
				rcc_color[w]       = 1;
			}
			// BLACK(2) neighbor: subtree already finished -> ignore
		}
	}
}

#if CC_ALG == RCC
// [RCC_TRIGGER_DETECT] Helper that reads the entry list directly from the txn_man of pred(txnid)
// and derives its preds. Needing no slot/publish, it also walks the edges of members
// still running; once a cycle is complete, the first txn to enter commit-wait catches it immediately.
// Rechecking txn_id before and after filters out attempt reuse and torn reads.
static bool rcc_read_txn(uint64_t expect_txnid, int thd, int stride,
                         int * out_cnt, RCCLockEntry ** out_ent,
                         uint64_t * out_pred) {
	txn_man * t = glob_manager->get_txn_man(thd);
	if (!t) return false;
	if (t->get_txn_id() != expect_txnid) return false;      // committed/reused
	if ((t->commit_barriers & 3UL) == ABORTED) return false; // resolution in progress
	int c = t->row_cnt;
	if (c < 0) c = 0;
	if (c > stride) c = stride;
	for (int k = 0; k < c; k++) {
		Access * a = t->accesses[k];
		if (a == NULL || a->lock_entry == NULL) { c = k; break; }
		out_ent[k] = a->lock_entry;
	}
	COMPILER_BARRIER
	if (t->get_txn_id() != expect_txnid) return false;      // reused during the read
	*out_cnt = c;
	for (int k = 0; k < c; k++)
		out_pred[k] = rcc_entry_dep(out_ent[k]);
	return true;
}

// [RCC_TRIGGER_DETECT] Targeted DFS starting from the caller. Cycles not including the caller
// are ignored (their members catch them on their own entry/re-search).
bool DL_detect::rcc_walk_from(uint64_t my_txnid, uint64_t my_thd, int max_depth) {
	const int stride = MAX_ROW_PER_TXN + 1;
	// V <= 64 (visited bitmask). Path length <= V, so V frames suffice.
	int      st_node[64];
	int      st_it[64];
	uint64_t nd_txnid[64];
	int      nd_cnt[64];
	RCCLockEntry * nd_ent[64][MAX_ROW_PER_TXN + 1];
	uint64_t nd_pred[64][MAX_ROW_PER_TXN + 1];
	uint64_t visited = 0;

	int me = (int)(my_thd % (uint64_t)V);
	st_node[0] = me;
	st_it[0] = 0;
	nd_txnid[0] = my_txnid;
	if (!rcc_read_txn(my_txnid, me, stride, &nd_cnt[0], nd_ent[0], nd_pred[0]))
		return false;
	visited |= 1UL << me;

	int sp = 0;
	while (sp >= 0) {
		int it = st_it[sp];
		if (it >= nd_cnt[sp]) { sp--; continue; }
		st_it[sp] = it + 1;
		uint64_t p = nd_pred[sp][it];
		if (p == RCC_NO_PRED) continue;
		if (p == my_txnid) {
			// Back to the caller -> cycle candidate. [recheck] members live + re-derive each hop.
			// A real deadlock always passes, since all its edges are stable (even with members
			// running: a pred inside the cycle cannot change because its target cannot leave).
			bool ok = true;
			for (int i = 0; i <= sp && ok; i++) {
				txn_man * vt = glob_manager->get_txn_man(st_node[i]);
				if (!vt || vt->get_txn_id() != nd_txnid[i]) { ok = false; break; }
				uint64_t target = (i < sp) ? nd_txnid[i + 1] : my_txnid;
				bool holds = false;
				for (int k = 0; k < nd_cnt[i] && !holds; k++)
					holds = (rcc_entry_dep(nd_ent[i][k]) == target);
				if (!holds) ok = false;
			}
			if (ok) {
				INC_GLOB_STATS(deadlock, 1);
				INC_GLOB_STATS(rcc_confirmed, 1);
				uint64_t len = (uint64_t)(sp + 1);
				__sync_fetch_and_add(&stats.rcc_cycle_len_sum, len);
				if (len > stats.rcc_cycle_len_max)
					stats.rcc_cycle_len_max = len;   // approximation is enough
				return true;
			}
			INC_GLOB_STATS(rcc_false_positive, 1);
			continue;
		}
		int w = (int)(p % (uint64_t)V);
		if (visited & (1UL << w)) continue;   // cycle not including the caller / merge point
		int nf = sp + 1;
		if (nf >= max_depth || nf >= V || nf >= 64) continue;  // depth limit
		st_node[nf] = w;
		st_it[nf] = 0;
		nd_txnid[nf] = p;
		// [direct read] Reads the other txn_man's entries as-is, without publish/slot.
		// Running members pass too; once a cycle is complete, the first txn to enter catches it.
		if (!rcc_read_txn(p, w, stride, &nd_cnt[nf], nd_ent[nf], nd_pred[nf]))
			continue;                          // committed/reused/being resolved -> stale edge
		visited |= 1UL << w;
		sp = nf;
	}
	return false;
}
#endif  // CC_ALG == RCC

// Handles a cycle closed by a back-edge (u -> w, w is GRAY on the current DFS path).
// Frames [idx..sp] of the stack form the cycle (idx = frame position of w).
#if CC_ALG == RCC && RCC_ADD
#define RCC_CYCLE_STAT(name, value) do { if (STATS_ENABLE) rcc_cycle_stats.name += (value); } while (0)
#else
#define RCC_CYCLE_STAT(name, value) do { INC_GLOB_STATS(name, value); } while (0)
#endif
void DL_detect::rcc_handle_cycle(int sp, int w, uint64_t caller_thd) {
	// Find the frame position of w
	int idx = sp;
	while (idx >= 0 && rcc_stack_node[idx] != w) idx--;
	if (idx < 0) return;   // defensive (should not happen)
#if CC_ALG == RCC && RCC_ADD && RCC_ADD_THREADS > 1
    int canonical = rcc_stack_node[idx];
    for (int i = idx + 1; i <= sp; ++i)
        if (rcc_stack_node[i] < canonical) canonical = rcc_stack_node[i];
    uint32_t * guard = &rcc_cycle_guards[canonical].held;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(guard, &expected, 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        ++rcc_acc_guard_busy;
        return; // no spinning; both detectors keep scanning
    }
    struct ReleaseGuard {
        uint32_t * word;
        ~ReleaseGuard() { __atomic_store_n(word, 0U, __ATOMIC_RELEASE); }
    } release_guard{guard};
    // Dedup/status and live-edge revalidation below run AFTER taking the guard.
#endif

	// [dedup] If a member is already marked ABORTED, this cycle is being resolved
	// (edges remain only because the victim has not noticed yet). With early publish, a victim
	// is caught while running and needs ~1 statement (tens of µs) to clean up, during which 1µs-period
	// rounds re-detect the same cycle dozens of times; do not recount it or call set_abort again.
	for (int i = idx; i <= sp; i++) {
		int node = rcc_stack_node[i];
		txn_man * t = glob_manager->get_txn_man(node);
		if (t && t->get_txn_id() == snap_txnid[node] &&
		    (t->commit_barriers & 3UL) == ABORTED)
			return;
	}

	RCC_CYCLE_STAT(deadlock, 1);

	// --- Check 1: is every cycle member unchanged since the snapshot? ---
	// If any version/txnid changed, the cycle is already resolved -> discard as a false positive.
	// This revalidation is what lets the search phase run fully lock-free.
	for (int i = idx; i <= sp; i++) {
		int node = rcc_stack_node[i];
		RccSlot & s = rcc_slots[node];
		if (s.version != snap_version[node] || s.txnid != snap_txnid[node]) {
			RCC_CYCLE_STAT(rcc_false_positive, 1);
			return;
		}
	}
	// --- Check 2: re-derive the cycle edges (double-read) ---
	// [early publish] also puts running txns into the graph, so the blocker may have been released
	// between derivation and now. Re-derive each edge (u -> next) now
	// to confirm it still holds. Cycles are ~2-3 long, so the cost is negligible.
	const int stride = MAX_ROW_PER_TXN + 1;
	for (int i = idx; i <= sp; i++) {
		int u   = rcc_stack_node[i];
		int nxt = (i < sp) ? rcc_stack_node[i + 1] : rcc_stack_node[idx];
		uint64_t target = snap_txnid[nxt];
		bool holds = false;
		for (int k = 0; k < snap_cnt[u] && !holds; k++)
			holds = (rcc_entry_dep(snap_entries[u*stride + k]) == target);
		if (!holds) {
			RCC_CYCLE_STAT(rcc_false_positive, 1);
			return;
		}
	}

#if (RCC_PERSIST_ROUNDS > 1 || RCC_PERSIST_NS > 0) && !RCC_ADD
	// [disabled under RCC_ADD] A pred cycle is definite as soon as it forms, so
	// passing revalidation = immediate abort. A window would only add detection latency.
	// --- [persistence filter] Even after passing revalidation, do not abort right away ---
	// A cycle with the same member set is aborted only after it stays "consecutively observed" (1-round gap allowed)
	//   (a) for g_rcc_persist_ns of elapsed time and (b) for at least RCC_PERSIST_ROUNDS observations.
	// Transients (torn-snapshot phantoms, cycles soon broken by a third-party
	// abort) vanish before that and are filtered here. pred edges are stable, so a real
	// cycle is re-observed with the same signature every round.
	// Only consecutiveness is judged in rounds, because observations happen per round. For why
	// confirmation is time-based, see the RCC_PERSIST_NS comment in config-std.h.
	{
		uint64_t sig = 0, sum = 0;
		for (int i = idx; i <= sp; i++) {
			uint64_t id = snap_txnid[rcc_stack_node[i]];
			sig ^= id;
			sum += id;
		}
		sig = (sig * 0x9E3779B97F4A7C15UL) ^ sum;
		if (sig == 0) sig = 1;                    // 0 marks an empty slot
		int free_slot = -1;
		RccSuspect * hit = NULL;
		for (int i = 0; i < rcc_susp_cap; i++) {
			RccSuspect & s = rcc_susp[i];
			if (s.sig == sig) { hit = &s; break; }
			if (free_slot < 0 &&
			    (s.sig == 0 || s.last_round + 2 < rcc_round_no))
				free_slot = i;
		}
		if (hit) {
			if (hit->last_round + 2 >= rcc_round_no)
				hit->streak++;                    // consecutive observation (1-round gap allowed)
			else {
				INC_GLOB_STATS(rcc_persist_filtered, 1);  // broken episode = transient
				hit->streak = 1;
				hit->first_ns = rcc_round_ns;     // restart the time window too
			}
			hit->last_round = rcc_round_no;
			if (hit->streak < RCC_PERSIST_ROUNDS)
				return;                           // too few observations
			if (rcc_round_ns - hit->first_ns < g_rcc_persist_ns)
				return;                           // not enough time yet
			hit->sig = 0;                         // confirmed -> free the slot, proceed to abort
		} else if (free_slot >= 0) {
			RccSuspect & s = rcc_susp[free_slot];
			if (s.sig != 0)
				INC_GLOB_STATS(rcc_persist_filtered, 1);  // stale eviction = transient
			s.sig = sig;
			s.streak = 1;
			s.last_round = rcc_round_no;
			s.first_ns = rcc_round_ns;
			return;                               // first observation -> no abort yet
		}
		// Neither a hit nor a free slot (abnormal saturation): safely fall through to an immediate abort
	}
#endif

	RCC_CYCLE_STAT(rcc_confirmed, 1);
	// --- Confirmed cycle: record its length (for avg/max) ---
	uint64_t len = (uint64_t)(sp - idx + 1);
#if CC_ALG == RCC && RCC_ADD
    rcc_cycle_stats.rcc_cycle_len_sum += len;
    if (len > rcc_cycle_stats.rcc_cycle_len_max) rcc_cycle_stats.rcc_cycle_len_max = len;
#else
	stats.rcc_cycle_len_sum += len;
	if (len > stats.rcc_cycle_len_max) stats.rcc_cycle_len_max = len;
#endif

#if CC_ALG == RCC && RCC_ADD
    // [reader-victim] hop i: u = stack[i] depends on nxt = stack[i+1] through one of u's entries; if that
    // edge is non-dirty (u wrote after nxt read), nxt is a reader in this cycle. Abort only the readers;
    // an all-dirty cycle (WW/WR only) still aborts every member.
	uint64_t reader_mask[(THREAD_CNT + 63) / 64] = {0}; uint64_t reader_cnt = 0;   // one bit per worker thread, any thread count (was 128 bits: threads >= 128 aliased)
	for (int i = idx; i <= sp; i++) {
		int u = rcc_stack_node[i]; int nxt = (i < sp) ? rcc_stack_node[i + 1] : rcc_stack_node[idx];
		uint64_t target = snap_txnid[nxt];
		for (int k = 0; k < snap_cnt[u]; k++) {
			RCCLockEntry * e = snap_entries[u*stride + k];
			if (rcc_entry_dep(e) == target) { if (!e->pred_dirty && !(reader_mask[nxt >> 6] & (1UL << (nxt & 63)))) { reader_mask[nxt >> 6] |= 1UL << (nxt & 63); reader_cnt++; } break; }
		}
	}
	if (STATS_ENABLE) { if (reader_cnt) { rcc_cycle_stats.rv_cycles++; rcc_cycle_stats.rv_victims += reader_cnt; } else rcc_cycle_stats.rv_alldirty_members += (sp - idx + 1); }
	for (int i = idx; i <= sp; i++) {
		int node = rcc_stack_node[i];
		if (reader_cnt && !(reader_mask[node >> 6] & (1UL << (node & 63)))) continue;   // [reader-victim]
		txn_man * txn = glob_manager->get_txn_man(node);
		if (txn != NULL && txn->get_txn_id() == snap_txnid[node]) {
			// Classify where the victim was caught (best-effort: may race)
			int wc = txn->wait_ctx;
			if (wc == 1)      { RCC_CYCLE_STAT(rcc_dl_lockwait, 1); }
			else if (wc == 2) { RCC_CYCLE_STAT(rcc_dl_commitwait, 1); }
			else              { RCC_CYCLE_STAT(rcc_dl_running, 1); }
			txn->set_abort(ABORT_SRC_SELF, nullptr, snap_txnid[node]);
			if (STATS_ENABLE) ++rcc_cycle_stats.rcc_lock_deadlock_cnt;
		}
	}
#else
	// --- victim: one node on the cycle (w) suffices. Releasing it breaks the cycle, and
	//     cascade (remove_descendants) handles the rest entangled through dirty reads. ---
	uint64_t victim_txnid = snap_txnid[w];
	txn_man * txn = glob_manager->get_txn_man(w);   // w == victim_txnid % V
	if (txn != NULL && txn->get_txn_id() == victim_txnid) {
#if CC_ALG == RCC   // (BB_FAKE_DETECT has no edges and never gets here; wait_ctx is also RCC-only)
		// Classify where the victim was caught (best-effort: the victim is another thread, so this may race)
		int wc = txn->wait_ctx;
		if (wc == 1)      { RCC_CYCLE_STAT(rcc_dl_lockwait, 1); }
		else if (wc == 2) { RCC_CYCLE_STAT(rcc_dl_commitwait, 1); }
#endif
		txn->set_abort(ABORT_SRC_SELF, nullptr, victim_txnid);
		INC_STATS(caller_thd, rcc_lock_deadlock_cnt, 1);
	}
#endif
}
#endif  // CC_ALG == RCC
