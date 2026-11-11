#ifndef ROW_RCC_H
#define ROW_RCC_H
// [BB_PF_PW for RCC] the latch holder logs its prevention (the prevent() of RCC_ODH false, the waiter queue) and detection
// (ERW edge collection) intervals per latch in its own buffer; a latch wait is logged whole and split by them after
// the run (system/stats.h BB_PW_LATCH_RAW).  Nothing is shared at run time.
#if CC_ALG == RCC && BB_PF_PW
#define RCC_PW 1
#if LATCH != LH_SPINLOCK || !PF_CS
#error "BB_PF_PW for RCC logs pthread spinlock waits with the PF_CS timestamps: needs LATCH = LH_SPINLOCK and PF_CS"
#endif
enum { RC_PH_FREE = 0, RC_PH_OTHER, RC_PH_PREV, RC_PH_DET };
extern thread_local uint64_t rc_pw_holder;   // txn id of the latch holder on this worker (wake / release events)
extern thread_local bool rc_pw_waited;       // the last lock() had to wait for the latch
// own work under the latch, stored in the pending log (moved at the next attempt start, system/stats.h)
// own work under the latch: the start reuses a timestamp when one was just taken (_AT: ns), else a raw rdtsc; the piece
// and the holder interval go to the pending log: one rdtsc inside the latch
#define RC_PH_BEGIN(v, X) const int v = (X); const uint64_t v##_t0 = bb_pw_ticks(); const int v##_raw = 3;
#define RC_PH_BEGIN_AT(v, X, t) const int v = (X); const uint64_t v##_t0 = (t); const int v##_raw = 2;
#define RC_PH_END(v, kind) bb_pw_cs_piece((uint64_t) latch, v##_t0, bb_pw_ticks(), kind, v == RC_PH_DET ? BB_PW_LATCH_D : BB_PW_LATCH, v##_raw);
// a latch wait, from the timestamps PF_CS takes around the latch anyway (lock() only notes whether it had to wait)
#define RC_PW_LATCH(t0, t1) if (rc_pw_waited) bb_pw_latch_wait((uint64_t) latch, t0, t1);
// lock() where no PF_CS timestamps are taken: its own (the end only if it waited)
#define RC_LOCK_LOGGED(tx) { const uint64_t rc_l0 = get_sys_clock(); lock(tx); RC_PW_LATCH(rc_l0, get_sys_clock()) }
#define RC_PW_WAKE(entry) bb_pw_wake((entry)->txn->get_txn_id(), rc_pw_holder, get_sys_clock());
// a release that brings the successor's commit barrier to 0; `txn` is the releasing transaction at every call site
#define RCC_DEC_CB(entry) { uint64_t rc_nv = ATOM_SUB_FETCH(entry->txn->commit_barriers, 1UL << 2); \
    if ((rc_nv >> 2) == 0) bb_pw_release_event(entry->txn->get_txn_id(), txn->get_txn_id(), get_sys_clock()); }
#else
#define RCC_PW 0
#define RC_PH_BEGIN(v, X)
#define RC_PH_BEGIN_AT(v, X, t)
#define RC_PH_END(v, kind)
#define RC_PW_LATCH(t0, t1)
#define RC_LOCK_LOGGED(tx) lock(tx);
#define RC_PW_WAKE(entry)
#define RCC_DEC_CB(entry) entry->txn->decrement_commit_barriers();
#endif
#if CC_ALG == RCC && !RCC_LUI && (RCC_ERW || !RCC_ODH)
#error "RCC_LUI false (Bamboo's in-place writes + undo) is the ablation step before lightweight rollback: needs RCC_ERW false"
#endif
#if CC_ALG == RCC && !RCC_ODH && (RCC_ERW || RCC_ADD || RCC_TRIGGER_DETECT)
#error "RCC_ODH false (prevention instead of detection) needs RCC_ERW false, RCC_ADD false, RCC_TRIGGER_DETECT false"
#endif
#include "rcc_srdet.h"   // RCC_ERW_ON: ERW (serialization detector) is on

// [version split] Joining past dead entries and the dependency-precise sweep both rely on "marking precedes
// undo/release": if marking is late, a joiner copies a version that is about to die, and the sweep takes an
// unmarked dependent for a survivor and leaves an orphan.
// The abort notification (propagated to the consumers as soon as the abort is decided)
// guarantees that marking comes first. A victim notices its own abort via the base lock_abort/ABORTED check.

// [RCC] "no blocker" sentinel.
// 0 cannot be used: since txn_id = thd_id + thd_txn_id * thread_cnt,
// the first transaction of thread 0 really has txn_id == 0.
#define RCC_NO_PRED ((txnid_t)-1)

// [measurement] pred cache soundness counters (see the rcc_note_* comment in dl_detect.h).
// This file is always compiled regardless of CC_ALG, but those DL_detect members exist
// only in RCC/BB_FAKE_DETECT builds, so elsewhere the macros expand to nothing.
// [measurement only] Counters that feed only the rcc_cache_sound/exact_pct statistics (slot pred cache soundness
// study); detection never uses them. Each DEC/repoint costs an atomic RMW under the latch plus a detector
// object call, so they are kept off the worker hot path; when needed,
// enable RCC_PRED_CACHE_PROBE (otherwise the statistics stay 0 = not measured).
#if (CC_ALG == RCC || (CC_ALG == BAMBOO && BB_FAKE_DETECT)) && RCC_PRED_CACHE_PROBE
#define RCC_REPOINT_PROBE(thd)  dl_detector.rcc_note_repoint(thd)
#define RCC_NOTE_PREDCLEAR(thd) dl_detector.rcc_note_predclear(thd)
#else
#define RCC_REPOINT_PROBE(thd)  {}
#define RCC_NOTE_PREDCLEAR(thd) {}
#endif
#if CC_ALG == RCC && RCC_ADD && RCC_DETECT_EVENT
#define RCC_NOTE_REPOINT(thd) { RCC_REPOINT_PROBE(thd); dl_detector.rcc_request_check(); }
#else
#define RCC_NOTE_REPOINT(thd) { RCC_REPOINT_PROBE(thd); }
#endif

// [abort notification] Non-VS path: no registration (empty macro).
// Non-VS path only: under VS, VS_GRANT registers with the actual version supplier (vsrc)
// (to catch exactly the cases where prev != supplier, e.g. an SH joining behind an SH).
#define RCC_CL_REG(src, en) {}

// update cohead info when a newly-init entry (en) is firstly 
// added to owners (bring_next, WR)
// or tail of retired (lock_get/bring_next, RD)
// (not apply when moving from owners to retired
// Algorithm:
//     if previous entry is not null
//         if self is WR,
//             not cohead, need to incr barrier
//         else 
//             if previous entry is RD
//               if prev is cohead, 
//                   //time saved is 0.
//               otherwise, self is not cohead, need to incr barrier,
//                   //record start_ts to calc time saved when becomes cohead
//             else if prev is WR
//               not cohead, need to incr barrier,
//               //record start_ts to calc time saved when becomes cohead
//     else
//         read no dirty data, becomes cohead
//         //record time saved from elr is 0.
//     		
// [RCC pred edge] A blocked entry stores the txnid of its "nearest conflicting predecessor"
// in pred: for a write, the entry right before it (conflicts with all); for a read, the nearest write ahead
// (a read does not block a read behind it, so pointing to a neighboring read would be a false edge).
// A cohead has RCC_NO_PRED. A detection round reads this value without locking and uses it as the edge.
//   - Unlike deriving the edge from the head (cur_head_id), the target changes only "when the preceding
//     conflicting neighbor leaves", so edges are stable -> fewer torn snapshots (phantom cycles), a cycle
//     is visible right after insertion (before its members rise to the head), and the persistence filter's
//     signature matching holds.
//   - Maintained at: UPDATE_RETIRE_INFO (set) / rm_from_retired (repoint when the target leaves)
//     / DEC_BARRIER (cleared on promotion to cohead) / return_entry (cleanup). All under the row latch.
//   - cur_head_id / cur_owner_id are kept only for debug dumps.
#define UPDATE_RETIRE_INFO(en, prev) { \
  if (prev) { \
    if (en->type == LOCK_EX) { \
      en->pred = prev->txn->get_txn_id(); \
      en->pred_dirty = (prev->type == LOCK_EX); \
      RCC_CL_REG(prev, en) \
      en->txn->increment_commit_barriers(); \
    } else { \
            if (prev->type == LOCK_SH) { \
              en->is_cohead = prev->is_cohead; \
              if (!en->is_cohead) { \
                en->pred = prev->pred; \
                en->pred_dirty = true; /* a reader's pred is always a write */ \
                en->txn->increment_commit_barriers(); \
              } \
            } else { \
              en->pred = prev->txn->get_txn_id(); \
              en->pred_dirty = true; /* reads the EX pred's dirty data */ \
              RCC_CL_REG(prev, en) \
              en->txn->increment_commit_barriers(); } \
    } \
  } else { \
    en->is_cohead = true; \
   } }

// [ERW] join rule: a transaction waits (commit barrier) only for its WRITE predecessor on the row: a reader for the
// nearest writer ahead (wr), a writer for the nearest writer ahead (ww). A writer never waits for the readers in front
// of it (rw): those are ordered by the serialization detector instead. `src` is the nearest live writer in retired.
#define UPDATE_RETIRE_INFO_S(en, prev, src) { \
  if (en->type == LOCK_EX) { \
    if (src) { \
      en->pred = src->txn->get_txn_id(); \
      en->pred_dirty = true; \
      en->txn->increment_commit_barriers(); \
    } else { \
      en->is_cohead = true; \
    } \
  } else if (prev) { \
    if (prev->type == LOCK_SH) { \
      en->is_cohead = prev->is_cohead; \
      if (!en->is_cohead) { \
        en->pred = prev->pred; \
        en->pred_dirty = true; \
        en->txn->increment_commit_barriers(); \
      } \
    } else { \
      en->pred = prev->txn->get_txn_id(); \
      en->pred_dirty = true; \
      en->txn->increment_commit_barriers(); \
    } \
  } else { \
    en->is_cohead = true; \
  } }
// [RCC_BATCH] the commit barrier counts the write predecessor (the nearest writer ahead, as ERW) only while it has not
// committed; the predecessor's commit decrements it directly (dep push, registered right after the latch), not the row.
// is_cohead = false <=> this entry registers with src.
#define UPDATE_RETIRE_INFO_B(en, prev, src) { \
  (void) (prev); \
  if (src && !src->crel && src->txn != en->txn) { \
    en->pred = src->q_id; \
    en->pred_dirty = true; \
    en->txn->increment_commit_barriers(); \
  } else { \
    en->is_cohead = true; \
  } }
#if RCCB
#define RCC_JOIN_INFO(en, prev, src) UPDATE_RETIRE_INFO_B(en, prev, src)
#elif RCC_ERW_ON
#define RCC_JOIN_INFO(en, prev, src) UPDATE_RETIRE_INFO_S(en, prev, src)
#else
#define RCC_JOIN_INFO(en, prev, src) UPDATE_RETIRE_INFO(en, prev)
#endif

// Apply the predecessor and version source captured by select_grant().
// The row latch protects the captured entries and their version storage.
// Do not search again here: an out-of-latch abort must not turn a selected
// speculative source into a fallback read from the committed base.
#if RCCB
// Recheck the captured version supplier after the copy; a committed entry's owner may be on its next attempt already
// (if it commits, moves on and aborts that next attempt right between the two reads, the consumer aborts needlessly).
#define RCC_CL_REG_VS(src, en) { if (!(src)->crel) (src)->txn->check_version_source((en)->txn); }
#elif CC_ALG == RCC
// Recheck the captured version supplier after the copy; propagation is row-local.
#define RCC_CL_REG_VS(src, en) { (src)->txn->check_version_source((en)->txn); }
#else
#define RCC_CL_REG_VS(src, en) {}
#endif

#if DEBUG_BAMBOO
#define VS_GRANT_PROBE(en) { \
    if (en->pred != RCC_NO_PRED) { \
        txn_man * pt = glob_manager->get_txn_man(en->pred % g_thread_cnt); \
        if (pt && pt->get_txn_id() == en->pred && \
            (pt->commit_barriers & 3UL) == ABORTED) \
            printf("[VS-DEADPRED] join txn=%lu type=%d pred=%lu (dead at join)\n", \
                   (uint64_t)en->txn->get_txn_id(), (int)en->type, \
                   (uint64_t)en->pred); \
    } }
#else
#define VS_GRANT_PROBE(en) {}
#endif
#if !RCC_LUI
// [!RCC_LUI] no private versions: the row holds the dirty data of the writers ahead; nothing is copied at the grant
#define VS_GRANT(en, vprev, vsrc) { \
    RCC_JOIN_INFO(en, vprev, vsrc); \
    if (en->type == LOCK_EX) \
        en->vs_seq = ++vs_grant_seq; }
#else
#define VS_GRANT(en, vprev, vsrc) { \
    RCC_JOIN_INFO(en, vprev, vsrc); \
    VS_GRANT_PROBE(en); \
    if (vsrc) { \
        en->access->data->copy(vsrc->access->data); \
        en->vs_copied = true; \
        RCC_CL_REG_VS(vsrc, en) \
    } \
    if (en->type == LOCK_EX) \
        en->vs_seq = ++vs_grant_seq; }
#endif


// txn id of the current retired_head (RCC_NO_PRED if none). Use only inside Row_rcc member functions.
#define RCC_HEAD_ID() (retired_head ? retired_head->txn->get_txn_id() : RCC_NO_PRED)

// [debug] cur_head_id / cur_owner_id are not on the detection path (pred edges are) and
// are kept only for dump_state output. The update is "O(1)" but costs 2 remote txn_man reads +
// 2 volatile stores under the latch on every get/retire/release, so it runs
// only in DEBUG builds.
#if DEBUG_BAMBOO
#define RCC_SYNC_IDS() { \
    cur_head_id  = RCC_HEAD_ID(); \
    cur_owner_id = owners ? owners->txn->get_txn_id() : RCC_NO_PRED; }
#else
#define RCC_SYNC_IDS() {}
#endif

// used by lock_retire() (move from owners to retired)
// or by lock_get()/bring_next(), used when has no owners but directly enters retired
// for the latter need to call UPDATE_RETIRE_INFO(to_insert, retired_tail);
#define ADD_TO_RETIRED_TAIL(to_retire) { \
  LIST_PUT_TAIL(retired_head, retired_tail, to_retire); \
  to_retire->status = LOCK_RETIRED; \
  if (vs_dead(to_retire)) clean_seq = (uint64_t)-1; /* [abort-seq] dead on entry */ \
  retired_cnt++; }

// // Insert to_insert(RD) into the tail when owners is not empty
// // (1) update inserted entry's cohead information
// // (2) NEED to update owners cohead information 
// //     if owner is not cohead, it cannot become one with RD inserted
// //     if owner is cohead, RD becomes cohead and owner is no longer a cohead 
// #define INSERT_TO_RETIRED_TAIL(to_insert) { \
//   UPDATE_RETIRE_INFO(to_insert, retired_tail); \
//   if (owners && owners->is_cohead) { \
//     owners->is_cohead = false; \
//     owners->txn->increment_commit_barriers(); \
//   } \
//   LIST_PUT_TAIL(retired_head, retired_tail, to_insert); \
//   to_insert->status = LOCK_RETIRED; \
//   retired_cnt++; }

#define RETIRE_ENTRY(to_retire) { \
  to_retire = owners; \
  owners = NULL; \
  to_retire->next=NULL; \
  to_retire->prev=NULL; \
  ADD_TO_RETIRED_TAIL(to_retire); }

// [ROLLBACK_DELAY] The actual undo point. RCC does not wound, so the only call path is
// lock_release (own abort). Unlike BAMBOO, a transaction never performs another's undo.
// It still runs under the row latch, so the delay serializes that row.
// (Fairness: paid at the same point and per row as BAMBOO's CHECK_ROLL_BACK.)
// [version split] Dirty data is never published to the base, so there is nothing to restore (abort = discard).
// RCC keeps even a row's conflict-free first write as a version, so there is no rollback spin either (all discarded).
#if !RCC_LUI
// [!RCC_LUI] Bamboo's undo: the first conflicting writer's before-image back into the row, under the latch
#define CHECK_ROLL_BACK(en) { en->access->orig_row->copy(en->access->orig_data); }
#else
#define CHECK_ROLL_BACK(en) {}
#endif

// [measurement] Clearing pred also invalidates the slot cache. But the stale cache then claims an edge that
// no longer exists, so it only yields a false positive, which the existing cycle validation catches.
// Hence it is counted separately from repoint (see the rcc_note_* comment in dl_detect.h).
#if RCC_ERW_PROBE
#define RCC_DEC_TS(entry) { entry->txn->last_dec_ts = get_sys_clock(); }
#else
#define RCC_DEC_TS(entry) {}
#endif
#define DEC_BARRIER_PF(entry) { \
    assert(!entry->is_cohead); \
    entry->is_cohead = true; \
    entry->pred = RCC_NO_PRED; /* [pred edge] promotion to cohead = edge removed */ \
    RCC_NOTE_PREDCLEAR(entry->txn->get_thd_id()); \
    uint64_t starttime = get_sys_clock(); \
    RCC_DEC_TS(entry); \
    RCC_DEC_CB(entry) \
    INC_STATS(txn->get_thd_id(), time_semaphore_cs, \
        get_sys_clock() - starttime); \
}

#define DEC_BARRIER(entry) { \
    assert(!entry->is_cohead); \
    entry->is_cohead = true; \
    entry->pred = RCC_NO_PRED; /* [pred edge] promotion to cohead = edge removed */ \
    RCC_NOTE_PREDCLEAR(entry->txn->get_thd_id()); \
    RCC_DEC_TS(entry); \
    RCC_DEC_CB(entry) \
}


struct RCCLockEntry {
    // type of lock: EX or SH
    txn_man * txn; // 0-7
    Access * access; // 8-15
    // [pred edge] txnid of the nearest conflicting predecessor (RCC_NO_PRED = not blocked).
    // Written only under the row latch; detection rounds read it without locking (volatile 8B, atomic).
    volatile txnid_t pred; // 16-23
    lock_t type; // 24-27
    // Whether this entry consumed pred's dirty data (pred is a writer). If false, there is only an order dependency.
    // Updated only together with pred (under the row latch).
    volatile bool pred_dirty; // 28
    uint8_t padding[64 - sizeof(void *)*2 - sizeof(txnid_t) - sizeof(lock_t) - 1];
    RCCLockEntry * next;
    bool is_cohead;
    lock_status status;
    RCCLockEntry * prev;
    // [version split] vs_seq: per-row monotonic EX grant number; stops an older commit from overwriting a newer one.
    // vs_copied: whether the grant copied a live EX pred's version into this entry's own copy
    //            (= the caller must read access->data, not the base; rc=FINISH).
    uint64_t vs_seq;
    uint64_t rd_seq;   // [ERW] the version this entry read: the vs_seq of its source writer, or the row's published one
    txnid_t q_id;      // attempt id at the grant: this entry's identity once its owner moved on to its next attempt
    // [ERW] this entry's attempt committed without any incoming edge. Edges into an attempt are logged only by that
    // attempt, during its accesses, so after its commit it can never get one: it cannot be on a cycle, and an edge from
    // it is useless (the detector drops the node anyway, but only once it has seen the commit).
    volatile bool rs_clean;
    // [RCC_BATCH] committed, waiting in the queue for a sweep (set by the owner, which may be gone by then); and who
    // owns the Access: 0 its owner, 1 given up to the queue (the sweeper returns it), 2 claimed by a sweeper
    volatile bool crel;
    volatile uint8_t handoff;
    bool vs_copied;
    bool woken;

    RCCLockEntry(txn_man * t, Access * a): txn(t), access(a), pred(RCC_NO_PRED),
                                          type(LOCK_NONE),
                                          next(NULL), is_cohead(false),
                                          status(LOCK_DROPPED),
                                          prev(NULL) { woken = false; rd_seq = 0; q_id = 0; rs_clean = false; crel = false; handoff = 0; }   // [wake-one] first use read an uninitialized bool (UBSan)
};

#if CC_ALG == RCC && RCC_ERW_PROBE
void rcc_probe_print();
#endif

class Row_rcc {
  public:
    void init(row_t * row);
    RC lock_get(lock_t type, txn_man * txn, Access * access);
    RC lock_release(RCCLockEntry * entry, RC rc, txn_man * executor = nullptr);
    RC lock_retire(RCCLockEntry * entry);
#if RCC_DEBUG_STUCK
    // [diagnostic] print this row's queue under its latch (called by a transaction stuck in commit wait)
    void stuck_dump(txn_man * me);
#endif

    // [debug] txnid of the current retired_head / owner (RCC_NO_PRED if none).
    // Not used on the detection path (pred edges replaced them); kept for RCC_DEBUG_STUCK dumps.
    volatile txnid_t cur_head_id;
    volatile txnid_t cur_owner_id;

  private:
    // data structure
    RCCLockEntry * owners;
    RCCLockEntry * retired_head;
    RCCLockEntry * retired_tail;
    RCCLockEntry * waiters_head;
    RCCLockEntry * waiters_tail;
    row_t * _row;
    UInt32 waiter_cnt;
    UInt32 retired_cnt;
    uint64_t clean_seq;   // [abort-seq] g_abort_seq value at the last scan that found no dead entry
    // latches
#if LATCH == LH_SPINLOCK
    pthread_spinlock_t * latch;
#elif LATCH == LH_MUTEX
    pthread_mutex_t * latch;
#else
    mcslock * latch;
#endif
    bool blatch;
    // [version split] This row's EX grant number generator / the highest number published to the base so far.
    // Lets commit publication overwrite the base "only with a newer version", regardless of release order
    // (the physical release order can differ from the commit order; going backwards would be a lost update).
    uint64_t vs_grant_seq;
    uint64_t vs_published;
#if RCC_ERW_ON
    // [ERW] the writer whose version is published (a later access depends on it: wr / ww), and the committed readers
    // that read the published version and have left the row (a later writer overwrites what they read: rw). Entries
    // whose version was overwritten by a committed writer, or whose attempt left the detector's graph, are pruned.
    uint64_t rs_last_wr;
    bool rs_has_wr;
    bool rs_last_clean;   // rs_last_wr committed without incoming edges (see RCCLockEntry::rs_clean): no edge from it
    uint64_t * rs_left_id;
    uint64_t * rs_left_seq;
    uint32_t rs_left_n, rs_left_cap;
    void rs_left_prune() {
        uint32_t w = 0;
        for (uint32_t i = 0; i < rs_left_n; i++)
            if (rs_left_seq[i] == vs_published && !rcc_srdet.gone(rs_left_id[i])) {
                rs_left_id[w] = rs_left_id[i]; rs_left_seq[w] = rs_left_seq[i]; w++;
            }
        rs_left_n = w;
    }
#if RCCB
    // [RCC_BATCH] committed entries not yet swept (+1 by the owner before it marks the entry, -1 by the sweeper that
    // removes it). A latch holder that releases the latch while it is non-zero tries to sweep again, so an entry whose
    // owner found the latch busy is always swept by the holder that made it wait.
    volatile uint32_t * rb_crel;   // in the latch's cache line (init)
    bool try_lock(txn_man * txn);
    void latch_release(txn_man * txn);
    uint32_t sweep_committed(bool wake = true);   // wake: bring the next waiter in if the owner left
  public:
    // [RCC_BATCH] a committed transaction's release: mark the entry committed and sweep the row now if its latch is
    // free, else leave the sweep to the latch holder. True: the entry already left the queue.
    bool lock_release_async(RCCLockEntry * entry, txn_man * txn);
  private:
#endif
    void rs_left_push(uint64_t id, uint64_t seq) {
        if (seq != vs_published) return;   // a committed writer overwrote it: its rw edge was recorded at that writer's join
        if (rs_left_n == rs_left_cap) {
            rs_left_prune();
            if (rs_left_n == rs_left_cap) {
                const uint32_t cap = rs_left_cap ? rs_left_cap * 2 : 8;
                rs_left_id = (uint64_t *) realloc(rs_left_id, cap * sizeof(uint64_t));
                rs_left_seq = (uint64_t *) realloc(rs_left_seq, cap * sizeof(uint64_t));
                rs_left_cap = cap;
            }
        }
        rs_left_id[rs_left_n] = id; rs_left_seq[rs_left_n] = seq; rs_left_n++;
    }
#endif

    // helper functions
    bool              bring_next(txn_man * txn);
    void              update_entry(RCCLockEntry * en, txn_man * txn);
    RCCLockEntry *     rm_from_retired(RCCLockEntry * en, bool is_abort, txn_man * txn);
    RCCLockEntry *     remove_descendants(RCCLockEntry * en, txn_man * txn);
    void              lock(txn_man * txn);
    void              unlock(txn_man * txn);
	// RC                insert_read_to_retired(RCCLockEntry * to_insert, ts_t ts, Access * access);
#if DEBUG_BAMBOO
	void              check_correctness();
	void              dump_state(const char * why, RCCLockEntry * bad);
#endif

    // // check priorities
    // inline static bool a_higher_than_b(ts_t a, ts_t b) {
    //     return a < b;
    // };

    // inline static int assign_ts(ts_t ts, txn_man * txn) {
    //     if (ts == 0) {
    //         ts = txn->set_next_ts(1);
    //         // if fail to assign, reload
    //         if ( ts == 0 )
    //             ts = txn->get_ts();
    //     }
    //     return ts;
    // };


    // init a lock entry (pre-allocated in each txn's access)
    static RCCLockEntry * get_entry(Access * access) {
    #if CC_ALG == RCC
        RCCLockEntry * entry = access->lock_entry;
    #if DEBUG_BAMBOO
        // [canary] Reusing an entry still left in a list (leak) cross-contaminates the lists.
        if (entry->status != LOCK_DROPPED) {
            printf("[VS-LEAK] entry=%p txn=%lu status=%d type=%d cohead=%d pred=%lu\n",
                   (void *)entry, (uint64_t)entry->txn->get_txn_id(),
                   (int)entry->status, (int)entry->type, (int)entry->is_cohead,
                   (uint64_t)entry->pred);
            fflush(stdout);
        }
    #endif
        entry->txn->lock_ready = false;
        // dont init lock_abort, can only be set true but not false.
        entry->next = NULL;
        entry->prev = NULL;
        entry->status = LOCK_DROPPED;
        entry->is_cohead = false;
        entry->pred = RCC_NO_PRED; // [pred edge] reset
        entry->vs_seq = 0;
        entry->vs_copied = false;
        entry->q_id = entry->txn->get_txn_id();
        entry->rs_clean = false;
        entry->crel = false;
        entry->handoff = 0;
        // [wake-one fix] `woken` is NOT reset here: the woken waiter re-enters lock_get through get_entry()

        return entry;
#else
        return NULL;
    #endif
    };


    // clean the lock entry
    // (status = LOCK_DROPPED itself removes the edge: rcc_entry_dep skips DROPPED)
    void return_entry(RCCLockEntry * entry) {
        entry->next = NULL;
        entry->prev = NULL;
#if RCCB
        // [RCC_BATCH] claim before DROPPED is published (the owner may reuse the entry right after it): if the owner
        // gave the Access up to the queue, return it to the owner's pool
        const uint8_t h = __atomic_exchange_n(&entry->handoff, 2, __ATOMIC_SEQ_CST);
        entry->pred = RCC_NO_PRED;
        __atomic_store_n(&entry->status, LOCK_DROPPED, __ATOMIC_RELEASE);
        if (h == 1) entry->txn->rb_return(entry->access);
        return;
#endif
        entry->status = LOCK_DROPPED;
        entry->pred = RCC_NO_PRED;
    };

	inline bool bring_out_waiter(RCCLockEntry * entry, txn_man * txn) {
		LIST_RM(waiters_head, waiters_tail, entry, waiter_cnt);
		entry->txn->lock_ready = true;
		if (txn == entry->txn) {
			return true;
		}
		return false;
	};

	// inline void add_to_waiters(ts_t ts, RCCLockEntry * to_insert) {
	// 	RCCLockEntry * en = waiters_head;
	// 	while (en != NULL) {
	// 		if (ts < en->txn->get_ts())
	// 				break;
	// 		en = en->next;
	// 	}
	// 	if (en) {
	// 		LIST_INSERT_BEFORE(en, to_insert);
	// 		if (en == waiters_head) 
	// 			waiters_head = to_insert;
	// 	} else {
	// 		LIST_PUT_TAIL(waiters_head, waiters_tail, to_insert);
	// 	}
	// 	to_insert->status = LOCK_WAITER;
	// 	to_insert->txn->lock_ready = false;
	// 	waiter_cnt++;
    //     assert(ts != 0);
	// };	
    
#if !RCC_ODH
    // [!RCC_ODH] Bamboo's prevention (row_bamboo.h): priorities, dynamic timestamps, ordered waiters, wound
    inline static bool a_higher_than_b(ts_t a, ts_t b) { return a < b; }
    inline static ts_t assign_ts(ts_t ts, txn_man * t) {
        if (ts == 0) {
            ts = t->set_next_ts(1);
            if (ts == 0) ts = t->get_ts();   // another worker assigned it first
        }
        return ts;
    }
    // waiters in timestamp order (oldest first); bring_next wakes the head
    inline void add_to_waiters_ts(RCCLockEntry * to_insert) {
        const ts_t ts = assign_ts(to_insert->txn->get_ts(), to_insert->txn);
        RCCLockEntry * en = waiters_head;
        while (en != NULL && !(ts < en->txn->get_ts())) en = en->next;
        if (en) {
            LIST_INSERT_BEFORE(en, to_insert);
            if (en == waiters_head) waiters_head = to_insert;
        } else {
            LIST_PUT_TAIL(waiters_head, waiters_tail, to_insert);
        }
        to_insert->status = LOCK_WAITER;
        to_insert->woken = false;
        to_insert->txn->lock_ready = false;
        waiter_cnt++;
    }
    // wound = mark the victim's attempt aborted and, as Bamboo does, remove its entry right away (rm_from_retired with
    // is_abort: a writer takes the entries after it -- they read its version -- with it; RCC's lightweight rollback has
    // nothing to undo, the private versions are just dropped). The victim's own cleanup then finds its entries dropped.
    // Returns false if the victim committed already (nothing to do).
    inline bool wound(RCCLockEntry * en, txn_man * txn) {
        return en->txn->set_abort(ABORT_SRC_WOUND, nullptr, en->q_id) != COMMITED;
    }
    // a read wounds the younger writers ahead of it
    inline void wound_retired_rd(ts_t ts, txn_man * txn) {
        RCCLockEntry * en = retired_head;
        while (en) {
            if (en->txn != txn && en->type == LOCK_EX && a_higher_than_b(ts, en->txn->get_ts()) && wound(en, txn))
                en = rm_from_retired(en, true, txn);
            else
                en = en->next;
        }
    }
    // a write wounds every younger (or unassigned) entry ahead of it
    inline void wound_retired_wr(ts_t ts, txn_man * txn) {
        RCCLockEntry * en = retired_head;
        while (en) {
            if (en->txn != txn && (en->txn->get_ts() == 0 || a_higher_than_b(ts, en->txn->get_ts())) && wound(en, txn))
                en = rm_from_retired(en, true, txn);
            else
                en = en->next;
        }
    }
    inline void wound_owner(txn_man * txn) {
        // the owner may be gone already: wounding a writer ahead removes everything after it, the owner included
        if (owners && owners->txn != txn && wound(owners, txn)) { return_entry(owners); owners = NULL; }
    }
    // the prevention step of lock_get (Bamboo's lock_get with BB_DYNAMIC_TS, minus its own grant / wait decisions,
    // which stay RCC's: wait while there is an owner or a dead writer, else join)
    inline void prevent(lock_t type, txn_man * txn) {
        ts_t ts = 0, owner_ts = 0;
        if (type == LOCK_SH) {
            if (owners) {
                owner_ts = assign_ts(owners->txn->get_ts(), owners->txn);
                ts = assign_ts(ts, txn);
                if (!a_higher_than_b(owner_ts, ts)) {   // older than the owner: wound the younger writers and the owner
                    wound_retired_rd(ts, txn);
                    wound_owner(txn);
                }
            } else if (retired_tail && (retired_tail->type == LOCK_EX || !retired_tail->is_cohead)) {
                // the read will depend on a writer ahead: give the writers ahead a timestamp first, then self (Bamboo
                // assigns only a cohead tail writer and only without waiters; RCC's queue can have waiters with no
                // owner, and an unassigned writer ahead would then never be wounded -> a commit-wait cycle)
                for (RCCLockEntry * en = retired_head; en; en = en->next)
                    if (en->type == LOCK_EX) assign_ts(en->txn->get_ts(), en->txn);
                ts = assign_ts(ts, txn);
                wound_retired_rd(ts, txn);
            }   // [RRR][][] / empty: no dependency, no timestamp
        } else if (owners || retired_head) {   // LOCK_EX on an empty row: no timestamp needed
            if (owners) owner_ts = owners->txn->get_ts();
            ts = txn->get_ts();
            if (ts == 0) {
                if (owners) {
                    if (!retired_head) owner_ts = assign_ts(owner_ts, owners->txn);   // [ ][W][...]
                    ts = assign_ts(ts, txn);
                } else {                                                           // [RR/W][][]
                    for (RCCLockEntry * en = retired_head; en; en = en->next) assign_ts(0, en->txn);
                    ts = assign_ts(ts, txn);
                }
            }
            wound_retired_wr(ts, txn);
            if (owners && (owner_ts == 0 || a_higher_than_b(ts, owner_ts))) wound_owner(txn);
        }
    }
// the ordered insert scans the waiters: prevention work, as Bamboo marks it (BB_PF_SCAN in its add_to_waiters)
#define RCC_ADD_WAITER(en) { RC_PH_BEGIN(rc_aw, RC_PH_PREV) add_to_waiters_ts(en); RC_PH_END(rc_aw, BB_PW_CS) }
#else
#define RCC_ADD_WAITER(en) add_to_waiters(en)
#endif
    // FIFO
    inline void add_to_waiters(RCCLockEntry * to_insert) {
        LIST_PUT_TAIL(waiters_head, waiters_tail, to_insert);
        to_insert->status = LOCK_WAITER;
        to_insert->woken = false;   // [wake-one fix] cleared when (re)queued
        to_insert->txn->lock_ready = false;
        waiter_cnt++;
    };

	// // NOTE: it is unrealistic to have completely ordered read with
	// // dynamically assigned ts. e.g. [0,0,0] -> [12, 11, 5]
	// // used when to_insert->type = LOCK_SH
	// inline RC wound_retired_rd(ts_t ts, RCCLockEntry * to_insert) {
	// 	RCCLockEntry * en = retired_head;
	// 	while(en) {
	// 		if (en->type == LOCK_EX && a_higher_than_b(ts, en->txn->get_ts())) {
	// 			if (to_insert->txn->wound_txn(en->txn) == COMMITED) {
	// 				//return_entry(to_insert);
	// 				//return Abort;
  //                   en = en->next;
  //                   continue;
	// 			}
	// 			en = rm_from_retired(en, true, to_insert->txn);
	// 		} else 
	// 			en = en->next;
	// 	}
	// 	return RCOK;
	// };

  // // Wound-Wait: before appending, kill the entries with a larger ts than mine
	// // try_wound(to_wound, wounder), if commited, wound failed, return wounder
	// inline RC wound_retired_wr(ts_t ts, RCCLockEntry * to_insert) {
	// 	RCCLockEntry * en = retired_head;
	// 	while(en) {
	// 		if (en->txn->get_ts() == 0 || a_higher_than_b(ts, en->txn->get_ts())) {
	// 			if (to_insert->txn->wound_txn(en->txn) == COMMITED) {
	// 				//return_entry(to_insert);
	// 				//return Abort;
  //                   en = en->next;
  //                   continue;
	// 			}
	// 			en = rm_from_retired(en, true, to_insert->txn);
	// 		} else 
	// 			en = en->next;
	// 	}
	// 	return RCOK;
	// };

	// inline RC wound_owner(RCCLockEntry * to_insert) {
	// 	if (to_insert->txn->wound_txn(owners->txn) == COMMITED) {
	// 		//return_entry(to_insert);
	// 		//return Abort;
  //           return WAIT;
	// 	}
	// 	return_entry(owners);
	// 	owners = NULL;
	// 	return RCOK;
	// };

    // Abort state excludes a version from consumption. The physical entry
    // remains a dependency barrier until its row cleanup has propagated abort.
    inline bool vs_dead(RCCLockEntry * en) {
#if RCCB
        if (en->crel) return false;   // committed; its owner's current attempt is another one
#endif
        return (en->txn->commit_barriers & 3UL) == ABORTED;
    };
    // Called under the row latch, before either direct admission or promotion.
    // Scan once for both ordering and data dependencies. Any observed aborted
    // writer blocks admission until row cleanup removes it. A source selected
    // while live stays selected if it aborts later; the source status recheck
    // then marks this consumer instead of silently substituting the base value.
    inline bool select_grant(RCCLockEntry *&vprev, RCCLockEntry *&vsrc) {
        // Keep physical ordering, including aborted readers, until cleanup.
        vprev = retired_tail;
        vsrc = NULL;
        // [abort-seq] no abort marked anywhere since the last clean scan => no dead entry can exist here.
        const uint64_t s0 = __atomic_load_n(&g_abort_seq, __ATOMIC_SEQ_CST);
        if (s0 == clean_seq) {
            for (RCCLockEntry *en = retired_tail; en; en = en->prev)
                if (en->type == LOCK_EX) {
                    vsrc = en; break;
                }
            return true;
        }
        for (RCCLockEntry *en = retired_tail; en; en = en->prev) {
            const bool dead = vs_dead(en);
            if (dead) {
                if (en->type == LOCK_EX) return false;
                continue;
            }
            if (!vsrc && en->type == LOCK_EX) {
                vsrc = en;
            }
        }
        clean_seq = s0;
        return true;
    };

// // owner's type is always LOCK_EX
// #define WOUND_OWNER(to_insert) { 
//     TRY_WOUND(owners, to_insert); 
//     return_entry(owners); 
//     owners = NULL; 
// }


};
#endif
