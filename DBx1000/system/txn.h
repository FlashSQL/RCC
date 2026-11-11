#pragma once

#include "global.h"
//#include "bloom_filter.h"
#include <unordered_map>
#include <unordered_set>
#include <stack>
#include "manager.h"
#include "row.h"
#include "rcc_srdet.h"
#include <atomic>
#include <memory>



class index_base;
class workload;
class thread_t;
class row_t;
class table_t;
class base_query;
class INDEX;
class txn_man;
struct BBLockEntry;
#if CC_ALG == WOUND_WAIT || CC_ALG == WAIT_DIE || CC_ALG == NO_WAIT || CC_ALG == DL_DETECT
struct LockEntry;
#elif CC_ALG == BAMBOO
//struct BBLockEntry;
#elif CC_ALG == RCC
struct RCCLockEntry;
#endif

// each thread has a txn_man.
// a txn_man corresponds to a single transaction.

//For VLL
enum TxnType {VLL_Blocked, VLL_Free};


class Access {
public:
    access_t 	type;       // operation type
    row_t * 	data;       // real data of this tuple version[In write operation, it records the new data]

    row_t * 	orig_row;
    row_t * 	orig_data;

#if CC_ALG == BAMBOO
    BBLockEntry * lock_entry;
#elif CC_ALG == WOUND_WAIT || CC_ALG == WAIT_DIE || CC_ALG == NO_WAIT || CC_ALG == DL_DETECT
    LockEntry * lock_entry;
#elif CC_ALG == RCC
    RCCLockEntry * lock_entry;
    Access * rb_next;   // [RCC_BATCH] link in its owner's pool of returned Access objects
#elif CC_ALG == TICTOC
    ts_t 		wts;
    ts_t 		rts;
#elif CC_ALG == SILO || CC_ALG == MOCC
    ts_t 		tid;
    ts_t 		epoch;
#elif CC_ALG == HEKATON
    void * 	history_entry;
#elif CC_ALG == IC3
    ts_t *    tids;
    ts_t      epochs;
    uint64_t  tid;
    uint64_t  rd_accesses;
    uint64_t  wr_accesses;
    uint64_t  lk_accesses;
#endif
#if COMMUTATIVE_OPS
    // support increment-only for now
    uint64_t  com_val;
    int       com_col;
    com_t     com_op;
#endif
    void cleanup();
};

#if CC_ALG == IC3
struct TxnEntry {
    txn_man * txn;
    uint64_t txn_id;
};
#endif

#if CC_ALG == MOCC
struct RLL {
    int lt;
    row_t *row;
    bool state;
};
#endif

class txn_man
{
public:
    // **************************************
    // General Data Fields
    // **************************************
    // update per txn
#if LATCH == LH_MCSLOCK
    mcslock::mcs_node * mcs_node;
#elif LATCH == LH_MUTEX
    pthread_mutex_t * latch;
#else
#endif

    thread_t *          h_thd;
    workload *          h_wl;
    txnid_t 		    txn_id;
    uint64_t            abort_cnt;          // Actually this attribute is useless because no one accesses it.

    bool readonly = false;
    bool read_committed = false;

    // update per request
    row_t * volatile    cur_row;
    // not used while having no inserts
//    uint64_t 		    insert_cnt;
//#if INSERT_ENABLED
//    row_t * 		    insert_rows[MAX_ROW_PER_TXN];
//#else
//    row_t *             insert_rows[1];
//#endif
    // ideal: one cache line

    // **************************************
    // Individual Data Fields
    // **************************************

    // [DL_DETECT, NO_WAIT, WAIT_DIE, WOUND_WAIT, BAMBOO]
    bool volatile       lock_ready;
    // [row reclamation] aborted new rows wait here until no worker can still reference them
    static const int RECLAIM_RING = 4096;
    row_t *  m_ret_rows[RECLAIM_RING];
    uint64_t m_ret_tags[RECLAIM_RING];
    int m_ret_head = 0, m_ret_tail = 0;
    void retire_row_for_reclaim(row_t * row) {
        int nxt = (m_ret_tail + 1) % RECLAIM_RING;
        if (nxt == m_ret_head) return;   // ring full: leak this one rather than block
        m_ret_rows[m_ret_tail] = row; m_ret_tags[m_ret_tail] = g_reclaim_epoch; m_ret_tail = nxt;
    }
    void reclaim_rows();
    bool is_aborting() {
#if CC_ALG == RCC || CC_ALG == BAMBOO
        return (commit_barriers & 3UL) == ABORTED;
#else
        return lock_abort || status == ABORTED;
#endif
    }
    bool volatile       lock_abort;         // forces another waiting txn to abort.
    status_t volatile   status;         // RUNNING, COMMITED, ABORTED, HOLDING
#if PF_ABORT
    uint64_t            abort_chain;
    uint8_t             padding0[64 - sizeof(bool)*2 - sizeof(status_t)- sizeof(uint64_t)];
    bool                wound;
    bool                wound_cascad;
//    uint64_t            start_sys_clock ;
    void                insert_wound(std::string ss);
#else
    uint8_t             padding0[64 - sizeof(bool)*2 - sizeof(status_t)];
#endif
    // ideal second cache line

    // [BAMBOO]
    ts_t volatile       timestamp;

    std::atomic<uint64_t>   timestamp_v;
    uint64_t                wait_latch_time;
    uint64_t                wait_passive_retire;
#if BB_PF_CWSPLIT
    // [cw-split] bb_cw_start / bb_commit_ts: this attempt's commit-wait start (= end of execution) and commit time.
    // bb_dec_*: written by the predecessor that releases this txn (update_entry): its two times and the release time.
    uint8_t             bb_cws_pad0[64];
    uint64_t volatile   bb_cw_start, bb_commit_ts, bb_dec_ts, bb_dec_pcw, bb_dec_pcommit;
    // flattened commit wait: what every transaction up the chain was doing (exec / release / notice), and chain depth;
    // bb_acc_* = mine (set at commit, before my rows are returned), bb_dec_acc_* = those of my releasing predecessor
    uint64_t volatile   bb_acc_ex, bb_acc_re, bb_acc_no, bb_acc_depth;
    uint64_t volatile   bb_dec_acc_ex, bb_dec_acc_re, bb_dec_acc_no, bb_dec_acc_depth;
    uint8_t             bb_cws_pad1[64];
#endif

    uint8_t             padding1[64 - sizeof(ts_t)];
    // share its own cache line since it happens tooooo frequent.
    // bamboo -- combine both status and barrier into one int.
    // low 2 bits representing status
    uint64_t volatile   commit_barriers;
#if CC_ALG == RCC
    // [measurement] cascade mark time. Stamped by the set_abort(CASCADE) winner; when the victim's
    // attempt ends, thread.cpp accumulates (endtime - this value) into time_abort_notice.
    uint64_t volatile   abort_mark_ts;
    // [measurement: propagation/notice latency] mark_ts = time I was marked ABORTED (stamped by the winner).
    // commit_wait_t0 = commit-wait entry time. last_dec_ts = commit time of the pred that issued
    // the last DEC (stamped by the DEC caller: that pred's finish time).
    uint64_t volatile   mark_ts;
    uint64_t volatile   last_dec_ts;
    // [layout] Close the commit_barriers cache line (the 4 x 8B fields above = 32B) here.
    // Otherwise cl_txn[0..3] below share the line, and a consumer's remote cascade_reg
    // store invalidates the line the victim CAS-spins on plus the ATOM_SUB line of every pred
    // (found in a t64 audit). padding2 lies after this block and cannot prevent this.
    uint8_t             padding_cb[64 - 4 * sizeof(uint64_t)];
#if CC_ALG == RCC
    // A detector pins the attempt while checking its ID and marking it.
    // Cascade propagation itself follows row cleanup, as in Bamboo.
    volatile uint32_t rcc_abort_users;
    void rcc_pin_abort() {
        for (;;) {
            uint32_t n = __atomic_load_n(&rcc_abort_users, __ATOMIC_ACQUIRE);
            if (n & 0x80000000U) { PAUSE; continue; }
            if (__atomic_compare_exchange_n(&rcc_abort_users, &n, n + 1, false,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        }
    }
    void rcc_unpin_abort() {
        __atomic_fetch_sub(&rcc_abort_users, 1U, __ATOMIC_RELEASE);
    }
    void rcc_lock_reuse() {
        for (;;) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&rcc_abort_users, &expected, 0x80000000U,
                                             false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            PAUSE;
        }
    }
    void rcc_unlock_reuse() {
        __atomic_store_n(&rcc_abort_users, 0U, __ATOMIC_RELEASE);
    }
    void check_version_source(txn_man *consumer) {
        // The supplying row latch keeps both attempts and the captured version
        // alive. Close the selection/abort race without walking other rows.
        if ((__atomic_load_n(&commit_barriers, __ATOMIC_SEQ_CST) & 3UL) == ABORTED)
            consumer->set_abort(ABORT_SRC_CASCADE);
    }

#endif
#else
    uint8_t             padding2[64 - sizeof(uint64_t)];
#endif
    //uint64_t volatile   tmp_barriers;
    //volatile uint64_t * volatile addr_barriers;
    int                 retire_threshold;

    // [BAMBOO-AUTORETIRE, OCC]
    uint64_t 		    start_ts; // bamboo: update once per txn
    // [OCC]
    uint64_t 		    end_ts;


    int 			    row_cnt;                // the count of tuples I access
    int	 		        wr_cnt;                 // the count of tuples I modify

    Access **		    accesses;
    int 			    num_accesses_alloc;

    // [TIMESTAMP, MVCC]
    bool volatile       ts_ready;
    // [HSTORE]
    int volatile 	    ready_part;
    // [TICTOC]
    bool                _write_copy_ptr;

#if BB_TRACK_DEPENDENS
    std::unordered_set<uint64_t> *i_depend_set;
    tbb::concurrent_unordered_set<uint64_t> *dependend_on_me;
#endif

#if CC_ALG == TICTOC
    bool			    _atomic_timestamp;
    ts_t 			    _max_wts;
    ts_t 			    last_wts;
    ts_t 			    last_rts;
    bool 			    _pre_abort;
    bool 			    _validation_no_wait;
    // [SILO]
#elif CC_ALG == SILO || CC_ALG == MOCC
    ts_t 			    last_tid;
    ts_t 			    _cur_tid;
    bool 			    _pre_abort;
    bool 			    _validation_no_wait;
    // [IC3]
#elif CC_ALG == IC3
    TPCCTxnType         curr_type;
    volatile int        curr_piece;
    int                 access_marker;
    TxnEntry **         depqueue;
    int                 depqueue_sz;
    uint64_t            piece_starttime;
    // [HEKATON]
#elif CC_ALG == HEKATON
//    volatile void * volatile     history_entry;
    void * volatile    history_entry;
#endif

    // **************************************
    // General Main Functions
    // **************************************
    virtual void        init(thread_t * h_thd, workload * h_wl, uint64_t  part_id);
    void                release();
    virtual RC 		    run_txn(base_query * m_query) = 0;
    RC 			        finish(RC rc);
    void 			    cleanup(RC rc);

    // **************************************
    // General Helper Functions
    // **************************************
    // getters
    uint64_t 		    get_thd_id();
    workload * 		    get_wl();
    txnid_t 		    get_txn_id();
    ts_t 			    get_ts();
    // setters
    void 			    set_txn_id(txnid_t txn_id);
    void 			    set_ts(ts_t timestamp);
    void			    reassign_ts();

    // **************************************
    // Individual Helper Functions
    // **************************************

    // [COMMUTATIVE OPERATIONS]
#if COMMUTATIVE_OPS
    void                inc_value(int col, uint64_t val);
    void                dec_value(int col, uint64_t val);
#endif
    // [WW, BAMBOO]
    // if already abort, no change, return aborted
    // if already commit, no change, return committed
    // if running, set abort, return aborted.
    // 1:exec deadlock check abort,  2:exec deadlock check abort,  3:valid deadlock check abort, 4:valid rw abort, 5:valid cascading abort
#if CC_ALG == RCC
    // Mark this attempt only; its row cleanups propagate to local successors.
    status_t rcc_mark_abort(uint32_t cascading=0,
                           bool *won_result=nullptr, txnid_t expected_id=(txnid_t)-1,
                           uint64_t *marked_at=nullptr) {
        if (marked_at) *marked_at = 0;
        rcc_pin_abort();
        if (expected_id != (txnid_t)-1 && get_txn_id() != expected_id) {
            if (won_result) *won_result = false;
            rcc_unpin_abort();
            return COMMITED; // old target is gone; do not modify the new attempt
        }
        uint64_t local = commit_barriers;
        uint64_t barriers = local >> 2;
        uint64_t s = local & 3UL;
        // only the caller that wins the RUNNING->ABORTED transition owns this
        // abort; others merely observe it and must not count it again.
        bool won = false;
        // cannot use atom_add:
        // (1) what if two txns both atomic add? may change from abort to commit
        // (2) moreover, may exceed two bits
        while (s == RUNNING) {
            if (ATOM_CAS(commit_barriers, local, (barriers << 2) + ABORTED)) {
                if (cascading == ABORT_SRC_CASCADE)
                    abort_mark_ts = get_sys_clock();
                mark_ts = get_sys_clock();
                __atomic_store_n(&lock_abort, true, __ATOMIC_RELEASE);
                __atomic_fetch_add(&g_abort_seq, 1, __ATOMIC_SEQ_CST);   // [abort-seq]
                won = true;
            }
            local = commit_barriers;
            barriers = local >> 2;
            s = local & 3UL;
        }
        if (won_result) *won_result = won;
        if (s == ABORTED) {
            if (marked_at) *marked_at = mark_ts;
            if (!lock_abort)
                lock_abort = true;
#if PF_MODEL
            if (won) {
                if (cascading == ABORT_SRC_CASCADE)     { INC_STATS(get_thd_id(), cascading_abort_cnt, 1); }
                else if (cascading == ABORT_SRC_WOUND)  { INC_STATS(get_thd_id(), wound_abort_cnt, 1); }
            }
#endif
            rcc_unpin_abort();
            return ABORTED;
        } else if (s == COMMITED) {
            rcc_unpin_abort();
            return COMMITED;
        } else {
            assert(false);
            rcc_unpin_abort();
            return COMMITED;
        }
    }
#endif
    status_t   set_abort(uint32_t cascading=0, bool *won_result=nullptr, txnid_t expected_id=(txnid_t)-1, uint64_t *marked_at=nullptr) {
#if CC_ALG == RCC
        return rcc_mark_abort(cascading, won_result, expected_id, marked_at);
#elif CC_ALG == BAMBOO
        uint64_t local = commit_barriers;
        uint64_t barriers = local >> 2;
        uint64_t s = local & 3UL;
        // cannot use atom_add:
        // (1) what if two txns both atomic add? may change from abort to commit
        // (2) moreover, may exceed two bits
        bool won = false;
        while (s == RUNNING) {
            if (ATOM_CAS(commit_barriers, local, (barriers << 2) + ABORTED))
                won = true;
            local = commit_barriers;
            barriers = local >> 2;
            s = local & 3UL;
//            assert(s == ABORTED);
        }
        if (won_result) *won_result = won;
        if (s == ABORTED) {

            if (!lock_abort)
                lock_abort = true;
#if PF_MODEL
            if (cascading)
                INC_STATS(get_thd_id(), cascading_abort_cnt, 1);
#endif
            (void) won;
            return ABORTED;
        } else if (s == COMMITED) {
            return COMMITED;
        } else {
            assert(false);
            return COMMITED;
        }
#elif CC_ALG == WOUND_WAIT || CC_ALG == IC3
        if (ATOM_CAS(status, RUNNING, ABORTED)) {
            lock_abort = true;
            return ABORTED;
       }
       return status;       // COMMITED or ABORTED
#else
        return ABORTED;
#endif
    }

    status_t            wound_txn(txn_man * txn);

    void                increment_commit_barriers();
    void                decrement_commit_barriers();

    // dynamically set timestamp
    bool                atomic_set_ts(ts_t ts);
    ts_t			    set_next_ts(int n);
    ts_t                set_next_ts() ;
    // auto retire
    ts_t                get_exec_time() {return get_sys_clock() - start_ts;};
#if CC_ALG == BAMBOO || CC_ALG == RCC
    RC                  retire_row(int access_cnt);
#endif
#if CC_ALG == RCC
    volatile int wait_ctx;
    volatile bool rcc_recheck;
    bool rcc_published;
    int rcc_scan_idx;   // [publish only dependencies] accesses already offered to rcc_publish in this attempt
    void rcc_publish_entries();
    // [ERW] conflict edges this attempt logged to the serialization detector (into itself); none: it cannot be on a
    // cycle and approves itself
    uint32_t rccs_nedges;
    bool rccs_self_ok;
#if RCC_ERW_ON
    // [ERW] this attempt's dependency records (its TLA): "I depend on pred, relation kind". The worker only appends
    // to its own object; the serialization detector visits every transaction object and reads the new records.
    // Protocol: a new attempt stores rs_rec_n = 0, then rs_rec_att = id, then appends (record, then rs_rec_n + 1);
    // the detector reads att, n, the records, then att again and keeps them only if att did not change.
    struct RsRec { uint64_t pred; uint32_t kind; uint32_t pad; };
    enum { RS_REC_MAX = 4096 };
    // one cache line of its own: the detector reads it every round, and the fields around it (row_cnt, wait_ctx, ...)
    // are written by the worker on every access
    alignas(64) volatile uint64_t rs_rec_att;
    volatile uint32_t rs_rec_n;
    RsRec * rs_rec;
    uint8_t rs_pad1[64 - 2 * sizeof(uint64_t) - sizeof(RsRec *)];
    inline void rs_record(uint64_t pred, uint32_t kind) {
        const uint32_t n = rs_rec_n;
        if (likely(n < RS_REC_MAX)) {
            rs_rec[n].pred = pred; rs_rec[n].kind = kind;
            __atomic_store_n(&rs_rec_n, n + 1, __ATOMIC_RELEASE);
            rcc_srdet.notify();   // [wake] edge recorded: wake the serialization detector if it sleeps
        } else rcc_srdet.log(get_thd_id(), RSE_EDGE, pred, get_txn_id(), kind);   // full: through the event log (it notifies)
    }
#endif
#if RCCB
    // [RCC_BATCH dep push] the write dependents of this attempt (they read or overwrote its versions): its commit
    // decrements their commit barriers itself, without any row latch. word = epoch << 32 | sealed | aborted | count;
    // epoch = the low 32 bits of the attempt id. A dependent reserves a slot by CAS (refused once sealed or for another
    // epoch), then publishes it; commit or abort seals the list and waits for every reserved slot to be published.
    struct RbDepSlot { txn_man * volatile dep; volatile uint64_t dep_id; volatile uint32_t state; uint32_t pad; };
    enum { RB_DEP_MAX = 1024 };
    static const uint64_t RB_SEALED = 1ULL << 31;
    static const uint64_t RB_ABORTED = 1ULL << 30;
    static const uint64_t RB_CNT = (1ULL << 30) - 1;
    // rb_register results: registered / that attempt committed or ended (nothing to wait for) / it aborted / list full
    enum { RB_REG_OK = 0, RB_REG_ENDED, RB_REG_ABORTED, RB_REG_FULL };
    volatile uint64_t rb_dep_word;
    RbDepSlot * rb_deps;
    int rb_register(txn_man * pred, uint32_t pred_epoch);    // this attempt depends on that attempt of pred
    void rb_push_commit();                                    // committed: decrement every registered dependent
    void rb_seal_drain();                                     // aborted: refuse registrations, drain reservations
    // [RCC_BATCH async release] Access objects whose entries a committed attempt left in row queues belong to those
    // queues until swept; the sweeper returns them here (any thread pushes, the owner takes them all at once)
    Access * volatile rb_ret_head;
    Access ** rb_free;
    uint32_t rb_free_n, rb_free_cap;
    Access * rb_spare();                                      // owner: a free Access (pool first, else a new one)
    void rb_return(Access * a);                               // any thread
#endif
#endif
    // [VLL]
    row_t * 		    get_row(row_t * row, access_t type);
//    itemid_t *	        index_read(INDEX * index, idx_key_t key, int part_id);
//    void 			    index_read(INDEX * index, idx_key_t key, int part_id, itemid_t *& item);
    // [IC3]
    void                begin_piece(int piece_id);
    RC                  end_piece(int piece_id);
    void                abort_ic3();
    int                 get_txn_pieces(int tpe);

#if CC_ALG == IC3
    RC                  validate_ic3();
    // [TICTOC]
#elif CC_ALG == TICTOC
    RC				    validate_tictoc();
    ts_t 			    get_max_wts() 	{ return _max_wts; }
    void 			    update_max_wts(ts_t max_wts);
    // [Hekaton]
#elif CC_ALG == HEKATON
    RC 				    validate_hekaton(RC rc);
    // [SILO]
#elif CC_ALG == SILO
    RC			   validate_silo();
#elif (CC_ALG == MOCC)
	int 			step = 0;
	RC 				validate_mocc();
#endif

    RC              validate();
    // Bamboo and RCC run it after commit wait, right before the commit CAS (a predecessor applies its index changes
    // before it releases its dependents)
    bool            commit_check();
#if CC_ALG == RCC
    inline bool     rcc_commit_check() { if (commit_check()) return true; set_abort(ABORT_SRC_SELF); return false; }
#endif
#if CC_ALG == BAMBOO
    inline bool     bb_commit_check() { if (commit_check()) return true; set_abort(); return false; }
#endif

    itemid_t *		index_read(index_base * index, idx_key_t key, int part_id);
    void 			index_read(index_base * index, idx_key_t key, int part_id, itemid_t *& item);
    RC				index_read_multiple(index_base* index, idx_key_t key, itemid_t** items, size_t& count, int part_id);
    RC				index_read_range(index_base* index, idx_key_t min_key, idx_key_t max_key, itemid_t** items, size_t& count, int part_id);
    RC				index_read_range_rev(index_base* index, idx_key_t min_key, idx_key_t max_key, itemid_t** items, size_t& count, int part_id);
    RC              apply_index_changes(RC rc, bool validated=false);
    bool            insert_idx(index_base* index, uint64_t key, row_t* row, int part_id);
    bool            remove_idx(index_base* index, uint64_t key, row_t* row, int part_id);
    row_t*          search(index_base* index, size_t key, int part_id, access_t type);

#if CC_ALG == MOCC
    bool is_locked(uint64_t key);
    void remove_non_cononical_lock(uint64_t key);
    void insert_cononical_lock(int lt, row_t *row);
    void remove_cononical_lock(uint64_t key);
    void unlock_read_locks_all();
    void clear_lock_state(RC rc);
    bool track_perf_sig = false;
    int	lock_rd_cnt = 0;
#endif

protected:
    void 			    insert_row(row_t * row, table_t * table);
//    void                index_insert(row_t * row, INDEX * index, idx_key_t key);
    bool 			    remove_row(row_t* row);

private:
#if CC_ALG == BAMBOO || CC_ALG == WOUND_WAIT || CC_ALG == WAIT_DIE || CC_ALG == NO_WAIT || CC_ALG == DL_DETECT || CC_ALG == RCC
    void                assign_lock_entry(Access * access);
#endif
    // insert rows
    uint64_t 		insert_cnt;
    row_t * 		insert_rows[MAX_ROW_PER_TXN];
    uint64_t 		remove_cnt;
    row_t * 		remove_rows[MAX_ROW_PER_TXN];

    // insert/remove indexes
    uint64_t 		   insert_idx_cnt;
    index_base*   insert_idx_idx[MAX_ROW_PER_TXN];
    idx_key_t	     insert_idx_key[MAX_ROW_PER_TXN];
    row_t* 		     insert_idx_row[MAX_ROW_PER_TXN];
    int	       	   insert_idx_part_id[MAX_ROW_PER_TXN];
#if CC_ALG == SILO
    // [SILO] index inserts deferred to the validation (validate_silo, write set locked and validated): an OCC transaction
    // takes no lock while it executes, so two TPC-C New-Orders read the same D_NEXT_O_ID; inserting at once made the
    // second abort on the duplicate key -- and again on every retry while the first was still running (84-95% of the
    // aborts). Deferred, the loser fails the validation of the district once, as in OCC.
    uint64_t       silo_pend_cnt = 0;
    index_base*    silo_pend_idx[MAX_ROW_PER_TXN];
    idx_key_t      silo_pend_key[MAX_ROW_PER_TXN];
    itemid_t*      silo_pend_item[MAX_ROW_PER_TXN];
    row_t*         silo_pend_row[MAX_ROW_PER_TXN];
    int            silo_pend_part[MAX_ROW_PER_TXN];
    RC             silo_apply_inserts();   // under the write-set locks, after the validation (silo.cpp)
#endif

    uint64_t 		   remove_idx_cnt;
    index_base*   remove_idx_idx[MAX_ROW_PER_TXN];
    idx_key_t	     remove_idx_key[MAX_ROW_PER_TXN];
    int	      	   remove_idx_part_id[MAX_ROW_PER_TXN];

#if CC_ALG == MOCC
    RLL cur_lock_list[MAX_ROW_PER_TXN];
	int cur_lock_list_head = 0;
#endif

};


inline status_t txn_man::wound_txn(txn_man * txn)
{
#if CC_ALG == BAMBOO || CC_ALG == WOUND_WAIT || CC_ALG == RCC
    auto ret = txn->set_abort();
#if PF_CS
    INC_STATS(this->get_thd_id(), find_circle_abort_depent, 1);
#endif
    return ret;
#else
    return ABORTED;
#endif
}
