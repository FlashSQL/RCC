#ifndef _DL_DETECT_
#define _DL_DETECT_

#include <limits.h>
#include <list>
#include <stdint.h>
#include "pthread.h"
#include "config.h"
#ifndef RCC_ADD_THREADS
#define RCC_ADD_THREADS 1
#endif
#if CC_ALG == RCC && RCC_ADD
static_assert(RCC_ADD_THREADS == 1 || RCC_ADD_THREADS == 2 ||
              RCC_ADD_THREADS == 4 || RCC_ADD_THREADS == 8,
              "RCC supports 1, 2, 4 or 8 dedicated detectors");
#if RCC_DETECT_EVENT && RCC_ADD_THREADS > 1
#error "Partitioned detectors use continuous scans; disable RCC_DETECT_EVENT"
#endif
#endif
//#include "global.h"
//#include "helper.h"

// The denpendency information per thread
struct DepThd {
    std::list<uint64_t> adj;    // Pointer to an array containing adjacency lists
	pthread_mutex_t lock; 
	volatile int64_t txnid; 				// -1 means invalid
	int num_locks;				// the # of locks that txn is currently holding
	char pad[2 * CL_SIZE - sizeof(int64_t) - sizeof(pthread_mutex_t) - sizeof(std::list<uint64_t>) - sizeof(int)];
};

#if CC_ALG == RCC || (CC_ALG == BAMBOO && BB_FAKE_DETECT)
// [BB_FAKE_DETECT] BAMBOO builds store a cast BBLockEntry* in the slot.
// The round only reads the lines through the pointer and derives no edges, so the type mismatch is harmless.
struct RCCLockEntry;
class Access;
// [RCC] Slot for the round-based detector. One per thread; each slot is written only by its owner.
// seqlock: odd version = write in progress, even = stable. No mutex.
// Publishes not edge values (blocker txnids) but the list of references to the lock entries
// the txn currently holds. The round derives edges at detection time from entry->pred (nearest conflicting
// predecessor, maintained under the row latch), so:
//   - the slot needs no update when a blocker commits (pred has already been repointed)
//   - a publish adds only the accesses appended since the last one (with RCC_ADD, after each get_row)
// entry/Access objects are statically allocated for the thread's lifetime, so the round can always dereference them.
struct RccSlot {
    volatile uint64_t version;
    volatile uint64_t txnid;               // (uint64_t)-1 == RCC_NO_PRED == empty
    volatile int      cnt;
    RCCLockEntry * volatile entries[MAX_ROW_PER_TXN + 1];
    // Pad each slot to a cache-line multiple (no false sharing between adjacent slots' versions).
    // The header is 24B including alignment padding, not 8+8+4 = 20. Computing it as 20 makes
    // sizeof a non-multiple of 64 (8264): each slot drifts by 8B and
    // the seqlock header of 1 in 8 slots straddles a cache-line boundary.
    char pad[CL_SIZE - ((3*sizeof(uint64_t)
                         + (MAX_ROW_PER_TXN+1)*sizeof(void*)) % CL_SIZE)];
};
static_assert(sizeof(RccSlot) % CL_SIZE == 0, "RccSlot must be cache-line multiple");
#endif

// shared data for a particular deadlock detection
struct DetectData {
	bool * visited;
	bool * recStack;
	bool loop;
	bool onloop;		// the current node is on the loop
	int loopstart;		// the starting point of the loop
	int min_lock_num; 	// the min lock num for txn in the loop
	uint64_t min_txnid; // the txnid that holds the min lock num
};

class DL_detect {
public:
	void init();
	// return values: 
	// 	0: no deadlocks
	//  1: deadlock exists
	int detect_cycle(uint64_t txnid);
	// txn1 (txn_id) dependes on txns (containing cnt txns)
	// return values:
	//	0: succeed.
	//	16: cannot get lock
	int add_dep(uint64_t txnid, uint64_t * txnids, int cnt, int num_locks);
	int set_dep(uint64_t txnid, uint64_t * txnids, int cnt, int num_locks); //[RCC]
	// remove all outbound dependencies for txnid.
	// will wait for the lock until acquired.
	void clear_dep(uint64_t txnid);

#if CC_ALG == RCC || (CC_ALG == BAMBOO && BB_FAKE_DETECT)
	// [RCC] Round-based detector. Separates publishing (lock-free) from searching (once per global period).
	void rcc_init(DL_detect * shared = nullptr, int detector_id = 0);
	// Publish the append-only access prefix of one transaction attempt.
	// Existing Access/lock_entry pointers stay fixed until rcc_clear().
	// scan_idx: how many accesses the caller has already offered (worker-local, reset with the slot).
	void rcc_publish(uint64_t txnid, Access * const * accesses, int cnt, int & scan_idx);
	// Clear the slot (at txn end = cleanup).
	void rcc_clear(uint64_t txnid);
	// Once the period has elapsed, only the one caller that wins busy by CAS runs the round; others get false.
	bool rcc_try_round(uint64_t caller_thd);
	// [RCC_TRIGGER_DETECT] A txn entering commit-wait (or a waiter notified of a pred
	// repoint) runs a targeted DFS along pred edges starting from itself. If it finds a path back to itself,
	// returns true after double-read revalidation (the caller self-aborts).
	// A branch stops at an unpublished slot, txnid mismatch, ABORTED member or slot over the retry limit.
	bool rcc_walk_from(uint64_t my_txnid, uint64_t my_thd, int max_depth);
	// [measurement: pred cache feasibility] The round currently derives every edge from the entry
	// (rcc_entry_dep: a different cache line per entry). Caching pred in the slot at publish time
	// would let the round read only contiguous memory, but that requires knowing whether the cache
	// is still valid. Two events invalidate the cache, with different consequences:
	//
	//   repoint   (rm_from_retired) : pred is repointed to a different target.
	//               A stale cache points to a txn that has left, and the DFS drops that edge
	//               -> false NEGATIVE (a real cycle is missed). Never acceptable.
	//               A committing txn always leaves as cohead, so commits never hit this. Occurs only
	//               when an SH entry aborts independently (an EX abort cascades, so it is irrelevant).
	//   predclear (DEC_BARRIER)     : pred disappears on promotion to cohead.
	//               A stale cache claims a nonexistent edge -> false POSITIVE.
	//               Acceptable, since the existing cycle revalidation catches it.
	//
	// Hence the two are counted separately. Without repoint the cache is sound;
	// without either, the cache matches the truth exactly. If the former ratio is high, the cache
	// approach is viable; if low, the cache would mostly fall back to re-derivation.
	//
	// Many threads write these counters, so they are split into per-thread lines to avoid false sharing.
	// The round reads only the sum to tell whether it changed since the last round, so slight lag is harmless.
	// (helper.h's ATOM_ADD is not used: dl_detect.h does not include helper.h,
	//  and the reverse include would be circular. The same builtin is called directly.)
	inline void rcc_note_repoint(uint64_t thd)   { __sync_fetch_and_add(&rcc_repoint_ctr[thd % V].v, 1); }
	inline void rcc_note_predclear(uint64_t thd) { __sync_fetch_and_add(&rcc_predclear_ctr[thd % V].v, 1); }
#if CC_ALG == RCC && RCC_ADD
	// [RCC_ADD] Dedicated detector thread. Event mode checks on change notifications and
	// a timer; RCC_DETECT_EVENT=false checks continuously.
	void rcc_detector_loop();
	void rcc_detector_halt();
	// Main thread calls after joining all detectors; no concurrent stats flush.
	void rcc_detector_collect();
#if RCC_DETECT_EVENT
    // Coalesced graph-change event. Producers never take a detector mutex.
    void rcc_request_check();
#endif
#endif
#endif

private:
	int V;    // No. of vertices
	DepThd * dependency;

#if CC_ALG == RCC || (CC_ALG == BAMBOO && BB_FAKE_DETECT)
	RccSlot *         rcc_slots;              // [V]
	// [layout] V / rcc_slots above are read-only pointers that workers read on every
	// publish/clear. The fields below are state the detector thread updates every round;
	// sharing their line would make every worker publish take a coherence miss.
	char              pad_ro[CL_SIZE - ((sizeof(int) + 2 * sizeof(void *)) % CL_SIZE)];
	volatile uint64_t rcc_round_busy;         // TTAS: 0=idle, 1=one thread is running a round
	volatile uint64_t rcc_last_round_end;     // end time of the last round (ns)
	// Round scratch space (one shared copy: only the busy holder uses it)
	uint64_t *        snap_txnid;             // [V]
	uint64_t *        snap_version;           // [V]
	int *             snap_cnt;               // [V]
	RCCLockEntry **   snap_entries;           // [V * (MAX_ROW_PER_TXN+1)] published references
	uint64_t *        snap_preds;             // [V * (MAX_ROW_PER_TXN+1)] derived edges
	char *            rcc_color;              // [V] 0=WHITE 1=GRAY 2=BLACK
	int *             rcc_stack_node;         // [V] DFS frame: node
	int *             rcc_stack_it;           // [V] DFS frame: next neighbor index
	// [persistence filter] Cycles awaiting confirmation. Only the round runner accesses them, so no lock.
	struct RccSuspect {
		uint64_t sig;         // hash of the member txnid set (0 = empty slot)
		uint64_t last_round;  // round number of the last observation (for the consecutiveness check)
		uint64_t first_ns;    // time consecutive observation began (for the elapsed-time check)
		uint32_t streak;      // number of consecutive observations
	};
	RccSuspect *      rcc_susp;               // [2V]
	int               rcc_susp_cap;
	uint64_t          rcc_round_no;           // round number (incremented only by the runner)
	uint64_t          rcc_round_ns;           // start time of the current round (measured once per round)
	// [measurement: pred cache feasibility] See the rcc_note_* comment above.
	struct RccEpochCtr { volatile uint64_t v; char pad[CL_SIZE - sizeof(uint64_t)]; };
	RccEpochCtr *     rcc_repoint_ctr;        // [V] pred repoint (source of false negatives)
	RccEpochCtr *     rcc_predclear_ctr;      // [V] cohead promotion (source of false positives)
	uint64_t          rcc_last_repoint_sum;   // sum seen in the previous round (runner only)
	uint64_t          rcc_last_predclear_sum; // sum seen in the previous round (runner only)
#if CC_ALG == RCC && RCC_ADD
	int rcc_detector_id, rcc_root_begin, rcc_root_end;
	struct RccCycleGuard { uint32_t held; char pad[64 - sizeof(uint32_t)]; };
	RccCycleGuard * rcc_cycle_guards; // shared; indexed by smallest cycle slot
	struct RccCycleStats {
		uint64_t deadlock, rcc_false_positive, rcc_confirmed;
		uint64_t rcc_cycle_len_sum, rcc_cycle_len_max;
		uint64_t rcc_dl_lockwait, rcc_dl_commitwait, rcc_dl_running;
		uint64_t rcc_lock_deadlock_cnt;
		uint64_t rv_cycles; uint64_t rv_victims; uint64_t rv_alldirty_members;   // [reader-victim]
	} rcc_cycle_stats;
	uint64_t rcc_acc_snapshot_nodes, rcc_acc_cross_nodes, rcc_acc_guard_busy;
	volatile bool     rcc_detector_stop;
#if RCC_DETECT_EVENT
    // 0=consumed/running, 1=pending, 2=armed for futex sleep.
    alignas(64) uint32_t rcc_event;
    char rcc_event_padding[64 - sizeof(uint32_t)]; // no detector-local counter shares the producer line
    uint64_t rcc_event_waits, rcc_event_timeouts, rcc_event_idle_ns;
#endif
	// [RCC_ADD] A per-round INC_GLOB_STATS invalidates the global stats cache line
	// tens of millions of times per second, turning every worker INC_STATS into a remote miss
	// (measured -20% at 1 thread without contention; 51% of lock_get samples were stats loads).
	// Accumulate locally in the dedicated thread and flush once when the loop exits.
	uint64_t          rcc_acc_rounds;
	uint64_t          rcc_acc_detect_time;
	uint64_t          rcc_acc_derive_time;
	uint64_t          rcc_acc_cache_sound;
	uint64_t          rcc_acc_cache_exact;
#endif
	void rcc_run_round(uint64_t caller_thd);
	void rcc_snapshot_node(int node);
	void rcc_handle_cycle(int sp, int w, uint64_t caller_thd);
#endif
	
	///////////////////////////////////////////
	// For deadlock detection
	///////////////////////////////////////////
	// dl_lock is the global lock. Only used when deadlock detection happens
	pthread_mutex_t _lock;
	// return value: whether a loop is detected.
	bool nextNode(uint64_t txnid, DetectData * detect_data);
	bool isCyclic(uint64_t txnid, DetectData * detect_data); // return if "thd" is causing a cycle
};

#endif
