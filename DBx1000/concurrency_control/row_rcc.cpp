#include "row.h"
#include "txn.h"
#include "row_rcc.h"
#include "mem_alloc.h"
#include "manager.h"
#if RCC_ERW_ON
// [ERW] conflict edges of the current access, found under the row latch and logged right after it
static thread_local std::vector<std::pair<uint64_t, uint32_t>> rs_edges;
#if RCC_ERW_EDGE_SEND
// [ERW] edges not sent yet (RCC_ERW_EDGE_SEND): sending them right after a write grant kept the row owned while writing
// the detector's log (~0.4 us on hot rows)
static thread_local std::vector<std::pair<uint64_t, uint32_t>> rs_pending;
void rccs_flush_edges(txn_man * txn) {
    const uint64_t me = txn->get_txn_id();
    (void) me;
    for (const auto &e : rs_pending) txn->rs_record(e.first, e.second);
    rs_pending.clear();
}
void rccs_drop_edges() { rs_pending.clear(); }
#endif
#endif
#if CC_ALG == RCC && RCC_ERW_PROBE
// [measurement only] cycles per table: critical sections and the parts inside them
#include <x86intrin.h>
#include "table.h"
enum { RP_GET, RP_GETN, RP_EDGE, RP_REL, RP_RELN, RP_UPD, RP_INST, RP_RSW, RP_RET, RP_RETN, RP_GETW, RP_RELW, RP_RETW, RP_OWN, RP_OWNN, RP_LOG, RP_N };
static thread_local uint64_t rp_own_t0;   // this thread's last owner grant (a transaction owns at most one row at a time)
// per thread, transaction level: [0] ns from barrier 0 to commit while waiting for approval, [1] commits that waited so,
// [2] commits that asked for approval, [3] commits
uint64_t rp_txn[THREAD_CNT + 1][8];
struct alignas(64) RpThd { uint64_t v[16][RP_N]; };
static RpThd rp_thd[THREAD_CNT + 1];
static table_t * volatile rp_tab[16];
static inline int rp_idx(table_t * t) {
    for (int i = 0; i < 15; i++) {
        table_t * c = rp_tab[i];
        if (c == t) return i;
        if (!c && (__sync_bool_compare_and_swap(&rp_tab[i], (table_t *) NULL, t) || rp_tab[i] == t)) return i;
    }
    return 15;
}
#define RP_T(v) const uint64_t v = __rdtsc();
#define RP_ADD(thd, m, t0) { rp_thd[thd].v[rp_idx(_row->get_table())][m] += __rdtsc() - (t0); }
#define RP_INC(thd, m) { rp_thd[thd].v[rp_idx(_row->get_table())][m]++; }
void rcc_probe_print() {
    const double f = CPU_FREQ;   // cycles per ns
    for (int i = 0; i < 16; i++) {
        if (!rp_tab[i] && i != 15) continue;
        uint64_t s[RP_N] = {0};
        for (int t = 0; t < THREAD_CNT; t++) for (int m = 0; m < RP_N; m++) s[m] += rp_thd[t].v[i][m];
        if (!s[RP_GETN] && !s[RP_RELN]) continue;
        const char * name = (i == 15 || !rp_tab[i]) ? "other" : rp_tab[i]->get_table_name().c_str();
        auto per = [&](int m, int n) { return s[n] ? s[m] / f / s[n] : 0.0; };
        printf("[rcc_probe] table=%-12s get_n=%lu get_wait_ns=%.1f get_cs_ns=%.1f edge_ns=%.1f | rel_n=%lu rel_wait_ns=%.1f rel_cs_ns=%.1f upd_ns=%.1f install_ns=%.1f rsleft_ns=%.1f | retire_n=%lu retire_wait_ns=%.1f retire_cs_ns=%.1f\n",
               name, s[RP_GETN], per(RP_GETW, RP_GETN), per(RP_GET, RP_GETN), per(RP_EDGE, RP_GETN), s[RP_RELN], per(RP_RELW, RP_RELN), per(RP_REL, RP_RELN), per(RP_UPD, RP_RELN),
               per(RP_INST, RP_RELN), per(RP_RSW, RP_RELN), s[RP_RETN], per(RP_RETW, RP_RETN), per(RP_RET, RP_RETN));
        printf("[rcc_probe] table=%-12s owner_n=%lu owner_hold_ns=%.1f (grant -> retire asks for the latch) edge_log_ns_per_get=%.1f\n",
               name, s[RP_OWNN], per(RP_OWN, RP_OWNN), per(RP_LOG, RP_GETN));
    }
    uint64_t t[8] = {0};
    for (int i = 0; i < THREAD_CNT; i++) for (int m = 0; m < 8; m++) t[m] += rp_txn[i][m];
    printf("[rcc_probe] commits=%lu asked_approval=%lu approval_after_barrier0=%lu approval_after_barrier0_avg_ns=%.1f\n",
           t[3], t[2], t[1], t[1] ? (double) t[0] / t[1] : 0.0);
    // totals in ns: [4] write edges sent after retire (Execution), [5] edges flushed + approval asked before the commit
    // spin (inside time_commit, committed attempts), [6] commit reported to the detector in cleanup (Execution, committed)
    printf("[rcc_probe] retire_flush_ns_total=%lu commit_request_ns_total=%lu commit_log_ns_total=%lu\n", t[4], t[5], t[6]);
}
#else
#define RP_T(v)
#define RP_ADD(thd, m, t0) {}
#define RP_INC(thd, m) {}
#endif
#if RCCB
// [RCC_BATCH] the entries one sweep removes
static thread_local std::vector<RCCLockEntry *> rb_sweep;
#endif


#if RCC_PW
thread_local uint64_t rc_pw_holder = 0;
thread_local bool rc_pw_waited = false;
#endif

void Row_rcc::init(row_t * row) {
    _row = row;
    // owners contains at most one lock entry, whose type is always LOCK_EX
    owners = NULL;
    // waiter is a doubly linked list
    waiters_head = NULL;
    waiters_tail = NULL;
    // retired is a doubly linked list
    retired_head = NULL;
    retired_tail = NULL;
    waiter_cnt = 0;
    retired_cnt = 0;
    clean_seq = (uint64_t)-1;
    cur_head_id  = RCC_NO_PRED;  // [RCC] pred normalization
    cur_owner_id = RCC_NO_PRED;
    vs_grant_seq = 0;
    vs_published = 0;
#if RCC_ERW_ON
    rs_last_wr = 0; rs_has_wr = false; rs_last_clean = false;
    rs_left_id = NULL; rs_left_seq = NULL; rs_left_n = 0; rs_left_cap = 0;
#endif
    // init latches. Each latch gets a cache line of its own: a 4-byte `new` put the latches of neighbouring rows
    // (the warehouse and the districts are created one after another) into one line. RCC_BATCH's committed-entry
    // counter shares it: its owner touches the counter and then try_locks the latch.
    char * lline = (char *) _mm_malloc(64, 64);
    static_assert(sizeof(pthread_mutex_t) <= 48 && sizeof(mcslock) <= 48, "latch and counter must fit one line");
#if RCCB
    rb_crel = (volatile uint32_t *) (lline + 48);
    *rb_crel = 0;
#endif
#if LATCH == LH_SPINLOCK
    latch = new (lline) pthread_spinlock_t;
    pthread_spin_init(latch, PTHREAD_PROCESS_SHARED);
#elif LATCH == LH_MUTEX
    latch = new (lline) pthread_mutex_t;
    pthread_mutex_init(latch, NULL);
#else
    latch = new (lline) mcslock();
#endif
    blatch = false;
}

// taking the latch
void Row_rcc::lock(txn_man * txn) {
#if RCC_PW
    // as without BB_PF_PW (pthread spinlock), noting only whether it had to wait: the caller logs the wait from the
    // PF_CS timestamps it takes anyway (RC_PW_LATCH)
    rc_pw_waited = false;
    if (likely(g_thread_cnt > 1) && likely(!g_central_man)) { if (pthread_spin_trylock(latch) != 0) { pthread_spin_lock(latch); rc_pw_waited = true; } }
    else if (likely(g_thread_cnt > 1)) glob_manager->lock_row(_row);
    rc_pw_holder = txn->get_txn_id();
    return;
#endif
    if (likely(g_thread_cnt > 1)) {
            if (unlikely(g_central_man))
                glob_manager->lock_row(_row);
            else {
#if LATCH == LH_SPINLOCK
               pthread_spin_lock( latch );
#elif LATCH == LH_MUTEX
                pthread_mutex_lock( latch );
#else
                latch->acquire(txn->mcs_node);
#endif
            }
    }
};


// release the latch
void Row_rcc::unlock(txn_man * txn) {
#if RCC_PW
    rc_pw_holder = 0;
    if (likely(g_thread_cnt > 1)) {
        if (unlikely(g_central_man)) glob_manager->release_row(_row);
        else pthread_spin_unlock(latch);
    }
    return;
#endif
#if RCCB
    latch_release(txn);
    // [RCC_BATCH] an owner that marked an entry committed while this holder had the latch left the sweep to it: look
    // again after the release (the owner stores before its try_lock, this holder loads after its release + fence)
    for (;;) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(rb_crel, __ATOMIC_ACQUIRE) == 0) return;
        if (!try_lock(txn)) return;   // the new holder looks after its own release
        const uint32_t n = sweep_committed();
        latch_release(txn);
        if (n == 0) return;           // the rest cannot leave yet (behind a writer not marked yet); its owner sweeps
    }
}

void Row_rcc::latch_release(txn_man * txn) {
#endif
        if (likely(g_thread_cnt > 1)) {
            if (unlikely(g_central_man))
                glob_manager->release_row(_row);
            else {
#if LATCH == LH_SPINLOCK
                pthread_spin_unlock( latch );
#elif LATCH == LH_MUTEX
                pthread_mutex_unlock( latch );
#else
                latch->release(txn->mcs_node);
#endif
            }
        }
};

#if RCCB
bool Row_rcc::try_lock(txn_man * txn) {
    if (unlikely(g_thread_cnt <= 1)) return true;
    if (unlikely(g_central_man)) { glob_manager->lock_row(_row); return true; }
#if LATCH == LH_SPINLOCK
    return pthread_spin_trylock(latch) == 0;
#elif LATCH == LH_MUTEX
    return pthread_mutex_trylock(latch) == 0;
#else
    return latch->try_acquire(txn->mcs_node);
#endif
}

// [RCC_BATCH] under the latch: remove the committed entries that may leave, and install the newest committed version
// once. From the head, every committed entry leaves (an uncommitted reader in front stays, as a committed writer leaves
// past it in ERW) until the first writer not marked committed: the entries behind it must stay, or that older
// version would be the next joiner's source after a newer one was installed. The owner leaves too if nothing stopped
// the walk. Returns the number removed.
uint32_t Row_rcc::sweep_committed(bool wake) {
    if (__atomic_load_n(rb_crel, __ATOMIC_ACQUIRE) == 0) return 0;
    rb_sweep.clear();
    RCCLockEntry * newest = NULL;
    bool stopped = false;
    for (RCCLockEntry * en = retired_head; en; en = en->next) {
        if (__atomic_load_n(&en->crel, __ATOMIC_ACQUIRE)) {
            rb_sweep.push_back(en);
            if (en->type == LOCK_EX) newest = en;
        } else if (en->type == LOCK_EX) {
            stopped = true;
            break;
        }
    }
    RCCLockEntry * own = (!stopped && owners && __atomic_load_n(&owners->crel, __ATOMIC_ACQUIRE)) ? owners : NULL;
    if (own) newest = own;
    if (rb_sweep.empty() && !own) return 0;
    // one install for the whole batch; the readers leave after it, so each is kept (rs_left) only if it read this one
    if (newest && newest->vs_seq > vs_published) {
        _row->copy(newest->access->data);
        vs_published = newest->vs_seq;
        rs_last_wr = newest->q_id; rs_has_wr = true; rs_last_clean = newest->rs_clean; rs_left_prune();
    }
    for (RCCLockEntry * en : rb_sweep) {
        if (en->type == LOCK_SH && !en->rs_clean) rs_left_push(en->q_id, en->rd_seq);
        LIST_RM(retired_head, retired_tail, en, retired_cnt);
        return_entry(en);
    }
    uint32_t n = (uint32_t) rb_sweep.size();
    if (own) {
        owners = NULL;
        return_entry(own);
        n++;
        if (wake) bring_next(NULL);
    }
    __atomic_fetch_sub(rb_crel, n, __ATOMIC_ACQ_REL);
    RCC_SYNC_IDS()
    return n;
}

bool Row_rcc::lock_release_async(RCCLockEntry * entry, txn_man * txn) {
    const lock_status st = entry->status;
    if (st != LOCK_RETIRED && st != LOCK_OWNER) {   // not in the queue (the synchronous path handles a pending wake)
        lock_release(entry, RCOK, txn);
        return true;
    }
    __atomic_fetch_add(rb_crel, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&entry->crel, true, __ATOMIC_SEQ_CST);
    if (try_lock(txn)) {
        sweep_committed();
        unlock(txn);
    }
    return __atomic_load_n(&entry->status, __ATOMIC_ACQUIRE) == LOCK_DROPPED;
}
#endif

#if RCC_DEBUG_STUCK
void Row_rcc::stuck_dump(txn_man * me) {
    lock(me);
    auto pr = [&](const char * where, RCCLockEntry * e) {
        txn_man * t = e->txn;
        const uint64_t b = t->commit_barriers;
        printf("    %-7s%s q_id=%lu now_id=%lu %s st=%d cohead=%d pred=%ld rd_seq=%lu vs_seq=%lu "
#if RCCB
               "crel=%d "
#endif
               "| owner bar=%lu stat=%lu wait_ctx=%d\n",
               where, t == me ? "*" : " ", (uint64_t) e->q_id, (uint64_t) t->get_txn_id(), e->type == LOCK_EX ? "W" : "R",
               (int) e->status, (int) e->is_cohead, e->pred == RCC_NO_PRED ? -1L : (long) e->pred, e->rd_seq, e->vs_seq,
#if RCCB
               (int) e->crel,
#endif
               b >> 2, b & 3UL, t->wait_ctx);
    };
    printf("  [row %p] retired_cnt=%u waiter_cnt=%u vs_published=%lu clean_seq=%lu\n", (void *) _row, retired_cnt,
           waiter_cnt, vs_published, clean_seq);
    for (RCCLockEntry * e = retired_head; e; e = e->next) pr("retired", e);
    if (owners) pr("owner", owners);
    int i = 0;
    for (RCCLockEntry * e = waiters_head; e && i < 4; e = e->next, i++) pr("waiter", e);
    unlock(me);
}
#endif

// Return value: RCOK/FINISH/WAIT/Abort
// - RCOK: acquired the lock and should read current data
// - FINISH: acquired the lock and have read previous copy
// - WAIT: added to wait list but not acquired the data
// - Abort: aborted
RC Row_rcc::lock_get(lock_t type, txn_man * txn, Access * access) {
    // init return value
    RC rc = RCOK;
#if RCC_ERW_ON
    rs_edges.clear();
#endif
    // initialize a lock entry
    RCCLockEntry * to_insert = get_entry(access);
    to_insert->type = type;
#if PF_MODEL
    INC_STATS(txn->get_thd_id(), lock_acquire_cnt, 1);
#endif
#if PF_CS
    uint64_t starttime = get_sys_clock();
#endif
    // take the latch
    RP_T(rp_gw0)
    lock(txn);
    COMPILER_BARRIER
    RP_T(rp_get0)
    RP_ADD(txn->get_thd_id(), RP_GETW, rp_gw0)
#if PF_CS
    uint64_t endtime = get_sys_clock();
    INC_TMP_STATS(txn->get_thd_id(), time_get_latch, endtime - starttime);
    RC_PW_LATCH(starttime, endtime)
    starttime = endtime;
#endif
#if RCCB
    sweep_committed();
    txn_man * rb_pred = NULL;   // the uncommitted writer this access waits for: register with it after the latch
    uint32_t rb_epoch = 0;
#endif

#if !RCC_ODH
    { RC_PH_BEGIN_AT(rc_ph, RC_PH_PREV, endtime)   // prevent() starts as the latch is taken
    prevent(type, txn);   // [!RCC_ODH] Bamboo's timestamps + wounds, under the latch
      RC_PH_END(rc_ph, BB_PW_CS) }
#endif
    if (owners && to_insert->woken) { to_insert->woken = false; }
#if !RCC_ODH
    // [!RCC_ODH] as Bamboo: a request queues behind the waiters (in timestamp order) and the queue is granted in
    // place (bring_next), so nobody can join between a waiter's wake-up and its grant
    if (owners || waiters_head) {
#else
    if (owners) {
#endif
        // [pred edge] A waiter creates no edge (see the rcc_entry_dep comment:
        // under the immediate-retire discipline a lock-wait deadlock cannot occur; the backstop is insurance).
        RCC_ADD_WAITER(to_insert);
        rc = WAIT;
#if !RCC_ODH
        if (!owners && bring_next(txn)) rc = to_insert->vs_copied ? FINISH : RCOK;   // granted in place
#endif
    }
    else {
        RCCLockEntry *vprev, *vsrc;
        if (!select_grant(vprev, vsrc)) {
            // [join block] If retired holds a writer marked ABORTED, joining now is
            // certainly wasted work; wait in waiters for the cleanup (bring_next wakes it).
            INC_STATS(txn->get_thd_id(), rcc_join_block_cnt, 1);
            RCC_ADD_WAITER(to_insert);
            rc = WAIT;
        } else {
#if PF_MODEL
            INC_STATS(txn->get_thd_id(), lock_directly_cnt, 1);
#endif
#if RCC_ERW_ON
            // [ERW] this access's conflict edges: wr / ww from the version it reads (the nearest writer ahead, or the
            // writer of the published version), and for a writer rw from every reader of the version it overwrites
            // (readers still queued after that writer, or committed readers of the published version that left)
            {
                RC_PH_BEGIN(rc_ph, RC_PH_DET)
                RP_T(rp_e0)
                const uint64_t rseq = vsrc ? vsrc->vs_seq : vs_published;
                to_insert->rd_seq = rseq;
                const uint32_t dep = type == LOCK_SH ? RS_WR : RS_WW;
                // no edge from an attempt that committed without incoming edges (rs_clean): it cannot be on a cycle
                if (vsrc) { if (!vsrc->rs_clean) rs_edges.push_back({vsrc->q_id, dep}); }
                else if (rs_has_wr && !rs_last_clean && !rcc_srdet.gone(rs_last_wr)) rs_edges.push_back({rs_last_wr, dep});
                if (type == LOCK_EX) {
                    for (RCCLockEntry *p = retired_tail; p && p != vsrc; p = p->prev)
                        if (p->type == LOCK_SH && p->rd_seq == rseq && !vs_dead(p) && !p->rs_clean)
                            rs_edges.push_back({p->q_id, RS_RW});
                    if (!vsrc)
                        for (uint32_t i = 0; i < rs_left_n; i++)
                            if (rs_left_seq[i] == rseq && !rcc_srdet.gone(rs_left_id[i]))
                                rs_edges.push_back({rs_left_id[i], RS_RW});
                }
                RP_ADD(txn->get_thd_id(), RP_EDGE, rp_e0)
                RC_PH_END(rc_ph, BB_PW_DCS)
            }
#endif
            VS_GRANT(to_insert, vprev, vsrc);
#if RCCB
            if (!to_insert->is_cohead) { rb_pred = vsrc->txn; rb_epoch = (uint32_t) vsrc->q_id; }
#endif
            txn->lock_ready = true;
            if (type == LOCK_EX) {
                owners = to_insert;
                owners->status = LOCK_OWNER;
#if CC_ALG == RCC && RCC_ERW_PROBE
                rp_own_t0 = __rdtsc();
#endif
            } else {
                ADD_TO_RETIRED_TAIL(to_insert);
            }
            if (to_insert->vs_copied)
                rc = FINISH; // the caller must read access->data (the snapshot), not the base
            if (to_insert->woken) { to_insert->woken = false; bring_next(NULL); }
#if !RCC_ODH
            else if (!owners && waiters_head) bring_next(NULL);   // a wounded owner left: its waiters may go now
#endif
        }
    }
    
    RCC_SYNC_IDS()
#if PF_CS
    INC_TMP_STATS(txn->get_thd_id(), time_get_cs, get_sys_clock() - starttime);
#endif
    // release the latch
    COMPILER_BARRIER
    RP_ADD(txn->get_thd_id(), RP_GET, rp_get0)
    RP_INC(txn->get_thd_id(), RP_GETN)
    unlock(txn);
#if RCCB
    if (rb_pred) {
        // [RCC_BATCH dep push] the barrier taken at the join is returned by that writer's commit; if it already sealed
        // its list, it committed (return the barrier here) or aborted (its row cleanup marks this attempt too)
        const int r = txn->rb_register(rb_pred, rb_epoch);
        if (r == txn_man::RB_REG_ENDED) txn->decrement_commit_barriers();
        else if (r == txn_man::RB_REG_ABORTED) txn->set_abort(ABORT_SRC_CASCADE);
        else if (r == txn_man::RB_REG_FULL) { __atomic_fetch_add(&rcc_srdet.rb_full, 1, __ATOMIC_RELAXED); txn->set_abort(ABORT_SRC_SELF); }
    }
#endif
#if RCC_ERW_ON
    RP_T(rp_log0)
#if RCC_PW
    const uint64_t rc_log0 = rs_edges.empty() ? 0 : get_sys_clock();
#endif
    if (!rs_edges.empty()) {
        const uint64_t me = txn->get_txn_id();
        for (const auto &e : rs_edges)
#if RCC_ERW_EDGE_SEND == 1
            if (e.first != me) {
                if (type == LOCK_EX && rc != WAIT) rs_pending.push_back(e);   // sent after the retire
                else txn->rs_record(e.first, e.second);
                txn->rccs_nedges++;
            }
#elif RCC_ERW_EDGE_SEND == 2
            if (e.first != me) { rs_pending.push_back(e); txn->rccs_nedges++; }
#else
            if (e.first != me) { txn->rs_record(e.first, e.second); txn->rccs_nedges++; }
#endif
        rs_edges.clear();
    }
#if RCC_PW
    if (rc_log0) bb_pw_iv(rc_log0, get_sys_clock(), BB_PW_DCS);   // own detection work: sending the edges
#endif
    RP_ADD(txn->get_thd_id(), RP_LOG, rp_log0)
#endif
    return rc;
}

RC Row_rcc::lock_retire(RCCLockEntry * entry) {
    ASSERT(entry->type == LOCK_EX);
#if PF_CS
    uint64_t starttime = get_sys_clock();
#endif
    RP_T(rp_rw0)
#if CC_ALG == RCC && RCC_ERW_PROBE
    if (rp_own_t0) { rp_thd[entry->txn->get_thd_id()].v[rp_idx(_row->get_table())][RP_OWN] += rp_rw0 - rp_own_t0; RP_INC(entry->txn->get_thd_id(), RP_OWNN); rp_own_t0 = 0; }
#endif
    lock(entry->txn);
    COMPILER_BARRIER
    RP_T(rp_ret0)
    RP_ADD(entry->txn->get_thd_id(), RP_RETW, rp_rw0)
#if PF_CS
    uint64_t endtime = get_sys_clock();
    INC_TMP_STATS(entry->txn->get_thd_id(), time_retire_latch, endtime - starttime);
    RC_PW_LATCH(starttime, endtime)
    starttime = endtime;
#endif
#if RCCB
    sweep_committed();
#endif
    RC rc = RCOK;
    // 1. find entry in owner and remove
    if (entry->status == LOCK_OWNER) {
        // move to retired list
        RETIRE_ENTRY(entry);
#if !RCC_LUI
        entry->access->orig_row->copy(entry->access->data);   // [!RCC_LUI] Bamboo: the write goes into the row at the retire
#endif
    } else {
        // may be is aborted: assert(txn->status == ABORTED);
        assert(entry->status == LOCK_DROPPED);
        rc = Abort;
    }
    bring_next(NULL);
    RCC_SYNC_IDS()

#if PF_CS
    INC_TMP_STATS(entry->txn->get_thd_id(), time_retire_cs, get_sys_clock() - starttime);
#endif
    COMPILER_BARRIER
    RP_ADD(entry->txn->get_thd_id(), RP_RET, rp_ret0)
    RP_INC(entry->txn->get_thd_id(), RP_RETN)
    unlock(entry->txn);
#if RCC_ERW_ON && RCC_ERW_EDGE_SEND == 1
#if RCC_ERW_PROBE || RCC_PW
    const uint64_t rp_fl0 = get_sys_clock();
#endif
    rccs_flush_edges(entry->txn);   // the write's edges, now that the row is no longer owned
#if RCC_PW
    bb_pw_iv(rp_fl0, get_sys_clock(), BB_PW_DCS);
#endif
#if RCC_ERW_PROBE
    rp_txn[entry->txn->get_thd_id()][4] += get_sys_clock() - rp_fl0;
#endif
#endif
    return rc;
}


RC Row_rcc::lock_release(RCCLockEntry * entry, RC rc, txn_man * executor) {
    if (!executor) executor = entry->txn;
    // Helpers only process irrevocably committed rows; abort stays owner-only.
    assert(executor == entry->txn || rc == RCOK);
	if (
        entry->status
        == LOCK_DROPPED) {
#if RCCB
	    if (entry->woken) { entry->woken = false; RC_LOCK_LOGGED(executor) sweep_committed(false); if (!owners) bring_next(NULL); unlock(executor); }
#else
	    if (entry->woken) { entry->woken = false; RC_LOCK_LOGGED(executor) if (!owners) bring_next(NULL); unlock(executor); }
#endif
	    return RCOK;
	}
#if PF_ABORT
    if (rc == Abort) entry->txn->abort_chain = 0;
#endif
#if PF_CS
    uint64_t starttime = get_sys_clock();
#endif
    RP_T(rp_lw0)
    lock(executor);
    COMPILER_BARRIER
    RP_T(rp_rel0)
    RP_ADD(executor->get_thd_id(), RP_RELW, rp_lw0)
#if PF_CS
    uint64_t endtime = get_sys_clock();
    PROF_CS(executor->get_thd_id(), release_latch, endtime- starttime);
    RC_PW_LATCH(starttime, endtime)
    starttime = endtime;
#endif
    // if in retired
    if (entry->status == LOCK_RETIRED) {
        rm_from_retired(entry, rc == Abort, executor);
    } else if (entry->status == LOCK_OWNER) {
        owners = NULL;
        // not found in retired, need to make globally visible if rc = commit
        if (rc == RCOK && (entry->type == LOCK_EX)) {
            // [version split] Commit publication (counterpart of tpl_apply). vs_seq prevents going backwards:
            // if a newer version is already published (release order inverted), do not overwrite it.
            if (entry->vs_seq > vs_published) {
                RP_T(rp_i0)
                entry->access->orig_row->copy(entry->access->data);
                vs_published = entry->vs_seq;
#if RCC_ERW_ON
                rs_last_wr = entry->txn->get_txn_id(); rs_has_wr = true; rs_last_clean = entry->rs_clean; rs_left_prune();
#endif
                RP_ADD(executor->get_thd_id(), RP_INST, rp_i0)
            }
        }
    } else if (entry->status == LOCK_WAITER) {
#if DEBUG_BAMBOO 
		UInt32 cnt = 0;
		RCCLockEntry * en = waiters_head;
		bool found = false;
		while(en) {
			if (en == entry) {
				found = true;
			}
			en = en->next;
			cnt++;
		}
		assert(found);
		assert(cnt == waiter_cnt);
		assert(cnt != 0);
        LIST_RM(waiters_head, waiters_tail, entry, waiter_cnt);
		assert(waiter_cnt == cnt-1);
#else
        LIST_RM(waiters_head, waiters_tail, entry, waiter_cnt);
#endif
    } else {
		// already removed
    }
    return_entry(entry);
#if RCCB
    sweep_committed(false);   // what waited behind the leaving entry (bring_next follows)
#endif
    if (!owners) {
        bring_next(NULL);
    }
    RCC_SYNC_IDS()
    assert(owners || retired_head || (waiter_cnt == 0));
    // WAIT - done releasing with is_abort = true
    // FINISH - done releasing with is_abort = false
#if PF_CS
    PROF_CS(executor->get_thd_id(), release_cs, get_sys_clock() - starttime);
#endif
    COMPILER_BARRIER
    RP_ADD(executor->get_thd_id(), RP_REL, rp_rel0)
    RP_INC(executor->get_thd_id(), RP_RELN)
    unlock(executor);
#if PF_ABORT 
    if (rc == Abort && entry->txn->abort_chain > 0) {
    UPDATE_STATS(entry->txn->get_thd_id(), max_abort_length, entry->txn->abort_chain);
    INC_STATS(entry->txn->get_thd_id(), cascading_abort_times, 1);
    INC_STATS(entry->txn->get_thd_id(), abort_length, entry->txn->abort_chain);
  }
#endif
    return RCOK;
}


inline
bool Row_rcc::bring_next(txn_man * txn) {
#if DEBUG_BAMBOO
    check_correctness();
#endif
    bool has_txn = false;
#if !RCC_ODH
    // [!RCC_ODH] Bamboo's promotion: grant the waiters in place, in timestamp order -- every reader up to the first
    // writer, or that writer -- while there is no owner and no aborted writer to wait for
    for (RCCLockEntry * entry = waiters_head; entry && !owners; ) {
        RCCLockEntry *vprev, *vsrc;
        if (!select_grant(vprev, vsrc)) break;
        RCCLockEntry * next = entry->next;
        LIST_RM(waiters_head, waiters_tail, entry, waiter_cnt);
        VS_GRANT(entry, vprev, vsrc);
        if (entry->type == LOCK_EX) { owners = entry; owners->status = LOCK_OWNER; }
        else { ADD_TO_RETIRED_TAIL(entry); }
        entry->txn->lock_ready = true;
        RC_PW_WAKE(entry)
        if (entry->txn == txn) has_txn = true;
        if (entry->type == LOCK_EX) break;
        entry = next;
    }
    return has_txn;
#endif
    RCCLockEntry * entry = waiters_head;
    RCCLockEntry * next = NULL;
#if BB_AUTORETIRE
    bool retired_has_write = (retired_tail && (retired_tail->type == LOCK_EX || !retired_tail->is_cohead));
#endif
    // If any waiter can join the owners, just do it
    while (entry) {
		// XXX(zhihan): entry may not be waiters_head
		next = entry->next;
        if (!owners) {
            RCCLockEntry *vprev, *vsrc;
            if (!select_grant(vprev, vsrc))
                break; // Keep waiters queued until the aborted writer is removed.
            if (entry->type == LOCK_EX) { // !owners
#if BB_AUTORETIRE
                printf("ERROR: not support AUTORETIRE yet\n");
#else
                // [self-grant] wake the writer only; it re-acquires and becomes owner itself
                LIST_RM(waiters_head, waiters_tail, entry, waiter_cnt);
                entry->status = LOCK_DROPPED;
                entry->woken = true;
                entry->txn->lock_ready = true;
                RC_PW_WAKE(entry)
#endif
                break;
            } else {
#if BB_AUTORETIRE && !BB_ALWAYS_RETIRED
                printf("ERROR: not support AUTORETIRE yet\n");
#else
                // [self-grant] wake the reader only; it re-acquires the latch and grants itself
                LIST_RM(waiters_head, waiters_tail, entry, waiter_cnt);
                entry->status = LOCK_DROPPED;
                entry->woken = true;
                entry->txn->lock_ready = true;
                RC_PW_WAKE(entry)
#endif
                break;   // [wake-one] the woken reader wakes the next head after it grants itself
            }
			entry = next;
        } else
            break; // no promotable waiters
    }
    assert(owners || retired_head || (waiter_cnt == 0));
    // [RCC] A waiter's blocker is derived from its pred edge at detection time, so
    // there is no need to walk the waiters and repoint them here (pred normalization).
#if DEBUG_BAMBOO
    check_correctness();
#endif
    return has_txn;
}

// return next lock entry in the retired list after removing en (and its
// descendants if is_abort = true)
inline
RCCLockEntry * Row_rcc::rm_from_retired(RCCLockEntry * en, bool is_abort, txn_man * txn) {
    if (is_abort && (en->type == LOCK_EX)) {
        CHECK_ROLL_BACK(en); // roll back only for the first-conflicting-write
        en->txn->lock_abort = true;
        en = remove_descendants(en, txn);
        return en;
    } else {
        RCCLockEntry * next = en->next;
        RP_T(rp_u0)
        update_entry(en, txn);
        RP_ADD(txn->get_thd_id(), RP_UPD, rp_u0)
        RP_T(rp_i0)
        // [version split] Publication point of a retired EX leaving on commit (counterpart of tpl_apply).
        // It was not published at retire, so it is done here. The vs_seq guard against going backwards
        // has the same rationale as the owner path in lock_release.
        if (RCC_LUI && !is_abort && en->type == LOCK_EX && en->vs_seq > vs_published) {   // [!RCC_LUI]: in the row already
            en->access->orig_row->copy(en->access->data);
            vs_published = en->vs_seq;
#if RCC_ERW_ON
            rs_last_wr = en->txn->get_txn_id(); rs_has_wr = true; rs_last_clean = en->rs_clean; rs_left_prune();
#endif
        }
        RP_ADD(txn->get_thd_id(), RP_INST, rp_i0)
#if RCC_ERW_ON
        // [ERW] a committed reader leaves: a later writer of the version it read still owes it an rw edge
        RP_T(rp_s0)
        if (!is_abort && en->type == LOCK_SH && !en->rs_clean) rs_left_push(en->txn->get_txn_id(), en->rd_seq);
        RP_ADD(txn->get_thd_id(), RP_RSW, rp_s0)
#endif
        // [pred edge] When en leaves, the entry that pointed to en as its nearest-conflicting-pred
        // is repointed to en->prev. The only candidates are en->next and the owner:
        //   - if en is an EX removed on commit, update_entry has promoted the successor to cohead,
        //     so its pred is already NO_PRED (the check below does not match)
        //   - a read's pred is always a write, so no read points to an SH en
        //   - the owner's pred is the tail, so this applies only when en is the tail
        // Hence an O(1) repoint suffices, without an O(chain) scan.
        // Keep physical predecessors until their own cleanup. Skipping an
        // aborted writer here could release an as-yet unmarked dirty consumer.
        txnid_t en_id = en->txn->get_txn_id();
        RCCLockEntry * pv = en->prev;
        RCCLockEntry * nxt = en->next;
        txnid_t new_pred = pv ? pv->txn->get_txn_id() : RCC_NO_PRED;
        // [measurement] This repoint invalidates the slot pred cache (a source of false negatives).
        // A commit leave is always a cohead and never gets here, and an EX abort takes the branch above,
        // so in practice it happens only when an SH entry aborts on its own. Its frequency is what matters.
        if (nxt && !nxt->is_cohead && nxt->pred == en_id) {
            if (pv == NULL) {
                // No physical predecessor remains; promote the successor.
#if PF_CS
                DEC_BARRIER_PF(nxt);
#else
                DEC_BARRIER(nxt);
#endif
            } else {
            nxt->pred = new_pred;
            nxt->pred_dirty = pv ? (pv->type == LOCK_EX ||
                                    nxt->type == LOCK_SH) : false;
            RCC_NOTE_REPOINT(txn->get_thd_id());
#if RCC_TRIGGER_DETECT && CC_ALG == RCC
            // [trigger detection] the waited-for txn changed -> request a recheck (after the pred write)
            if (new_pred != RCC_NO_PRED)
                nxt->txn->rcc_recheck = true;
#endif
            }
        }
        if (owners && !owners->is_cohead && owners->pred == en_id) {
            if (pv == NULL && !nxt) {
                // [VS] same rationale as above (only when no live entry is ahead of the owner)
#if PF_CS
                DEC_BARRIER_PF(owners);
#else
                DEC_BARRIER(owners);
#endif
            } else {
            owners->pred = new_pred;
            owners->pred_dirty = pv ? (pv->type == LOCK_EX) : false;
            RCC_NOTE_REPOINT(txn->get_thd_id());
#if RCC_TRIGGER_DETECT && CC_ALG == RCC
            if (new_pred != RCC_NO_PRED)
                owners->txn->rcc_recheck = true;
#endif
            }
        }
        if (retired_cnt == 0)
          printf("error!\n");
#if DEBUG_BAMBOO
        // [caught-in-the-act dangling probe] A "live" entry still pointing to the leaving en as its pred
        // means update_entry/the repoint above missed it (to identify the path).
        for (RCCLockEntry * s_ = retired_head; s_; s_ = s_->next)
            if (s_ != en && !vs_dead(s_) && s_->pred == en_id)
                printf("[VS-DANGLE-LEAVE] leaver=%lu ltype=%d lcohead=%d holder=%lu htype=%d hcohead=%d hbar=%lu\n",
                       (uint64_t)en_id, (int)en->type, (int)en->is_cohead,
                       (uint64_t)s_->txn->get_txn_id(), (int)s_->type,
                       (int)s_->is_cohead, (uint64_t)s_->txn->commit_barriers);
        if (owners && !vs_dead(owners) && owners->pred == en_id)
            printf("[VS-DANGLE-LEAVE-OWNER] leaver=%lu ltype=%d holder=%lu\n",
                   (uint64_t)en_id, (int)en->type,
                   (uint64_t)owners->txn->get_txn_id());
#endif
        LIST_RM(retired_head, retired_tail, en, retired_cnt);
        return_entry(en);
        return next;
    }
}

// used when entry is removed from retired, need to update other's co-head info
// case 1: no following nodes (!next && !owners)
//         return
// case 2.1: entry is RD
// case 2.1.1: [entry] is co-head
// case 2.1.1.1: has other co-head
// - [R](R)W       W will not become co-head 
// case 2.1.1.2: no other co-head
// - [R]W          [acction point] W becomes cohead, decrement barrier
// case 2.1.2: [entry] is not co-head -> there is a W as co-head
// - W(R)[R](R)R  R will not become co-head
// - W(W)[R](R)R  R will not become co-head
// - W(R)[R](R)W  W will not become co-head
// - W(W)[R](R)W  W will not become co-head
// case 2.2: entry is WR
// [entry] must be co-head, otherwise rm_descendants & no need to call this
// rule: update util first write (include owners)
// - [W]RRWR      all RDs till first W (exclusive), e.g. RR, new co-head
// - [W]WR        first WR becomes new co-head
inline
void Row_rcc::update_entry(RCCLockEntry * entry, txn_man * txn) {
#if RCCB
    return;   // [RCC_BATCH] the commit pushes to its dependents; a row release promotes nobody
#endif

#if RCC_ERW_ON
    // [ERW] nobody waits for a reader (rw is not a commit dependency). A committed writer releases what waited for
    // it: the readers behind it up to the next writer, and that writer (wr / ww), identified by their pred.
    if (entry->type == LOCK_SH) return;
    {
        const txnid_t wid = entry->txn->get_txn_id();
        for (RCCLockEntry *p = entry->next; p; p = p->next) {
            if (!p->is_cohead && p->pred == wid) {
#if PF_CS
                DEC_BARRIER_PF(p);
#else
                DEC_BARRIER(p);
#endif
            }
            if (p->type == LOCK_EX) return;
        }
        if (owners && !owners->is_cohead && owners->pred == wid) {
#if PF_CS
            DEC_BARRIER_PF(owners);
#else
            DEC_BARRIER(owners);
#endif
        }
    }
    return;
#endif
    if (!entry->next && !owners) {
        return; // nothing to update
    }
    if (entry->type == LOCK_SH) {
        // entry is the only RD cohead, update the write following it to be cohead
        if (entry->is_cohead && (!entry->prev)) {
            entry = entry->next;
            if (entry) {
                    if (entry->type == LOCK_SH)
                        return; // has other RD cohead
                    else if (entry->is_cohead)
                        return; // [VS] joined past a dead entry, already cohead
                    else {
#if PF_CS
                        DEC_BARRIER_PF(entry);
#else
                        DEC_BARRIER(entry); // decrement the following entries
#endif
                    }
            } else if (owners) {
                if (owners->is_cohead)
                    return; // [VS] promoted behind a list of only dead entries, already cohead
#if PF_CS
                DEC_BARRIER_PF(owners);
#else
                DEC_BARRIER(owners);
#endif
            }
        }
        return;
    } else { // entry->type == LOCK_EX
        assert(entry->is_cohead);
        entry = entry->next;
        bool updated = false;
        while(entry) {
            // An aborted writer is still a dependency barrier until its
            // cleanup marks/removes this row's suffix. Never skip it.
            if (entry->type == LOCK_EX) {
                if (!updated) {
                    updated = true;
#if PF_CS
                    DEC_BARRIER_PF(entry);
#else
                    DEC_BARRIER(entry);
#endif
                }
                break;
            } else {
                // update all RDs before 1st write
                updated = true;
#if PF_CS
                DEC_BARRIER_PF(entry);
#else
                DEC_BARRIER(entry);
#endif

            }
            entry = entry->next;
        }
        if (!updated && owners) {
#if PF_CS
                DEC_BARRIER_PF(owners);
#else
                DEC_BARRIER(owners);
#endif
        }
    }
}

inline
RCCLockEntry * Row_rcc::remove_descendants(RCCLockEntry *en, txn_man *txn) {
    assert(en->type == LOCK_EX);
    RCCLockEntry *prev = en->prev;
    // Like Bamboo: all successors on this row are dependent. The grant gate
    // never lets a clean reader/writer bypass an aborted writer, and physical
    // predecessors are retained until cleanup. No global cascade walk here.
    LIST_RM_SINCE(retired_head, retired_tail, en);
    bool root = true;
    while (en) {
        en->txn->set_abort(root ? ABORT_SRC_SELF : ABORT_SRC_CASCADE);
#if PF_ABORT
        if (!root) txn->abort_chain++;
#endif
        root = false;
        RCCLockEntry *next = en->next;
        --retired_cnt;
        return_entry(en);
        en = next;
    }
    if (owners) {
        owners->txn->set_abort(ABORT_SRC_CASCADE);
#if PF_ABORT
        txn->abort_chain++;
#endif
        return_entry(owners);
        owners = NULL;
    }
#if !RCCB   // [RCC_BATCH] barriers are returned by push: the head keeps is_cohead = false after its writer left
    assert(!retired_head || retired_head->is_cohead);
#endif
    return prev ? prev->next : retired_head;
}

#if DEBUG_BAMBOO
// Checks list structure + cohead accounting + [RCC] cur_head_id/cur_owner_id.
// Invariant under pred normalization: right before unlock, RCC_SYNC_IDS sets both fields to the current
// head/owner. But check_correctness is also called "in the middle of" mutations such as bring_next,
// so a field check is meaningful only when the list is stable;
// only structure/cohead accounting is checked here.
// On a violation, the whole list is dumped to show the shape in which it broke.
void Row_rcc::dump_state(const char * why, RCCLockEntry * bad) {
    printf("[RCC-BAD] %s  row=%p retired_cnt=%u waiter_cnt=%u head_id=%lu owner_id=%lu\n",
           why, (void *)_row, retired_cnt, waiter_cnt,
           (uint64_t)cur_head_id, (uint64_t)cur_owner_id);
    int i = 0;
    for (RCCLockEntry * e = retired_head; e; e = e->next, i++)
        printf("   retired[%d]%s txn=%lu %s cohead=%d pred=%lu bar=%lu st=%d\n",
               i, (e == bad ? " <<<" : ""), (uint64_t)e->txn->get_txn_id(),
               e->type == LOCK_EX ? "W" : "R", (int)e->is_cohead,
               (uint64_t)e->pred,
               (uint64_t)(e->txn->commit_barriers >> 2),
               (int)e->status);
    if (owners)
        printf("   owners%s txn=%lu W cohead=%d pred=%lu bar=%lu st=%d\n",
               (owners == bad ? " <<<" : ""), (uint64_t)owners->txn->get_txn_id(),
               (int)owners->is_cohead, (uint64_t)owners->pred,
               (uint64_t)(owners->txn->commit_barriers >> 2), (int)owners->status);
    i = 0;
    for (RCCLockEntry * e = waiters_head; e; e = e->next, i++)
        printf("   waiter[%d]%s txn=%lu %s st=%d\n", i,
               (e == bad ? " <<<" : ""), (uint64_t)e->txn->get_txn_id(),
               e->type == LOCK_EX ? "W" : "R", (int)e->status);
    fflush(stdout);
}

#define RCC_CHK(cond, why, bad) \
    do { if (!(cond)) { dump_state(why, bad); assert(cond); } } while (0)

void Row_rcc::check_correctness() {
    UInt32 cnt = 0;
    bool has_conflict = false;

    RCCLockEntry * en = retired_head;
    RCCLockEntry * last_wr = NULL;   // [pred edge] last write "ahead of" en
    RCCLockEntry * prev_e  = NULL;   // [pred edge] entry right before en
    // [version split] While a dead entry (marked ABORTED) stays in the list, a joiner arriving after the marking
    // has a dependency that skips the dead entry, so the physical-neighbor invariants are (intentionally) broken.
    // Once any dead entry is seen, relaxed mode: (a) a dead entry's own pred/cohead reflect a stale structure
    // and are not checked, (b) a live entry's pred passes if it is the "nearest physical" or the "nearest
    // live" entry, (c) the cohead count equality is skipped.
    bool dead_seen = false;
    RCCLockEntry * prev_live = NULL;
    // [VS] ids of all entries ahead (live + dead). A pred pointing to "someone still in the list"
    // is fine: a live entry pointing to a dead entry is only in the transient window (notice delay) before
    // that dead entry's phase 1 marks it. The real defect is a dangling pred "pointing to a txn that has left",
    // which is not in this list and is still caught.
    txnid_t vs_ahead[THREAD_CNT + 1];   // chain <= number of concurrent txns
    int vs_na = 0;
    while (en) {
        RCC_CHK(en->status == LOCK_RETIRED, "retired entry status", en);
        if ((en->txn->commit_barriers & 3UL) == ABORTED) {
            dead_seen = true;
            if (vs_na < (int)(THREAD_CNT + 1)) vs_ahead[vs_na++] = en->txn->get_txn_id();
            if (en->type == LOCK_EX)
                last_wr = en;
            prev_e = en;
            cnt++;
            en = en->next;
            continue;
        }
        if (dead_seen) {
            if (en->is_cohead) {
                RCC_CHK(en->pred == RCC_NO_PRED, "cohead must have no pred", en);
            } else {
                bool ok = false;
                for (int i = 0; i < vs_na && !ok; i++)
                    if (en->pred == vs_ahead[i]) ok = true;
                RCC_CHK(ok, "pred must point into list ahead", en);
            }
            if (vs_na < (int)(THREAD_CNT + 1)) vs_ahead[vs_na++] = en->txn->get_txn_id();
            if (en->type == LOCK_EX)
                last_wr = en;
            prev_e = en;
            prev_live = en;
            cnt++;
            en = en->next;
            continue;
        }
        if (vs_na < (int)(THREAD_CNT + 1)) vs_ahead[vs_na++] = en->txn->get_txn_id();
        prev_live = en;   // (advances with prev_e after the common checks below)
        // [pred edge] Invariant: a cohead has NO_PRED, a non-cohead write points to the entry right before it,
        // a non-cohead read to the nearest write ahead. (Checked against the state before en, so
        // before last_wr/prev_e are updated)
        if (en->is_cohead)
            RCC_CHK(en->pred == RCC_NO_PRED, "cohead must have no pred", en);
        else if (en->type == LOCK_EX)
            RCC_CHK(prev_e != NULL && en->pred == prev_e->txn->get_txn_id(),
                    "W pred must be immediate prev", en);
        else
            RCC_CHK(last_wr != NULL && en->pred == last_wr->txn->get_txn_id(),
                    "R pred must be nearest write ahead", en);
        if (en->type == LOCK_EX)
            has_conflict = true;
        if (en == retired_head) {
            RCC_CHK(en->is_cohead, "head must be cohead", en);
        } else {
            RCC_CHK(en->is_cohead == !has_conflict, "cohead accounting", en);
        }
        if (en->type == LOCK_EX)
            last_wr = en;
        prev_e = en;
        cnt++;
        en = en->next;
    }
    RCC_CHK(cnt == retired_cnt, "retired_cnt mismatch", NULL);

    if (owners) {
        RCC_CHK(owners->status == LOCK_OWNER, "owner status", owners);
        RCC_CHK(owners->type == LOCK_EX, "owner must be W", owners);
        if (dead_seen) {
            // [version split] While dead entries remain, the owner joined behind the "live tail".
            if (owners->is_cohead)
                RCC_CHK(owners->pred == RCC_NO_PRED, "owner cohead must have no pred", owners);
            else {
                bool ok = (retired_tail && owners->pred == retired_tail->txn->get_txn_id()) ||
                          (prev_live && owners->pred == prev_live->txn->get_txn_id());
                // [marked-not-cleaned window] If the pred, live at join time, has just died by a marking
                // outside the latch, pointing to that dead entry still in the list is legal:
                // that dead entry's sweep (remove_descendants) marks the owner and
                // cleans it up. The real defect (a dangling pred pointing to a txn that has left) is still caught.
                if (!ok)
                    for (RCCLockEntry * de = retired_head; de && !ok; de = de->next)
                        if (owners->pred == de->txn->get_txn_id() && vs_dead(de))
                            ok = true;
                RCC_CHK(ok, "owner pred must be tail (phys, live, or listed corpse)", owners);
            }
        } else {
        RCC_CHK(owners->is_cohead == (retired_head == NULL), "owner cohead", owners);
        // [pred edge] A non-cohead owner always points to the current tail
        if (owners->is_cohead)
            RCC_CHK(owners->pred == RCC_NO_PRED, "owner cohead must have no pred", owners);
        else
            RCC_CHK(retired_tail != NULL &&
                    owners->pred == retired_tail->txn->get_txn_id(),
                    "owner pred must be tail", owners);
        }
    }

    // waiters. On entry to bring_next, owners may be briefly empty.
    cnt = 0;
    en = waiters_head;
    while (en) {
        RCC_CHK(en->status == LOCK_WAITER, "waiter status", en);
        RCC_CHK(!en->is_cohead, "waiter must not be cohead", en);
        en = en->next;
        cnt++;
    }
    RCC_CHK(cnt == waiter_cnt, "waiter_cnt mismatch", NULL);
}
#endif




