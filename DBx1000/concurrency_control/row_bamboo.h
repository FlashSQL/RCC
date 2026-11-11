#ifndef ROW_BAMBOO_H
#define ROW_BAMBOO_H

// BB_PF_PREVENT: the time lock_get holds the row latch (time_get_cs), split into exclusive parts:
//   time_bb_ts    timestamp assignment (assign_ts)
//   time_bb_wound wound_txn on a victim and removing it (and its descendants) from the queue
//   time_bb_copy  copying the version a read gets from a retired / owner write
//   time_bb_scan  walking the retired / waiter lists (wound checks, ts stamping, insert position), minus the above
// Charged to the worker running lock_get (bb_pf_tid, set there), not to the txn the work is for: assign_ts also stamps
// owners / retired entries. bb_pf_nested sums the parts recorded so far, so a scan around them subtracts them.
#if BB_PF_AHEAD
extern thread_local uint64_t bb_ahead_last;   // owner + waiters ahead at this worker's last add_to_waiters
#endif
#if BB_PF_PREVENT
extern thread_local uint64_t bb_pf_tid;
extern thread_local uint64_t bb_pf_nested;
#define BB_PF_START(v) uint64_t v = get_sys_clock();
#define BB_PF_END(name, v) { uint64_t bb_pf_d = get_sys_clock() - (v); INC_TMP_STATS(bb_pf_tid, name, bb_pf_d); bb_pf_nested += bb_pf_d; }
#define BB_PF_CNT(name) INC_TMP_STATS(bb_pf_tid, name, 1);
#define BB_PF_SCAN_START_T(v) uint64_t v = get_sys_clock(); uint64_t v##_n = bb_pf_nested;
#define BB_PF_SCAN_END_T(v) { uint64_t bb_pf_d = get_sys_clock() - (v) - (bb_pf_nested - v##_n); \
    INC_TMP_STATS(bb_pf_tid, time_bb_scan, bb_pf_d); bb_pf_nested += bb_pf_d; }
#else
#define BB_PF_START(v)
#define BB_PF_END(name, v)
#define BB_PF_CNT(name)
#define BB_PF_SCAN_START_T(v)
#define BB_PF_SCAN_END_T(v)
#endif

// BB_PF_HOLDER: the latch holder marks what it is doing in the row (bb_phase), so that a transaction waiting for the
// latch to return its rows after its commit (lock_release) can split that wait by the holder's work. lock_get points
// bb_ph at its row's bb_phase while it holds the latch; the parts inside lock_get push / pop their phase through it.
enum { BB_PH_FREE = 0,   // latch free when sampled (lost the race for it)
       BB_PH_OTHER,      // held by a path that does not mark itself
       BB_PH_GET,        // lock_get, none of the parts below (queue updates, copies, promotion)
       BB_PH_TS,         // lock_get: timestamp assignment
       BB_PH_SCAN,       // lock_get: walking the retired / waiter lists
       BB_PH_WOUND,      // lock_get: wounding
       BB_PH_RETIRE,     // lock_retire
       BB_PH_RELEASE,    // lock_release
       BB_PH_N };
#define BB_PHASES (BB_PF_HOLDER || BB_PF_PW)
#define BB_PH_PREV(p) ((p) == BB_PH_TS || (p) == BB_PH_SCAN || (p) == BB_PH_WOUND)
#if BB_PF_HOLDER && BB_PF_PW
#error "BB_PF_HOLDER and BB_PF_PW cannot be on together"
#endif
#if BB_PF_PW
// [BB_PF_PW] the phase is this worker's own (bb_ph points to a thread-local byte, not into the row): nothing is shared
// at run time.  The outermost prevention part (pushed over BB_PH_GET) is logged as own prevention work and, with the
// latch, as a holder interval that splits the latch waits of others after the run (system/stats.h BB_PW_LATCH_RAW).
extern thread_local volatile uint8_t * bb_ph;
extern thread_local uint64_t bb_pw_cs0;      // start of the outermost prevention part of this lock_get
extern thread_local uint64_t bb_pw_holder;   // txn id of the latch holder on this worker (wake events)
extern thread_local uint64_t bb_pw_lid;      // the latch this worker's lock_get holds
extern thread_local bool bb_pw_waited;       // the last lock_pw had to wait for the latch
extern thread_local bool bb_pw_open;         // this lock_get has entered prevention
extern thread_local uint64_t bb_pw_t_acq;    // when this lock_get took the latch (PF_CS endtime, ns)
extern thread_local uint64_t bb_pw_e;        // end of its latest prevention part (raw ticks)
// The prevention parts of one lock_get (timestamps, list scans + wounds, the waiter-queue insert) follow each other with
// only comparisons in between, and all of them before its grant work: they are logged as one piece, from the latch
// acquisition (its PF_CS timestamp, no new read) to the end of the last part (one rdtsc per part), at unlock.
#define BB_PH_PUSH(v, X) uint8_t v = 0; if (bb_ph) { v = *bb_ph; *bb_ph = (X); if (v == BB_PH_GET && !bb_pw_open) { bb_pw_open = true; bb_pw_cs0 = bb_pw_t_acq; } }
#define BB_PH_POP(v) if (bb_ph) { if (v == BB_PH_GET) bb_pw_e = bb_pw_ticks(); *bb_ph = v; }
#elif BB_PF_HOLDER
extern thread_local volatile uint8_t * bb_ph;
#define BB_PH_PUSH(v, X) uint8_t v = 0; if (bb_ph) { v = *bb_ph; *bb_ph = (X); }
#define BB_PH_POP(v) if (bb_ph) *bb_ph = v;
#else
#define BB_PH_PUSH(v, X)
#define BB_PH_POP(v)
#endif
#define BB_PF_SCAN_START(v) BB_PF_SCAN_START_T(v) BB_PH_PUSH(v##_ph, BB_PH_SCAN)
#define BB_PF_SCAN_END(v) BB_PH_POP(v##_ph) BB_PF_SCAN_END_T(v)

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
#define UPDATE_RETIRE_INFO(en, prev) { \
  if (prev) { \
    if (en->type == LOCK_EX) \
      en->txn->increment_commit_barriers(); \
    else { \
            if (prev->type == LOCK_SH) { \
              en->is_cohead = prev->is_cohead; \
              if (!en->is_cohead) { \
                en->txn->increment_commit_barriers(); \
              } \
            } else { \
              en->txn->increment_commit_barriers(); } \
    } \
  } else { \
    en->is_cohead = true; \
   } }

// used by lock_retire() (move from owners to retired)
// or by lock_get()/bring_next(), used when has no owners but directly enters retired
// for the latter need to call UPDATE_RETIRE_INFO(to_insert, retired_tail);
#define ADD_TO_RETIRED_TAIL(to_retire) { \
  LIST_PUT_TAIL(retired_head, retired_tail, to_retire); \
  to_retire->status = LOCK_RETIRED; \
  retired_cnt++; }

// Insert to_insert(RD) into the tail when owners is not empty
// (1) update inserted entry's cohead information
// (2) NEED to update owners cohead information 
//     if owner is not cohead, it cannot become one with RD inserted
//     if owner is cohead, RD becomes cohead and owner is no longer a cohead 
#define INSERT_TO_RETIRED_TAIL(to_insert) { \
  UPDATE_RETIRE_INFO(to_insert, retired_tail); \
  if (owners && owners->is_cohead) { \
    owners->is_cohead = false; \
    owners->txn->increment_commit_barriers(); \
  } \
  LIST_PUT_TAIL(retired_head, retired_tail, to_insert); \
  to_insert->status = LOCK_RETIRED; \
  retired_cnt++; }

#define RETIRE_ENTRY(to_retire) { \
  to_retire = owners; \
  owners = NULL; \
  to_retire->next=NULL; \
  to_retire->prev=NULL; \
  ADD_TO_RETIRED_TAIL(to_retire); }

#define CHECK_ROLL_BACK(en) { \
    en->access->orig_row->copy(en->access->orig_data); \
}

#if BB_PF_CWSPLIT   // bb_rel = the releasing txn (update_entry)
#define BB_CWS_MARK(entry) { entry->txn->bb_dec_pcw = bb_rel->bb_cw_start; \
    entry->txn->bb_dec_pcommit = bb_rel->bb_commit_ts; \
    entry->txn->bb_dec_acc_ex = bb_rel->bb_acc_ex; entry->txn->bb_dec_acc_re = bb_rel->bb_acc_re; \
    entry->txn->bb_dec_acc_no = bb_rel->bb_acc_no; entry->txn->bb_dec_acc_depth = bb_rel->bb_acc_depth; \
    entry->txn->bb_dec_ts = get_sys_clock(); }
#else
#define BB_CWS_MARK(entry)
#endif
#if BB_PF_CWX   // a release that brings the successor's barrier to 0 is logged (bb_rel = releasing txn, update_entry)
#define BB_DEC(entry) { uint64_t bb_nv = ATOM_SUB_FETCH(entry->txn->commit_barriers, 1UL << 2); \
    if ((bb_nv >> 2) == 0) bb_cwx_release_event(entry->txn->get_txn_id(), bb_rel->get_txn_id(), get_sys_clock()); }
#elif BB_PF_PW
#define BB_DEC(entry) { uint64_t bb_nv = ATOM_SUB_FETCH(entry->txn->commit_barriers, 1UL << 2); \
    if ((bb_nv >> 2) == 0) bb_pw_release_event(entry->txn->get_txn_id(), bb_rel->get_txn_id(), get_sys_clock()); }
#else
#define BB_DEC(entry) entry->txn->decrement_commit_barriers();
#endif
#define DEC_BARRIER_PF(entry) { \
    assert(!entry->is_cohead); \
    entry->is_cohead = true; \
    BB_CWS_MARK(entry) \
    uint64_t starttime = get_sys_clock(); \
    BB_DEC(entry) \
    INC_STATS(entry->txn->get_thd_id(), time_semaphore_cs, \
        get_sys_clock() - starttime); \
}

#define DEC_BARRIER(entry) { \
    assert(!entry->is_cohead); \
    entry->is_cohead = true; \
    BB_CWS_MARK(entry) \
    BB_DEC(entry) \
}

struct BBLockEntry {
    // type of lock: EX or SH
    txn_man * txn;
    Access * access;
    //uint8_t padding[64 - sizeof(void *)*2];
    lock_t type;
    uint8_t padding[64 - sizeof(void *)*2 - sizeof(lock_t)];
    BBLockEntry * next;
    //uint8_t padding[64 - sizeof(void *)*3 - sizeof(lock_t)];
    bool is_cohead;
    lock_status status;
    BBLockEntry * prev;
    BBLockEntry(txn_man * t, Access * a): txn(t), access(a), type(LOCK_NONE),
                                          next(NULL), is_cohead(false),
                                          status(LOCK_DROPPED),
                                          prev(NULL) {};
};

class Row_bamboo {
  public:
    void init(row_t * row);
    RC lock_get(lock_t type, txn_man * txn, Access * access);
    RC lock_release(BBLockEntry * entry, RC rc);
    RC lock_retire(BBLockEntry * entry);

  private:
    // data structure
    BBLockEntry * owners;
    BBLockEntry * retired_head;
    BBLockEntry * retired_tail;
    BBLockEntry * waiters_head;
    BBLockEntry * waiters_tail;
    row_t * _row;
    UInt32 waiter_cnt;
    UInt32 retired_cnt;
    // latches
#if LATCH == LH_SPINLOCK
    pthread_spinlock_t * latch;
#elif LATCH == LH_MUTEX
    pthread_mutex_t * latch;
#else
    mcslock * latch;
#endif
    bool blatch;
#if BB_PHASES
    volatile uint8_t * bb_php;   // what the latch holder is doing (BB_PH_*), in the latch's cache line
#endif
#if BB_PF_HOLDER
    void              lock_sampled(txn_man * txn, uint64_t t0, bool commit);   // lock_release: latch wait split by holder
#endif
#if BB_PF_PW
    void              lock_pw();   // the latch as lock(), noting whether it had to wait (bb_pw_waited)
#endif

    // helper functions
    bool              bring_next(txn_man * txn);
    void              update_entry(BBLockEntry * en);
    BBLockEntry *     rm_from_retired(BBLockEntry * en, bool is_abort, txn_man * txn);
    BBLockEntry *     remove_descendants(BBLockEntry * en, txn_man * txn);
    void              lock(txn_man * txn);
    void              unlock(txn_man * txn);
	RC                insert_read_to_retired(BBLockEntry * to_insert, ts_t ts, Access * access);
#if DEBUG_BAMBOO
	void              check_correctness();
#endif

    // check priorities
    inline static bool a_higher_than_b(ts_t a, ts_t b) {
        return a < b;
    };

    inline static int assign_ts(ts_t ts, txn_man * txn) {
        if (ts == 0) {
            BB_PF_START(pf_t0)
            BB_PH_PUSH(ph_ts, BB_PH_TS)
            ts = txn->set_next_ts(1);
            // if fail to assign, reload
            if ( ts == 0 )
                ts = txn->get_ts();
            BB_PH_POP(ph_ts)
            BB_PF_END(time_bb_ts, pf_t0)
            BB_PF_CNT(bb_ts_cnt)
        }
        return ts;
    };


    // init a lock entry (pre-allocated in each txn's access)
    static BBLockEntry * get_entry(Access * access) {
    #if CC_ALG == BAMBOO
        BBLockEntry * entry = access->lock_entry;
        entry->txn->lock_ready = false;
        // dont init lock_abort, can only be set true but not false. 
        entry->next = NULL;
        entry->prev = NULL;
        entry->status = LOCK_DROPPED;
        entry->is_cohead = false;
        return entry;
    #else
        return NULL;
    #endif
    };


    // clean the lock entry
    void return_entry(BBLockEntry * entry) {
        entry->next = NULL;
        entry->prev = NULL;
        entry->status = LOCK_DROPPED;
    };

	inline bool bring_out_waiter(BBLockEntry * entry, txn_man * txn) {
		LIST_RM(waiters_head, waiters_tail, entry, waiter_cnt);
		entry->txn->lock_ready = true;
#if BB_PF_PW
		bb_pw_wake(entry->txn->get_txn_id(), bb_pw_holder, get_sys_clock());
#endif
		if (txn == entry->txn) {
			return true;
		}
		return false;
	};

	inline void add_to_waiters(ts_t ts, BBLockEntry * to_insert) {
		BB_PF_SCAN_START(pf_s0)
#if BB_PF_AHEAD
		uint64_t ahead = owners ? 1 : 0;
#endif
		BBLockEntry * en = waiters_head;
		while (en != NULL) {
			if (ts < en->txn->get_ts())
					break;
			en = en->next;
#if BB_PF_AHEAD
			ahead++;
#endif
		}
#if BB_PF_AHEAD
		bb_ahead_last = ahead;
#endif
		BB_PF_SCAN_END(pf_s0)
		if (en) {
			LIST_INSERT_BEFORE(en, to_insert);
			if (en == waiters_head) 
				waiters_head = to_insert;
		} else {
			LIST_PUT_TAIL(waiters_head, waiters_tail, to_insert);
		}
		to_insert->status = LOCK_WAITER;
		to_insert->txn->lock_ready = false;
		waiter_cnt++;
        assert(ts != 0);
	};	

	// NOTE: it is unrealistic to have completely ordered read with
	// dynamically assigned ts. e.g. [0,0,0] -> [12, 11, 5]
	// used when to_insert->type = LOCK_SH
	inline RC wound_retired_rd(ts_t ts, BBLockEntry * to_insert) {
		BB_PF_SCAN_START(pf_s0)
		BBLockEntry * en = retired_head;
		while(en) {
			if (en->type == LOCK_EX && a_higher_than_b(ts, en->txn->get_ts())) {
				BB_PF_CNT(bb_wound_cnt)
				BB_PF_START(pf_w0)
				BB_PH_PUSH(ph_w, BB_PH_WOUND)
				if (to_insert->txn->wound_txn(en->txn) == COMMITED) {
					//return_entry(to_insert);
					//return Abort;
					BB_PH_POP(ph_w) BB_PF_END(time_bb_wound, pf_w0)
                    en = en->next;
                    continue;
				}
				en = rm_from_retired(en, true, to_insert->txn);
				BB_PH_POP(ph_w) BB_PF_END(time_bb_wound, pf_w0)
			} else
				en = en->next;
		}
		BB_PF_SCAN_END(pf_s0)
		return RCOK;
	};

	// try_wound(to_wound, wounder), if commited, wound failed, return wounder
	inline RC wound_retired_wr(ts_t ts, BBLockEntry * to_insert) {
		BB_PF_SCAN_START(pf_s0)
		BBLockEntry * en = retired_head;
		while(en) {
			if (en->txn->get_ts() == 0 || a_higher_than_b(ts, en->txn->get_ts())) {
				BB_PF_CNT(bb_wound_cnt)
				BB_PF_START(pf_w0)
				BB_PH_PUSH(ph_w, BB_PH_WOUND)
				if (to_insert->txn->wound_txn(en->txn) == COMMITED) {
					//return_entry(to_insert);
					//return Abort;
					BB_PH_POP(ph_w) BB_PF_END(time_bb_wound, pf_w0)
                    en = en->next;
                    continue;
				}
				en = rm_from_retired(en, true, to_insert->txn);
				BB_PH_POP(ph_w) BB_PF_END(time_bb_wound, pf_w0)
			} else
				en = en->next;
		}
		BB_PF_SCAN_END(pf_s0)
		return RCOK;
	};

	inline RC wound_owner(BBLockEntry * to_insert) {
		BB_PF_START(pf_t0)
		BB_PH_PUSH(ph_w, BB_PH_WOUND)
		BB_PF_CNT(bb_wound_cnt)
		if (to_insert->txn->wound_txn(owners->txn) == COMMITED) {
			//return_entry(to_insert);
			//return Abort;
			BB_PH_POP(ph_w) BB_PF_END(time_bb_wound, pf_t0)
            return WAIT;
		}
		return_entry(owners);
		owners = NULL;
		BB_PH_POP(ph_w) BB_PF_END(time_bb_wound, pf_t0)
		return RCOK;
	};

// owner's type is always LOCK_EX
#define WOUND_OWNER(to_insert) { \
    TRY_WOUND(owners, to_insert); \
    return_entry(owners); \
    owners = NULL; \
}


};
#endif
