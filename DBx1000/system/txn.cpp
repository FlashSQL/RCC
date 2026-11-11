#include "txn.h"
#include "rcc_srdet.h"

#include "wl.h"
#include "ycsb.h"
#include "thread.h"
#include "mem_alloc.h"
#include "occ.h"
#include "table.h"
#include "catalog.h"
#include "index_btree.h"
#include "index_hash.h"
#include "index_mbtree.h"
// for info of lock entry
#include "row_lock.h"
#include "row_bamboo.h"
#include "row_rcc.h"
#include "rcc_parallel_cleanup.h"
#include "row_mocc.h"
#include <unordered_map>

extern thread_local std::unordered_map<void*, uint64_t>  node_map;

void txn_man::init(thread_t * h_thd, workload * h_wl, uint64_t thd_id) {
    this->h_thd = h_thd;
    this->h_wl = h_wl;
    lock_ready = false;
    lock_abort = false;
    timestamp = 0;
#if CC_ALG == RCC
    commit_barriers = 0;
    wait_ctx = 0;
    abort_mark_ts = mark_ts = last_dec_ts = 0;
    rcc_recheck = false;
    rcc_published = false;
    rcc_scan_idx = 0;
    rcc_abort_users = 0;
    rccs_nedges = 0; rccs_self_ok = false;
#endif
#if RCC_ERW_ON
    rs_rec = (RsRec *) _mm_malloc(sizeof(RsRec) * RS_REC_MAX, 64);
    rs_rec_n = 0; rs_rec_att = (uint64_t) -1;
#endif
#if RCCB
    rb_dep_word = 0;
    rb_deps = (RbDepSlot *) _mm_malloc(sizeof(RbDepSlot) * RB_DEP_MAX, 64);
    for (int i = 0; i < RB_DEP_MAX; i++) { rb_deps[i].dep = NULL; rb_deps[i].dep_id = 0; rb_deps[i].state = 0; }
    rb_ret_head = NULL;
    rb_free_cap = 64; rb_free_n = 0;
    rb_free = (Access **) malloc(sizeof(Access *) * rb_free_cap);
#endif
#if CC_ALG == BAMBOO
    commit_barriers = 0;
#if BB_TRACK_DEPENDENS
    i_depend_set = new std::unordered_set<uint64_t>();
    dependend_on_me = new tbb::concurrent_unordered_set<uint64_t>();
#endif
#endif
    ready_part = 0;
    row_cnt = 0;
    wr_cnt = 0;
    insert_cnt = 0;
    remove_cnt = 0;
    insert_idx_cnt = 0;
    remove_idx_cnt = 0;
#if CC_ALG == MOCC
    cur_lock_list_head = 0;
    track_perf_sig = false;
    lock_rd_cnt = 0;
#endif

    // init accesses
    accesses = (Access **) _mm_malloc(sizeof(Access *) * MAX_ROW_PER_TXN, 64);
    for (int i = 0; i < MAX_ROW_PER_TXN; i++)
        accesses[i] = NULL;

    wait_latch_time = 0;
    wait_passive_retire = 0;
    num_accesses_alloc = 0;

#if CC_ALG == TICTOC || CC_ALG == SILO || CC_ALG == MOCC
    _pre_abort = (g_params["pre_abort"] == "true");
    if (g_params["validation_lock"] == "no-wait")
        _validation_no_wait = true;
    else if (g_params["validation_lock"] == "waiting")
        _validation_no_wait = false;
    else
        assert(false);
#endif
#if CC_ALG == TICTOC
    _max_wts = 0;
    _write_copy_ptr = (g_params["write_copy_form"] == "ptr");
    _atomic_timestamp = (g_params["atomic_timestamp"] == "true");
#elif CC_ALG == SILO || CC_ALG == MOCC
    _cur_tid = 0;
#elif CC_ALG == IC3
    depqueue = (TxnEntry **) _mm_malloc(sizeof(void *)*THREAD_CNT, 64);
  for (int i = 0; i < THREAD_CNT; i++)
    depqueue[i] = NULL;
  depqueue_sz = 0;
  piece_starttime = 0;
#endif
}

void txn_man::set_txn_id(txnid_t txn_id) {
#if CC_ALG == SILO
    silo_pend_cnt = 0;   // a new attempt: nothing deferred yet
#endif
#if CC_ALG == RCC
    rcc_lock_reuse();
#endif
#if RCCB
    // [RCC_BATCH] a new, open dependent list; a late registration for the previous attempt sees another epoch
    __atomic_store_n(&rb_dep_word, ((uint64_t)(uint32_t) txn_id) << 32, __ATOMIC_RELEASE);
#endif
#if CC_ALG == WOUND_WAIT || CC_ALG == BAMBOO || CC_ALG == DL_DETECT || CC_ALG == RCC
    lock_abort = false;
    lock_ready = false;
    status = RUNNING;
#if CC_ALG == BAMBOO || CC_ALG == RCC
    commit_barriers = 0;
    if (g_last_retire > 0)
        start_ts = get_sys_clock();
#endif
#if (CC_ALG == BAMBOO || CC_ALG == WOUND_WAIT || CC_ALG == RCC) && BB_PF_PW
    bb_pw_attempt_begin(txn_id);
#endif
#if BB_PF_CWSPLIT
    bb_cw_start = bb_commit_ts = bb_dec_ts = bb_dec_pcw = bb_dec_pcommit = 0;
    bb_acc_ex = bb_acc_re = bb_acc_no = bb_acc_depth = 0;
    bb_dec_acc_ex = bb_dec_acc_re = bb_dec_acc_no = bb_dec_acc_depth = 0;
#endif
#if CC_ALG == RCC
    abort_mark_ts = mark_ts = last_dec_ts = 0;
    rcc_recheck = false;
    rccs_nedges = 0; rccs_self_ok = false;
#endif
#endif
#if RCC_ERW_ON
    // a new, empty record for this attempt: count first, then the id (the detector validates by the id)
    __atomic_store_n(&rs_rec_n, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&rs_rec_att, (uint64_t) txn_id, __ATOMIC_RELEASE);
#endif
#if CC_ALG == IC3
    status = RUNNING;
    depqueue_sz = 0;
#endif
    this->txn_id = txn_id;
#if LATCH == LH_MCSLOCK
    mcs_node = new mcslock::mcs_node();
#endif
#if CC_ALG == RCC
    rcc_unlock_reuse();
#endif
}

txnid_t txn_man::get_txn_id() {
    return this->txn_id;
}

workload * txn_man::get_wl() {
    return h_wl;
}

uint64_t txn_man::get_thd_id() {
    return h_thd->get_thd_id();
}

bool txn_man::atomic_set_ts(ts_t ts) {
    return ATOM_CAS(timestamp, 0, ts);
}

uint64_t txn_man::set_next_ts(int n) {
    if (atomic_set_ts(h_thd->get_next_n_ts(n))) {
        return this->timestamp;
    } else {
        return 0; // fail to set timestamp
    }
}
uint64_t txn_man::set_next_ts() {
    if (atomic_set_ts(h_thd->get_next_ts())) {
        return this->timestamp;
    } else {
        return 0; // fail to set timestamp
    }
}
void txn_man::reassign_ts() {
    this->timestamp = h_thd->get_next_n_ts(1);
}

void txn_man::set_ts(ts_t timestamp) {
    this->timestamp = timestamp;
}

ts_t txn_man::get_ts() {
    return this->timestamp;
}

void txn_man::reclaim_rows() {
    uint64_t min_e = (uint64_t)-1;
    for (uint32_t i = 0; i < g_thread_cnt; i++) if (g_thread_epoch[i].e < min_e) min_e = g_thread_epoch[i].e;
    while (m_ret_head != m_ret_tail && m_ret_tags[m_ret_head] < min_e) {
        row_t * row = m_ret_rows[m_ret_head];
        mem_allocator.free(row->manager, 0);
        row->free_row();
        mem_allocator.free(row, sizeof(row));
        m_ret_head = (m_ret_head + 1) % RECLAIM_RING;
    }
}


void txn_man::cleanup(RC rc) {
#if RCC_ERW_ON
    // [ERW] the attempt ends: a committed node stays in the graph while it has a predecessor, an aborted one leaves
#if RCC_ERW_EDGE_SEND
    { extern void rccs_drop_edges(); rccs_drop_edges(); }   // edges of an attempt that ended before sending them
#endif
#if RCC_ERW_PROBE || BB_PF_PW
    const uint64_t rp_cl0 = get_sys_clock();
#endif
    rcc_srdet.log(get_thd_id(), rc == RCOK ? RSE_COMMIT : RSE_ABORT, get_txn_id());
#if BB_PF_PW
    if (rc == RCOK) bb_pw_own(rp_cl0, get_sys_clock(), BB_PW_DCS);   // telling the detector about the commit
#endif
#if RCC_ERW_PROBE
    if (rc == RCOK) { extern uint64_t rp_txn[THREAD_CNT + 1][8]; rp_txn[get_thd_id()][6] += get_sys_clock() - rp_cl0; }
#endif
    // committed without incoming edges: mark every entry before any of them is released, so no later access logs a
    // useless edge from this attempt while the detector has not processed its commit yet
    if (rc == RCOK && rccs_self_ok)
        for (int rid = 0; rid < row_cnt; rid++)
            if (accesses[rid]->orig_row) accesses[rid]->lock_entry->rs_clean = true;
#endif
#if CC_ALG == RCC
    // Mark self before index/row work. Each aborted writer cleanup marks
    // its same-row successors before removing their entries.
    if (rc == Abort && (commit_barriers & 3UL) == RUNNING)
        set_abort(ABORT_SRC_SELF);
#if RCCB
    if (rc == Abort) rb_seal_drain();   // late registrations see the abort; this attempt's row cleanup marks the rest
#endif
    // finish() already validated before commit admission; index undo/publication uses the shared eager path.
    rc = apply_index_changes(rc, true);
#endif
    // Aborted: remove the index keys inserted during execution BEFORE the rows are released. A successor that gets a
    // released row (in TPC-C the district, with D_NEXT_O_ID put back) derives the same keys again; had it found this
    // attempt's keys still in the index, its insert would hit a duplicate of an aborted transaction.
    if (rc == Abort) {
        for (size_t i = 0; i < insert_idx_cnt; i++)
            insert_idx_idx[i]->index_remove(insert_idx_key[i], insert_idx_part_id[i]);
        insert_idx_cnt = 0;
    }
#if RCCB
    if (rc == RCOK) {
        // [RCC_BATCH] dep push: return the barrier of every write dependent now, without row latches (after the index
        // changes, as the row release did). Then release asynchronously: each entry is marked committed and swept by
        // whoever holds its row latch next (one install per batch). An entry still queued keeps its Access, which the
        // sweeper returns to this transaction's pool; the next attempt takes another one.
        rb_push_commit();
        for (int rid = row_cnt - 1; rid >= 0; rid--) {
            Access * a = accesses[rid];
            row_t * r = a->orig_row;
            if (r == NULL) continue;
            a->orig_row = NULL;   // before the hand-off: after it, the Access belongs to the queue
            RCCLockEntry * e = a->lock_entry;
            if (r->manager->lock_release_async(e, this)) continue;
            if (__atomic_exchange_n(&e->handoff, (uint8_t) 1, __ATOMIC_SEQ_CST) == 0)
                accesses[rid] = rb_spare();
            else   // a sweeper claimed it meanwhile: it is ours again once DROPPED is published
                while (__atomic_load_n(&e->status, __ATOMIC_ACQUIRE) != LOCK_DROPPED) PAUSE
        }
    } else
#endif
#if CC_ALG == RCC && RCC_PARALLEL_CLEANUP
    // Publish only after commit admission and index changes. The join inside
    // rcc_cleanup_rows keeps this attempt and all Access/private buffers alive.
    if (rc == RCOK) rcc_cleanup_rows(this);
    else
#endif
    // go through accesses and release
    for (int rid = row_cnt - 1; rid >= 0; rid --) {
#if (CC_ALG == WOUND_WAIT) || (CC_ALG == BAMBOO) || (CC_ALG == RCC)
        if (accesses[rid]->orig_row == NULL) {
            continue;
        }
#endif

        row_t * orig_r = accesses[rid]->orig_row;
        access_t type = accesses[rid]->type;

#if COMMUTATIVE_OPS
        if (accesses[rid]->com_op != COM_NONE && (rc != Abort)) {
                if (accesses[rid]->com_op == COM_INC)
                    orig_r->inc_value(accesses[rid]->com_col, accesses[rid]->com_val);
                else
                    orig_r->dec_value(accesses[rid]->com_col, accesses[rid]->com_val);
                accesses[rid]->com_op = COM_NONE;
            }
#endif

        if (type == WR && rc == Abort)
            type = XP;

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
        if (type == RD) {
            accesses[rid]->data = NULL;
            continue;
        }
#endif

#if COMMUTATIVE_OPS && !COMMUTATIVE_LATCH
        if (type != CM) {
#endif

#if   CC_ALG == BAMBOO || CC_ALG == RCC
        orig_r->return_row(accesses[rid]->lock_entry, rc);
        accesses[rid]->orig_row = NULL;
#elif CC_ALG == WOUND_WAIT
        orig_r->return_row(type, accesses[rid]->data, accesses[rid]->lock_entry);
        accesses[rid]->orig_row = NULL;
#elif CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE
        if (ROLL_BACK && type == XP) {
        orig_r->return_row(type, this, accesses[rid]->orig_data);
    } else {
        orig_r->return_row(type,this, accesses[rid]->data);
    }
#else
        orig_r->return_row(type, this, accesses[rid]->data);
#endif

#if COMMUTATIVE_OPS && !COMMUTATIVE_LATCH
        }
#endif

#if CC_ALG != TICTOC && (CC_ALG != SILO) && (CC_ALG != WOUND_WAIT) && (CC_ALG!= BAMBOO) && (CC_ALG != MOCC) && (CC_ALG != RCC)
            // invalidate ptr for cc keeping globally visible ptr
            accesses[rid]->data = NULL;
#endif
    }

    if (rc == Abort ) {
        for (UInt32 i = 0; i < insert_cnt; i ++) {
            row_t * row = insert_rows[i];
            assert(g_part_alloc == false);
            row->is_deleted = 1;

            asm volatile ("sfence" ::: "memory");
            retire_row_for_reclaim(row);   // freed once every worker has moved to a newer attempt
            // [fix] do NOT free the manager. insert_idx published this row in the index while we were
            // still running, so another thread can be holding row->manager right now - in TPC-C,
            // Stock-Level range-scans ORDER-LINE and reads exactly the rows New-Order inserts. Freeing
            // it under them is a use-after-free: at 20 threads RCC died in pthread_spin_unlock inside
            // Row_rcc::lock_release, unlocking a latch whose memory had already been recycled. The row
            // itself is deliberately not freed either (see the commented-out free below); is_deleted is
            // what retires it, and apply_index_changes has already removed the key by this point.
//            row->free_row();
//            mem_allocator.free(row, sizeof(row));
        }
    }


    row_cnt = 0;
    wr_cnt = 0;
    insert_cnt = 0;
    insert_cnt = 0;
    remove_cnt = 0;

#if CC_ALG == MOCC
    lock_rd_cnt = 0;
#endif

    insert_idx_cnt = 0;
    remove_idx_cnt = 0;
    node_map.clear();

#if CC_ALG == DL_DETECT
    dl_detector.clear_dep(get_txn_id());
#endif

#if CC_ALG == RCC
    if (rcc_published) dl_detector.rcc_clear(get_txn_id());   // never published => the slot is already empty
    rcc_published = false;
    rcc_scan_idx = 0;
#endif
}



#if CC_ALG == BAMBOO || CC_ALG == WOUND_WAIT || CC_ALG == WAIT_DIE || CC_ALG == NO_WAIT || CC_ALG == DL_DETECT || CC_ALG == RCC
inline
void txn_man::assign_lock_entry(Access * access) {
#if CC_ALG == RCC
    auto lock_entry = (RCCLockEntry *) _mm_malloc(sizeof(RCCLockEntry), 64);
    new (lock_entry) RCCLockEntry(this, access);
#elif CC_ALG == BAMBOO
#if PF_CS
    uint64_t starttime  = get_sys_clock();
#endif
    auto lock_entry = (BBLockEntry *) _mm_malloc(sizeof(BBLockEntry), 64);
    new (lock_entry) BBLockEntry(this, access);
#if PF_CS
    INC_STATS(this->get_thd_id(), time_creat_entry, get_sys_clock() - starttime);
#endif
#else
#if PF_CS
    uint64_t starttime  = get_sys_clock();
#endif
    auto lock_entry = (LockEntry *) _mm_malloc(sizeof(LockEntry), 64);
    new (lock_entry) LockEntry(this, access);
#if PF_CS
    INC_STATS(this->get_thd_id(), time_creat_entry, get_sys_clock() - starttime);
#endif
#endif
    access->lock_entry = lock_entry;
    //lock_entry->txn = this;
    //lock_entry->access = access;
}
#endif

#if RCCB
// [RCC_BATCH dep push] reserve a slot in pred's list for that attempt (epoch), then publish it.
int txn_man::rb_register(txn_man * pred, uint32_t pred_epoch) {
    uint64_t w = __atomic_load_n(&pred->rb_dep_word, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((uint32_t) (w >> 32) != pred_epoch) return RB_REG_ENDED;   // that attempt ended and its cleanup is done
        if (w & RB_SEALED) return (w & RB_ABORTED) ? RB_REG_ABORTED : RB_REG_ENDED;
        const uint64_t n = w & RB_CNT;
        if (n >= RB_DEP_MAX) return RB_REG_FULL;
        if (__atomic_compare_exchange_n(&pred->rb_dep_word, &w, w + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            RbDepSlot & s = pred->rb_deps[n];
            s.dep = this;
            s.dep_id = get_txn_id();
            __atomic_store_n(&s.state, 1U, __ATOMIC_RELEASE);
            return RB_REG_OK;
        }
    }
}

// Committed (status COMMITED): seal, then return one barrier to each registered dependent that is still that attempt
// (pinned, so it cannot move on between the check and the decrement).
void txn_man::rb_push_commit() {
    const uint64_t w = __atomic_fetch_or(&rb_dep_word, RB_SEALED, __ATOMIC_ACQ_REL);
    const uint64_t n = w & RB_CNT;
    for (uint64_t i = 0; i < n; i++) {
        RbDepSlot & s = rb_deps[i];
        while (__atomic_load_n(&s.state, __ATOMIC_ACQUIRE) != 1U) PAUSE
        txn_man * d = s.dep;
        const uint64_t id = s.dep_id;
        s.state = 0;
        d->rcc_pin_abort();
        if (d->get_txn_id() == id) d->decrement_commit_barriers();
        d->rcc_unpin_abort();
    }
}

// Aborted: seal with the abort bit (a late registration aborts itself); the registered dependents are marked by this
// attempt's row cleanup. Wait for every reserved slot so the next attempt starts from an empty list.
void txn_man::rb_seal_drain() {
    const uint64_t w = __atomic_fetch_or(&rb_dep_word, RB_SEALED | RB_ABORTED, __ATOMIC_ACQ_REL);
    if (w & RB_SEALED) return;
    const uint64_t n = w & RB_CNT;
    for (uint64_t i = 0; i < n; i++) {
        RbDepSlot & s = rb_deps[i];
        while (__atomic_load_n(&s.state, __ATOMIC_ACQUIRE) != 1U) PAUSE
        s.state = 0;
    }
}

// [RCC_BATCH async release] owner only: an Access for the slot whose Access stayed in a row queue.
Access * txn_man::rb_spare() {
    if (rb_free_n == 0) {
        Access * l = __atomic_exchange_n(&rb_ret_head, (Access *) NULL, __ATOMIC_ACQUIRE);
        for (; l; l = l->rb_next) {
            if (rb_free_n == rb_free_cap) {
                rb_free_cap *= 2;
                rb_free = (Access **) realloc(rb_free, sizeof(Access *) * rb_free_cap);
            }
            rb_free[rb_free_n++] = l;
        }
    }
    Access * a;
    if (rb_free_n) {
        a = rb_free[--rb_free_n];
    } else {
        a = (Access *) _mm_malloc(sizeof(Access), 64);
        assign_lock_entry(a);
        a->data = (row_t *) _mm_malloc(sizeof(row_t), 64);
        a->data->init(MAX_TUPLE_SIZE);
        a->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
        a->orig_data->init(MAX_TUPLE_SIZE);
    }
    a->orig_row = NULL;
    a->rb_next = NULL;
    return a;
}

// any thread (the sweeper of an entry whose owner gave its Access up)
void txn_man::rb_return(Access * a) {
    Access * h = __atomic_load_n(&rb_ret_head, __ATOMIC_RELAXED);
    do {
        a->rb_next = h;
    } while (!__atomic_compare_exchange_n(&rb_ret_head, &h, a, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}
#endif

/**
 * Record this operation in Accesses set and call corresponding get_row() to actually access the row
 * @param row : the row accesses by the txn [Full: with data]
 * @param type : access type
 * @return
 */
#if CC_ALG == RCC
// [RCC pre-commit publish] Publish the lock entries this txn currently holds to its detector slot.
// Edges (blockers) are derived from entry->pred by each detection round at that time, so the slot
// needs no fix-up when a blocker commits. The slot is reclaimed by rcc_clear in cleanup.
//
// Without RCC_ADD the only publish point is the commit-wait spin (the CC_ALG==RCC branch of finish);
// with RCC_ADD a txn also publishes after each get_row once it depends on another txn.
// Publishing on every successful get_row (eager publication) would put running txns that are
// not blocked yet into the graph, so a cycle would mean "deadlock if nothing changes" rather than
// "deadlock now". Such predicted cycles mostly resolve themselves, so false positives explode and
// a persistence filter becomes necessary. A/B measurement (TPCC NUM_WH=1, t=32):
//   eager      -> rcc_false_positive 357,082 / round 26.3us / detector wall 67.8%
//   pre-commit -> rcc_false_positive   8,068 / round 13.8us / detector wall 52.4%
// Aborts, cascades and throughput were statistically identical; rcc_lock_timeout_cnt stayed 0.
//
// Caveat: a txn that never reaches commit-wait (i.e. one stuck in lock-wait) is not visible
// in the graph. A cycle through it is caught only by the RCC_LOCK_TIMEOUT backstop, so
// check that rcc_lock_timeout_cnt stays 0 whenever the workload changes. In the measurement above
// it is 0 because the benchmark retires writes immediately, so no txn blocks while holding
// an owner entry (same reasoning as the waiter-edge comment in rcc_entry_dep).
void txn_man::rcc_publish_entries() {
#if RCC_ERW_ON
    return; // [ERW] the serialization detector gets explicit edges from the access log instead
#endif
    // [lazy publication] A txn that never had a pred has no outgoing edge and cannot be
    // a cycle member; edges into it are dropped by the round on a slot txnid mismatch
    // (the stale-edge check in dl_detect.cpp). Publishing is therefore deferred until
    // the first pred appears. A conflict-free txn never calls publish/clear.
    //
    // Why this single point suffices: a pred is set only when the entry joins owners/retired
    // (UPDATE_RETIRE_INFO, by a direct join or a bring_next promotion), and the join
    // always precedes the successful return of that get_row. RCC has no path that later
    // flips an already granted entry from cohead to non-cohead
    // (rm_from_retired only reassigns an existing pred).
#if CC_ALG == RCC && RCC_ADD
    // Publish after each successful get_row once a dependency has appeared.
    // rcc_publish reads only newly appended accesses; an unchanged count is O(1).
    if (!rcc_published && (commit_barriers >> 2) == 0)
        return;                        // never blocked yet -> nothing to publish
#else
    if (rcc_published)
        return;                        // the list does not change during commit-wait
    if ((commit_barriers >> 2) == 0)
        return;                        // never blocked yet -> nothing to publish
#endif
    // Pass the access array directly: rebuilding a temporary prefix here made
    // publishing n rows quadratic even though the slot itself appended one row.
    dl_detector.rcc_publish(get_txn_id(), accesses, row_cnt, rcc_scan_idx);
    rcc_published = true;
}

#endif

row_t * txn_man::get_row(row_t * row, access_t type) {
    if (CC_ALG == HSTORE)
        return row;

    uint64_t starttime = get_sys_clock();
    RC rc = RCOK;

    if (accesses[row_cnt] == NULL) {
        assert(row_cnt < MAX_ROW_PER_TXN);
        Access *access = (Access *) _mm_malloc(sizeof(Access), 64);

#if COMMUTATIVE_OPS
        // init
                access->com_op = COM_NONE;
#endif

        accesses[row_cnt] = access;

#if   (CC_ALG == SILO || CC_ALG == TICTOC || CC_ALG == MOCC)
        access->data = (row_t *) _mm_malloc(sizeof(row_t), 64);
        access->data->init(MAX_TUPLE_SIZE);
        access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
        access->orig_data->init(MAX_TUPLE_SIZE);
#elif (CC_ALG == IC3)
        access->data = (row_t *) _mm_malloc(sizeof(row_t), 64);
            access->data->init(MAX_TUPLE_SIZE);
            #if IC3_FIELD_LOCKING
                access->tids = (ts_t *) _mm_malloc(sizeof(ts_t) * MAX_FIELD_SIZE, 64);
#else
                access->tid = 0;
            #endif
#elif (CC_ALG == WOUND_WAIT)
        // allocate lock entry as well
            assign_lock_entry(access);
#if PF_CS
        uint64_t starttime  = get_sys_clock();
#endif
            // for ww and bb, data is a local copy of original row for txn to work on
            access->data = (row_t *) _mm_malloc(sizeof(row_t), 64);
            access->data->init(MAX_TUPLE_SIZE);
#if PF_CS
        INC_STATS(this->get_thd_id(), time_creat_entry, get_sys_clock() - starttime);
#endif
#elif (CC_ALG == BAMBOO) || (CC_ALG == RCC)
        // allocate lock entry as well
            assign_lock_entry(access);
#if PF_CS
           uint64_t starttime  = get_sys_clock();
#endif
            // data is for making local changes before added to retired
            access->data = (row_t *) _mm_malloc(sizeof(row_t), 64);
            access->data->init(MAX_TUPLE_SIZE);
            access->data->table = row->get_table();
            // orig data is for rollback
            access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
            access->orig_data->init(MAX_TUPLE_SIZE);
            access->orig_data->table = row->get_table();
#if PF_CS
        INC_STATS(this->get_thd_id(), time_creat_entry, get_sys_clock() - starttime);
#endif
#elif (CC_ALG == DL_DETECT || (CC_ALG == NO_WAIT) || (CC_ALG == WAIT_DIE))
        // allocate lock entry as well
            assign_lock_entry(access);
            access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
            access->orig_data->init(MAX_TUPLE_SIZE);
#endif
        num_accesses_alloc++;
    }

    if (row->is_deleted) {
        return NULL;
    }

#if CC_ALG == RCC
    accesses[row_cnt]->data->table = row->get_table();
#endif
#if (CC_ALG == WOUND_WAIT) || (CC_ALG == BAMBOO) || (CC_ALG == RCC)
    rc = row->get_row(type, this, accesses[ row_cnt ]->orig_row, accesses[row_cnt]);
    if (rc == Abort) {
        accesses[row_cnt]->orig_row = NULL;
        return NULL;
    }
#elif CC_ALG == DL_DETECT || (CC_ALG == NO_WAIT) || (CC_ALG == WAIT_DIE)
    rc = row->get_row(type, this, accesses[ row_cnt ]->data, accesses[row_cnt]);
    if (rc == Abort){
        return NULL;
    }
    accesses[row_cnt]->orig_row = row;
#elif CC_ALG == IC3
    assert(rc == RCOK);
        // re-initialize read/write sets for the tuple.
        accesses[row_cnt]->rd_accesses = 0;
        accesses[row_cnt]->wr_accesses = 0;
        accesses[row_cnt]->lk_accesses = 0;
        accesses[row_cnt]->data->init_accesses(accesses[row_cnt]);
        accesses[row_cnt]->data->manager = row->manager;
        accesses[row_cnt]->data->table = row->get_table();
        accesses[row_cnt]->data->orig = row;
        accesses[row_cnt]->orig_row = row;
        #if !IC3_FIELD_LOCKING
            row->get_row(type, this, row, accesses[row_cnt]);
        #endif
#else
    rc = row->get_row(type, this, accesses[ row_cnt ]->data, accesses[row_cnt]);
    if (rc == Abort) {
        return NULL;
    }
    accesses[row_cnt]->orig_row = row;
#endif

#if (CC_ALG == BAMBOO && BB_OPT_RAW)
    if (rc == FINISH) {
        // RAW optimization
        accesses[row_cnt]->data->table = row->get_table();
    }
#endif

    /**
     * Set the operation type of a certain access [row->get_row() successfully, so we can set access object directly]
     */
    accesses[row_cnt]->type = type;

#if CC_ALG == TICTOC
    accesses[row_cnt]->wts = last_wts;
    accesses[row_cnt]->rts = last_rts;
#elif CC_ALG == SILO || CC_ALG == MOCC
    accesses[row_cnt]->tid = last_tid;
#elif CC_ALG == HEKATON
    accesses[row_cnt]->history_entry = history_entry;
#endif

    if (row->is_deleted) {
        // [fix] the lock was already granted when we noticed the row is gone. Returning here without
        // counting the access left the entry in the row's list forever, so every transaction queued
        // behind it waited in commit-wait until the run wedged. Count it: the caller aborts and
        // cleanup releases it like any other access.
#if CC_ALG == WOUND_WAIT || CC_ALG == BAMBOO || CC_ALG == RCC
        row_cnt++;
#endif
        return NULL;
    }

    if (type == WR) {
#if CC_ALG == WOUND_WAIT
#if PF_CS
        uint64_t startt = get_sys_clock();
#endif
        // make local copy to work on
        accesses[row_cnt]->data->table = row->get_table();
        accesses[row_cnt]->data->copy(row);
#if PF_CS
        INC_STATS(get_thd_id(), time_copy, get_sys_clock() - startt);
#endif
#elif CC_ALG == BAMBOO || CC_ALG == RCC
        #if PF_CS
        uint64_t startt = get_sys_clock();
        #endif
#if CC_ALG == RCC
        if (rc != FINISH) accesses[row_cnt]->data->copy(row);
#if !RCC_LUI
        // [!RCC_LUI] Bamboo: the before-image for the undo
        accesses[row_cnt]->orig_data->table = row->get_table();
        accesses[row_cnt]->orig_data->copy(row);
#endif
#else
        // make local copy to work on
        accesses[row_cnt]->data->table = row->get_table();
        accesses[row_cnt]->data->copy(row);
        // make copy to rollback
        accesses[row_cnt]->orig_data->table = row->get_table();
        accesses[row_cnt]->orig_data->copy(row);
#endif
        #if PF_CS
        INC_STATS(get_thd_id(), time_copy, get_sys_clock() - startt);
        #endif
#elif ROLL_BACK && (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
        accesses[row_cnt]->orig_data->table = row->get_table();
        accesses[row_cnt]->orig_data->copy(row);
#endif
    }

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
    if (type == RD)
        row->return_row(type, accesses[ row_cnt ]->data, accesses[row_cnt]->lock_entry);
#endif



    row_cnt++;
#if CC_ALG == RCC && RCC_ADD
#if BB_PF_PW
    { const uint64_t pw0 = get_sys_clock(); rcc_publish_entries(); bb_pw_iv(pw0, get_sys_clock(), BB_PW_DCS); }   // for the detector
#else
    rcc_publish_entries();
#endif
#endif
    INC_STATS(get_thd_id(), getrow_cnt, 1);
    if (type == WR) {
        wr_cnt++;
    }

    uint64_t timespan = get_sys_clock() - starttime;
    INC_TMP_STATS(get_thd_id(), time_man, timespan);

#if  (CC_ALG == WOUND_WAIT)
    if (type == WR)
        return accesses[row_cnt - 1]->data;
    else
        return accesses[row_cnt - 1]->orig_row;
#elif CC_ALG == BAMBOO
    //printf("txn %lu got row %p at %d-th access %p\n", get_txn_id(), (void *)accesses[row_cnt - 1]->orig_row, row_cnt - 1, (void *)accesses[row_cnt - 1]);
        if (type == WR)
            return accesses[row_cnt - 1]->data;
        else {
            if (rc != FINISH)
              return accesses[row_cnt - 1]->orig_row;
            else
              return accesses[row_cnt - 1]->data; // RAW
        }
#elif CC_ALG == IC3
    return accesses[row_cnt - 1]->data;
#elif CC_ALG == RCC
  // [version split] A write always uses the local copy. A read uses the copy if the pred version
  // snapshot was copied in at grant time (FINISH), otherwise (no conflict) reads the base directly;
  // the counterpart of MySQL's future_vers/rec branch on the presence of conf.
  if (type == WR)
    return accesses[row_cnt - 1]->data;
  else
    return (rc == FINISH) ? accesses[row_cnt - 1]->data
                          : accesses[row_cnt - 1]->orig_row;
#else
    return accesses[row_cnt - 1]->data;
#endif
}
bool txn_man::remove_row(row_t* row) {
    remove_rows[remove_cnt++] = row;
    return true;
}
void txn_man::insert_row(row_t * row, table_t * table) {
    if (CC_ALG == HSTORE)
        return;
    assert(insert_cnt < MAX_ROW_PER_TXN);
    insert_rows[insert_cnt ++] = row;
}

//void txn_man::index_insert(row_t * row, INDEX * index, idx_key_t key) {
//    //TODO(zhihan): insert row in the index.
//    uint64_t part_id = get_part_id(row);
//    itemid_t * m_item = (itemid_t *) mem_allocator.alloc( sizeof(itemid_t), part_id);
//    m_item->init();
//    m_item->type = DT_row;
//    m_item->location = row;
//    m_item->valid = true;
//#ifdef NDEBUG
//    index->index_insert(key, m_item, part_id);
//#else
//    assert(index->index_insert(key, m_item, part_id) == RCOK);
//#endif
//}

RC txn_man::apply_index_changes(RC rc, bool validated) {

#if WORKLOAD == TPCC
    if (rc == RCOK && !validated) rc = validate();
    if (rc != RCOK) {
        // Aborted: the keys inserted during execution are removed in cleanup(), before the rows are
        // released.
        return rc;
    }
    insert_idx_cnt = 0;
    for (size_t i = 0; i < remove_idx_cnt; i++) {
        auto idx = remove_idx_idx[i];
        auto key = remove_idx_key[i];
        auto part_id = remove_idx_part_id[i];
        // printf("remove_idx idx=%p key=%" PRIu64 " part_id=%d\n", idx, key, part_id);
        auto rc_remove = idx->index_remove(key, part_id);
        (void) rc_remove;
//        assert(rc_remove == RCOK);
    }

    remove_idx_cnt = 0;

    // Free deleted rows
    for (size_t i = 0; i < remove_cnt; i++) {
        auto row = remove_rows[i];
//        assert(!row->is_deleted);
        row->is_deleted = 1;
    }
    remove_cnt = 0;

#endif

    return rc;
}

row_t* txn_man::search(index_base* index, uint64_t key, int part_id, access_t type) {
    itemid_t * item = NULL;
    item = index_read(index, key, part_id);
    if (item == NULL) {
        return NULL;
    }

    // printf("%lld, %lld\n", ((row_t *)item->location)->index_cnt, h_wl->get_index_cnt(index));
    assert(((row_t *)item->location)->index_cnt == h_wl->get_index_cnt(index));

    // cur_key = key;
    auto item_row = (row_t *)item->location;
    return get_row(item_row, type);
}
itemid_t * txn_man::index_read(index_base * index, idx_key_t key, int part_id) {
    // h_wl->update_index_accessed(index);
    INC_STATS(get_thd_id(), idx_read_cnt, 1);
    uint64_t starttime = get_sys_clock();
    itemid_t * item = NULL;
    index->index_read(key, item, part_id, get_thd_id());
    INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
    return item;
}

void txn_man::index_read(index_base * index, idx_key_t key, int part_id, itemid_t *& item) {
    // h_wl->update_index_accessed(index);
    uint64_t starttime = get_sys_clock();
    index->index_read(key, item, part_id, get_thd_id());
    INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
}

RC txn_man::index_read_multiple(index_base* index, idx_key_t key, itemid_t** items, size_t& count, int part_id) {
    return index->index_read_multiple(key, items, count, part_id);
}

RC txn_man::index_read_range(index_base* index, idx_key_t min_key, idx_key_t max_key, itemid_t** items, size_t& count, int part_id) {
    return index->index_read_range(min_key, max_key, items, count, part_id);
}

RC txn_man::index_read_range_rev(index_base* index, idx_key_t min_key, idx_key_t max_key, itemid_t** items, size_t& count, int part_id) {
    return index->index_read_range_rev(min_key, max_key, items, count, part_id);
}

bool txn_man::insert_idx(index_base* index, uint64_t key, row_t* row, int part_id) {
    row->index_cnt = h_wl->get_index_cnt(index);
    row->inserter = this;
    row->inserter_id = get_txn_id();

    itemid_t * m_item = (itemid_t *) mem_allocator.alloc( sizeof(itemid_t), part_id);
    m_item->init();
    m_item->type = DT_row;
    m_item->location = row;
    m_item->valid = true;

#if CC_ALG == SILO
    // [SILO] deferred: inserted by silo_apply_inserts() once the validation succeeded (txn.h)
    assert(silo_pend_cnt < MAX_ROW_PER_TXN);
    silo_pend_idx[silo_pend_cnt] = index; silo_pend_key[silo_pend_cnt] = key; silo_pend_item[silo_pend_cnt] = m_item;
    silo_pend_row[silo_pend_cnt] = row; silo_pend_part[silo_pend_cnt] = part_id; silo_pend_cnt++;
    return true;
#endif
    RC rc_insert;
    uint64_t wait_start = 0;
    for (;;) {
        rc_insert = index->index_insert(key, m_item, part_id); // May fail if others also insert one.
        if (rc_insert != ERROR)
            break;
        // Duplicate key. If its owner is aborting, the key is removed by that owner's undo within
        // microseconds: wait for it instead of aborting (an abort here re-derives the same key on the
        // next attempt, which under heavy cascades chained into an abort storm).
        // A live owner is a real conflict: give up as before.
        itemid_t * ex = NULL;
        index->index_read(key, ex, part_id, get_thd_id());
        row_t * exr = ex ? (row_t *) ex->location : NULL;
        if (exr) {
            // Resolve the owner by id through the manager table instead of dereferencing the pointer
            // stored in the row: a loaded row or a recycled buffer can carry garbage there (Bamboo
            // segfaulted here). txn ids are thd_id + n * g_thread_cnt.
            uint64_t hid = exr->inserter_id;
            txn_man * h = (hid != (uint64_t)-1) ? glob_manager->get_txn_man(hid % g_thread_cnt) : NULL;
            if (h && h->get_txn_id() == hid && !h->is_aborting())
                break;                       // live owner holds the key
        }
        uint64_t now = get_sys_clock();
        if (wait_start == 0) wait_start = now;
        else if (now - wait_start > 1000000)     // 1 ms backstop
            break;
        PAUSE
    }
    if (rc_insert == ERROR) {
        return false;   // the key belongs to somebody else; we have nothing to undo
    }

    assert(insert_idx_cnt < MAX_ROW_PER_TXN);

    // [fix] the key is in the index now, so record it before reporting the outcome. A key we
    // inserted but did not record would survive our abort and block that key forever.
    insert_idx_idx[insert_idx_cnt] = index;
    insert_idx_key[insert_idx_cnt] = key;
    insert_idx_row[insert_idx_cnt] = row;
    insert_idx_part_id[insert_idx_cnt] = part_id;
    insert_idx_cnt++;
    return rc_insert == RCOK;
}

bool txn_man::remove_idx(index_base* index, uint64_t key, row_t* row, int part_id) {
    (void)row;
    assert(remove_idx_cnt < MAX_ROW_PER_TXN);
    remove_idx_idx[remove_idx_cnt] = index;
    remove_idx_key[remove_idx_cnt] = key;
    remove_idx_part_id[remove_idx_cnt] = part_id;
    remove_idx_cnt++;
    return true;
}
//itemid_t * txn_man::index_read(INDEX * index, idx_key_t key, int part_id) {
//    uint64_t starttime = get_sys_clock();
//    itemid_t * item;
//    index->index_read(key, item, part_id, get_thd_id());
//    INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
//    return item;
//}
//
//void txn_man::index_read(INDEX * index, idx_key_t key, int part_id, itemid_t *& item) {
//    uint64_t starttime = get_sys_clock();
//    index->index_read(key, item, part_id, get_thd_id());
//    INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
//}

RC txn_man::finish(RC rc) {
#if TPCC_USER_ABORT
    RC ret_rc = rc;
    if (rc == ERROR)
        rc = Abort;
#endif

#if CC_ALG == RCC
    // Execution/user abort: mark self before any delay or cleanup. Row
    // cleanup propagates to successors. Preserve ERROR for a user abort.
    if (rc == Abort)
        set_abort(ABORT_SRC_SELF);
#endif

#if THINKTIME > 0
    usleep(THINKTIME);
#endif

#if CC_ALG == HSTORE
    return RCOK;
#endif

#if CC_ALG == WOUND_WAIT
    // (Bamboo and RCC check in commit_check(), after commit wait and right before their commit point)
    for (int i = 0; i < wr_cnt; i ++) {
        if (accesses[i]->orig_row->is_deleted){
            rc = Abort;
        }
    }
#endif

    uint64_t starttime = get_sys_clock();
#if CC_ALG == OCC
    if (rc == RCOK)
        rc = occ_man.validate(this);
    else
        cleanup(rc);
#elif CC_ALG == TICTOC
    if (rc == RCOK)
		rc = validate_tictoc();  // tictoc,silo,mocc all apply_index_changes in validate processing
	else {
		rc = apply_index_changes(rc);
		cleanup(rc);
	}
#elif CC_ALG == SILO
    if (rc == RCOK){
        rc = validate_silo();
    }
	else {
	    rc = apply_index_changes(rc);
	    cleanup(rc);
	}
#elif CC_ALG == MOCC
    if (rc == RCOK) {
		rc = validate_mocc();
	} else {
		rc = apply_index_changes(rc);
		cleanup(rc);
	}
#elif CC_ALG == IC3
    if (rc == RCOK) {
    rc = validate_ic3();
    if (rc == RCOK) {
      if (!ATOM_CAS(status, RUNNING, COMMITED))
        rc = Abort;
    } else {
      status = ABORTED;
    }
  } else { // abort an txn
    // involve cascading aborts
    status = ABORTED; // may overwritten the aborts set by others.
  }
  if (rc == Abort)
    abort_ic3();
  cleanup(rc);
#elif CC_ALG == HEKATON
    rc = validate_hekaton(rc);
	cleanup(rc);
#elif CC_ALG == WOUND_WAIT
    if (rc == RCOK) {
        if (!ATOM_CAS(status, RUNNING, COMMITED))
            rc = Abort;
	}
	rc = apply_index_changes(rc);
#if BB_PF_PW
	bool pw_c = (rc == RCOK);
	if (pw_c) { uint64_t t = get_sys_clock(); bb_pw_commit_begin(t, t, false); }   // no commit wait in Wound-Wait
#endif
	cleanup(rc);
#if BB_PF_PW
	if (pw_c) bb_pw_commit(get_txn_id());
#endif
#elif CC_ALG == BAMBOO
  if (rc == Abort)
      status = ABORTED;
  else {
#if BB_PF_CWPOS
    // [cw-pos] entries ahead of me in the retired lists I wait on (the most over my rows), taken before the commit wait
    // starts; the commit wait of a committed attempt is then added to that position's bucket (stats.h BB_CWPOS_METRICS).
    // Statistics only: walks other workers' entries without the row latch, as CW_POS_STAT does; capped at 4096 steps.
    uint64_t cw_pos = 0;
    for (int i = 0; i < row_cnt; i++) {
        BBLockEntry * e = accesses[i]->lock_entry;
        if (!e || e->status != LOCK_RETIRED || e->is_cohead) continue;
        uint64_t pos = 0; for (BBLockEntry * q = e->prev; q && pos < 4096; q = q->prev) pos++;
        if (pos > cw_pos) cw_pos = pos;
    }
#endif
    uint64_t starttime = get_sys_clock();
#if BB_PF_CWX
    bool cwx_waited = (commit_barriers >> 2) != 0;
#endif
#if BB_PF_PW
    bool pw_waited = (commit_barriers >> 2) != 0;
    bb_pw_cw_enter();
#endif
#if BB_PF_CWSPLIT
    bb_cw_start = starttime;
    uint64_t bb_waited = commit_barriers >> 2;   // predecessors left: this commit wait is released by one of them
#endif
    { uint64_t _b =
        commit_barriers >> 2;
        if (_b) { INC_STATS(get_thd_id(), cw_enter_cnt, 1); INC_STATS(get_thd_id(), cw_pred_sum, _b); }
#if BB_PF_AHEAD
        if (_b) { INC_TMP_STATS(get_thd_id(), bb_cw_ahead, _b); INC_TMP_STATS(get_thd_id(), bb_cw_cnt, 1); }
#endif
    }
#if CW_POS_STAT   // statistics only: walks other workers' lock entries right before commit wait (off by default)
    { // [cw-stat] physical entries ahead of me on each retired row (same metric as the RCC path)
        for (int i = 0; i < row_cnt; i++) {
            BBLockEntry * e = accesses[i]->lock_entry; if (!e || e->status != LOCK_RETIRED || e->is_cohead) continue;
            uint64_t pos = 0; for (BBLockEntry * q = e->prev; q; q = q->prev) pos++;
            INC_STATS(get_thd_id(), cwp_cw, 1); INC_STATS(get_thd_id(), cwp_nopred, pos);
        }
    }
#endif
    // every predecessor released this attempt (barrier 0): check, then commit
    while (!(commit_barriers == 0 && bb_commit_check() && ATOM_CAS(commit_barriers, 0, COMMITED))) {
        if (commit_barriers & ABORTED) {
            rc = Abort;
            break;
        }
        if (g_last_retire > 0 && (retire_threshold < row_cnt - 1)) {
                uint64_t lapse = get_sys_clock();
                if ((lapse - starttime) >= (lapse - start_ts) * g_last_retire) {
                    for (int rid = row_cnt - 1; rid > retire_threshold; rid--) {
                        if (accesses[rid]->lock_entry->type == LOCK_SH)
                            continue;
                        accesses[rid]->orig_row->retire_row(accesses[rid]->lock_entry);
                    }
                    retire_threshold = row_cnt - 1;
                }
        }
    }
#if PF_BASIC
    INC_TMP_STATS(get_thd_id(), time_commit, get_sys_clock() - starttime);   // [orig-acct]
#endif
#if BB_PF_CWX
    if (rc == RCOK) bb_cwx_begin_release(starttime, get_sys_clock(), cwx_waited);
#endif
#if BB_PF_PW
    if (rc == RCOK) bb_pw_commit_begin(starttime, get_sys_clock(), pw_waited);
#endif
#if BB_PF_CWPOS
    if (rc == RCOK) bb_cwpos_add(get_thd_id(), cw_pos, get_sys_clock() - starttime);
#endif
#if BB_PF_CWSPLIT
    if (rc == RCOK) {
        uint64_t end = get_sys_clock();
        bb_commit_ts = end;   // before cleanup releases my rows: my successors read it then
        if (bb_waited) {
            // the last release before my commit: what that predecessor was doing during my commit wait [starttime, end]
            uint64_t pcw = bb_dec_pcw, pc = bb_dec_pcommit, d = bb_dec_ts;
            auto clip = [&](uint64_t a, uint64_t b) -> uint64_t {   // length of [a, b] inside [starttime, end]
                a = a < starttime ? starttime : a; b = b > end ? end : b; return b > a ? b - a : 0; };
            uint64_t ex = 0, ca = 0, re = 0, no = 0;
            if (d >= starttime && pc && pcw && pcw <= pc && pc <= d) {
                ex = clip(starttime, pcw); ca = clip(pcw, pc); re = clip(pc, d); no = clip(d, end);
            }
            uint64_t total = end - starttime, sum = ex + ca + re + no;
            INC_TMP_STATS(get_thd_id(), time_cws_exec, ex); INC_TMP_STATS(get_thd_id(), time_cws_cascade, ca);
            INC_TMP_STATS(get_thd_id(), time_cws_release, re); INC_TMP_STATS(get_thd_id(), time_cws_notice, no);
            INC_TMP_STATS(get_thd_id(), time_cws_other, total > sum ? total - sum : 0);
            INC_TMP_STATS(get_thd_id(), bb_cws_cnt, 1);
            // flatten: my own link (ex, re, no) + the share of the predecessor's flattened wait that falls in mine
            // (ca of its pc - pcw, assuming its parts spread evenly over its wait)
            double f = (ca && pc > pcw) ? (double) ca / (double) (pc - pcw) : 0;
            uint64_t fex = ex + (uint64_t) (f * bb_dec_acc_ex), fre = re + (uint64_t) (f * bb_dec_acc_re);
            uint64_t fno = no + (uint64_t) (f * bb_dec_acc_no);
            uint64_t fsum = fex + fre + fno;
            bb_acc_ex = fex; bb_acc_re = fre; bb_acc_no = fno;
            // links inside my wait, x1000 (fixed point): me + the share of the predecessor's links that falls in my wait
            bb_acc_depth = 1000 + (uint64_t) (f * bb_dec_acc_depth);
            INC_TMP_STATS(get_thd_id(), time_cwf_exec, fex); INC_TMP_STATS(get_thd_id(), time_cwf_release, fre);
            INC_TMP_STATS(get_thd_id(), time_cwf_notice, fno);
            INC_TMP_STATS(get_thd_id(), time_cwf_other, total > fsum ? total - fsum : 0);
            INC_TMP_STATS(get_thd_id(), bb_cwf_depth, bb_acc_depth);
        }
    }
#endif
  }

  { uint64_t _rel0 = get_sys_clock(); bool _c = (rc == RCOK);
  rc = apply_index_changes(rc, true);   // validated in the commit loop
  cleanup(rc);
#if BB_PF_CWX
  if (_c) bb_cwx_commit(get_txn_id());
#endif
#if BB_PF_PW
  if (_c) bb_pw_commit(get_txn_id());
#endif
  if (_c) { INC_STATS(get_thd_id(), time_release_phase, get_sys_clock() - _rel0); INC_STATS(get_thd_id(), release_phase_cnt, 1); } }
#elif CC_ALG == RCC
    if (rc == Abort) status = ABORTED;
    else {
        uint64_t cw_start = get_sys_clock();
        wait_ctx = 2;   // enter commit-wait
        { uint64_t _b =
        commit_barriers >> 2;
        if (_b) { INC_STATS(get_thd_id(), cw_enter_cnt, 1); INC_STATS(get_thd_id(), cw_pred_sum, _b); } }   // [cw-stat] same as the Bamboo path
#if CW_POS_STAT   // statistics only: walks other workers' lock entries right before commit wait (off by default)
        { // [cw-stat] predecessor progress at commit-wait entry: for each of my entries with a live pred, record the pred's
          // accessed-row count (progress out of REQ_PER_QUERY) and whether it is still executing (wait_ctx 0) or already in commit wait (2)
            for (int i = 0; i < row_cnt; i++) {
                RCCLockEntry * e = accesses[i]->lock_entry; if (!e || e->pred == RCC_NO_PRED) continue;
                txn_man * pt = glob_manager->get_txn_man(e->pred % g_thread_cnt); if (!pt || pt->get_txn_id() != e->pred) continue;
                INC_STATS(get_thd_id(), cwp_cw, 1); INC_STATS(get_thd_id(), cwp_lockwait, (uint64_t)pt->row_cnt);
                int wc = pt->wait_ctx; if (wc == 0) { INC_STATS(get_thd_id(), cwp_exec, 1); } else if (wc == 2) { INC_STATS(get_thd_id(), cwp_done, 1); }
                { uint64_t pos = 0; for (RCCLockEntry * q = e->prev; q; q = q->prev) pos++; INC_STATS(get_thd_id(), cwp_nopred, pos); }   // [cw-stat] physical entries ahead of me on this row
            }
        }
#endif
#if RCC_DEBUG_STUCK
        bool stuck_warned = false;
#endif
#if RCC_TRIGGER_DETECT && RCC_ADD
#error "RCC_TRIGGER_DETECT cannot be combined with RCC_ADD"
#endif
#if RCC_TRIGGER_DETECT
        // [triggered detection + fast path] Try to commit first and walk only when actually
        // blocked. An unblocked commit (chain head) is on the critical path, so walking there
        // costs TPS directly. A blocked txn's walk overlaps its wait and is practically free.
        // Walk: targeted DFS along my pred chain (no publish/slot needed: it reads the other txn_man's
        // entries directly and also passes through running members). On a cycle, self-abort.
        // Edges are recorded under the row latch beforehand, so no fence is needed.
        bool rcc_go_spin = !(commit_barriers == 0 && rcc_commit_check() && ATOM_CAS(commit_barriers, 0, COMMITED));
        uint64_t last_walk = get_sys_clock();   // [safety net] time of the last walk, for re-walks
        if (rcc_go_spin) {
            uint64_t walk_start = last_walk;
            if (dl_detector.rcc_walk_from(get_txn_id(), get_thd_id(),
                                          g_thread_cnt)) {
                set_abort();
                INC_STATS(get_thd_id(), rcc_lock_deadlock_cnt, 1);
            }
            INC_GLOB_STATS(dl_detect_time, get_sys_clock() - walk_start);
        }
#else
        const bool rcc_go_spin = true;
#endif
        uint32_t spin_iter = 0;
#if RCC_ERW_PROBE
        extern uint64_t rp_txn[THREAD_CNT + 1][8];
        uint64_t rp_zero = 0;
#endif
#if CC_ALG == RCC && BB_PF_PW
        const uint64_t pw_rq0 = get_sys_clock();   // [BB_PF_PW] commit wait = [cw_start, commit]: approval request (ERW),
        uint64_t pw_z = 0; bool pw_waited = false; // waiting for the predecessors until the barrier is 0, approval wait
#endif
#if RCC_ERW_ON
#if RCC_ERW_PROBE
        const uint64_t rp_rq0 = get_sys_clock();
#endif
        // [ERW] ask the serialization detector for approval right before commit, after every edge was logged. An
        // attempt that logged no edge into itself has no predecessor: it cannot be on a cycle and approves itself.
        if (rccs_nedges == 0) { rccs_self_ok = true; rcc_srdet.note_fast(get_thd_id()); }
        else {
#if RCC_ERW_EDGE_SEND
            { extern void rccs_flush_edges(txn_man *); rccs_flush_edges(this); }   // every edge not sent yet, then the request
#endif
            rcc_srdet.log(get_thd_id(), RSE_REQUEST, get_txn_id());
        }
#if RCC_ERW_PROBE
        const uint64_t rp_rq = get_sys_clock() - rp_rq0;
#endif
#endif
#if CC_ALG == RCC && BB_PF_PW
        const uint64_t pw_rq1 = get_sys_clock();
#endif
        // [spin optimization] (1) test-then-CAS: lock RMW only when barriers is 0;
        // a CAS on every iteration while blocked fights the predecessor's DEC_BARRIER CAS for my cache
        // line and slows the chain reaction. (2) read the clock (rdtsc+fdiv) once every 64 iterations:
        // a 100ms timeout needs no ns precision, and reading it on every iteration spends 1/3 of
        // the spin CPU on the clock (measured with perf annotate).
        while (rcc_go_spin &&
#if   RCC_ERW_ON
               // [ERW] commit = approved AND every write predecessor (wr / ww) committed (commit barrier 0)
               !((rccs_self_ok || rcc_srdet.approved(get_thd_id(), get_txn_id())) &&
                 commit_barriers == 0 && rcc_commit_check() && ATOM_CAS(commit_barriers, 0, COMMITED))) {
#else
               !(commit_barriers == 0 && rcc_commit_check() && ATOM_CAS(commit_barriers, 0, COMMITED))) {
#endif
            if (commit_barriers & ABORTED) {
                // [measurement] notice latency: mark time -> time the spin notices it
                {
                    uint64_t mt = mark_ts, nw = get_sys_clock();
                    // small cross-core TSC skew (the marking core's clock is ahead) -> avoid a negative value
                    if (mt && nw > mt) {
                        INC_STATS(get_thd_id(), lat_abort_notice, nw - mt);
                        INC_STATS(get_thd_id(), lat_abort_notice_cnt, 1);
                    }
                }
                rc = Abort;
                break;
            }
#if CC_ALG == RCC && BB_PF_PW
            pw_waited = true;
            if (!pw_z && (commit_barriers >> 2) == 0) pw_z = get_sys_clock();   // every predecessor committed
#endif
#if RCC_ERW_PROBE && RCC_ERW_ON
            // [probe] the barrier is already 0 but the attempt is not approved yet: waiting for the detector only
            if (!rp_zero && commit_barriers == 0) rp_zero = get_sys_clock();
#endif
            bool check_clock = (((++spin_iter) & 0x3F) == 0);
#if RCC_PARALLEL_CLEANUP
            // No row latch is held here. Bound help to one row before checking
            // this transaction's outcome again; use this worker's MCS node.
            if (check_clock) rcc_cleanup_help(this);
#endif
#if !RCC_TRIGGER_DETECT && !RCC_ADD && RCC_ODH
            // [RCC pre-commit publish] The only publish point. Reaching here means the txn is actually
            // blocked, so every node in the graph is "waiting now".
            // The first call publishes everything and sets rcc_published; later spins return at once.
            rcc_publish_entries();
            // Rounds derive edges directly from the row fields, so nothing else is needed.
#elif RCC_ADD
            // [DT mode] Publishing is done only at the end of get_row (on formation). After entering
            // commit-wait the list does not change, so calling it here compares the slot on every iteration,
            // ping-ponging a line the detector thread reads (perf: 48% of the spin CPU).
            // Publish once on entry as a safety net and not during the spin.
            if (spin_iter == 1)
                rcc_publish_entries();
#endif
            // Backstop: keeps a cycle missed by the detector from spinning forever (0 = disabled).
            // Symmetric to RCC_LOCK_TIMEOUT for lock waits (row.cpp). Anything caught here
            // signals a hole in the detector.
            if (check_clock && g_rcc_commit_timeout &&
                get_sys_clock() - cw_start > g_rcc_commit_timeout) {
                INC_STATS(get_thd_id(), rcc_commit_timeout_cnt, 1);
#if RCC_TRIGGER_DETECT
                // [diagnostic] dump the shape of a cycle missed by triggered detection: my unresolved
                // preds, and each target's publish state / wait context.
                printf("[TRIGGER-MISS] thd=%lu txn=%lu preds:", get_thd_id(),
                       (uint64_t)get_txn_id());
                for (int rid = 0; rid < row_cnt; rid++) {
                    RCCLockEntry * e = accesses[rid]->lock_entry;
                    if (!e || e->is_cohead) continue;
                    uint64_t p = e->pred;
                    if (p == (uint64_t)-1) continue;
                    txn_man * bt = glob_manager->get_txn_man(p % g_thread_cnt);
                    bool alive = bt && bt->get_txn_id() == p;
                    printf(" [rid=%d pred=%lu alive=%d wc=%d st=%lu]",
                           rid, p, alive ? 1 : 0,
                           alive ? bt->wait_ctx : -1,
                           alive ? (bt->commit_barriers & 3UL) : 9);
                }
                printf("\n");
                fflush(stdout);
#endif
                set_abort();          // ABORTED bit -> leaves the loop on the next iteration
                rc = Abort;
                break;
            }
#if RCC_TRIGGER_DETECT
            // [triggered detection] two re-walk paths:
            //  (a) pred reassignment notification (Step 5)
            //  (b) [safety net] re-walk after waiting 1ms or more: the entry walk runs once, so if it
            //      overlaps a transient transition of another entry (owner->retired, cohead update, etc.)
            //      it can miss a real cycle. A cycle does not disappear, so
            //      an infrequent re-walk always catches it (replaces the 100ms backstop for correctness).
            {
                bool due = check_clock &&
                    (get_sys_clock() - cw_start > 1000000) &&    // 1ms
                    (get_sys_clock() - last_walk > 1000000);
                if (rcc_recheck || due) {
                    rcc_recheck = false;
                    COMPILER_BARRIER
                    uint64_t now_w = get_sys_clock();
                    last_walk = now_w;
                    if (dl_detector.rcc_walk_from(get_txn_id(), get_thd_id(),
                                                  g_thread_cnt)) {
                        set_abort();
                        INC_STATS(get_thd_id(), rcc_lock_deadlock_cnt, 1);
                    }
                    INC_GLOB_STATS(dl_detect_time, get_sys_clock() - now_w);  // [measurement]
                }
            }
#elif !RCC_ADD && RCC_ODH
            // the waiter runs the detection round itself (baseline mode, for A/B)
            dl_detector.rcc_try_round(get_thd_id());
#endif
#if RCC_DEBUG_STUCK
            if (check_clock && !stuck_warned && get_sys_clock() - cw_start > g_rcc_commit_stuck_warn) {
                stuck_warned = true;
                uint64_t bar = commit_barriers;
                printf("[RCC-STUCK] thd=%lu txn=%lu waited=%.3fs "
                       "barriers=%lu status=%lu row_cnt=%d\n",
                       get_thd_id(), (uint64_t)get_txn_id(),
                       (double)(get_sys_clock() - cw_start) / 1000000000.0,
                       bar >> 2, bar & 3UL, row_cnt);
#if RCC_ERW_ON
                // [diagnostic] approval state, then the queue of every row where this attempt still waits (non-cohead)
                static volatile int dumps = 0;
                if (__atomic_fetch_add(&dumps, 1, __ATOMIC_RELAXED) < 12) {
                    uint64_t ds[5]; rcc_srdet.dbg_state(get_thd_id(), ds);
                    printf("  approval: self_ok=%d approved=%d nedges=%u | log head=%lu tail=%lu tail_seen=%lu slot_approved=%ld floor=%lu rec_att=%lu rec_n=%u\n",
                           (int) rccs_self_ok, (int) rcc_srdet.approved(get_thd_id(), get_txn_id()), rccs_nedges,
                           ds[0], ds[1], ds[2], (long) ds[3], ds[4], (uint64_t) rs_rec_att, (uint32_t) rs_rec_n);
                    for (int rid = 0; rid < row_cnt; rid++) {
                        RCCLockEntry * e = accesses[rid]->lock_entry;
                        if (!accesses[rid]->orig_row || e->is_cohead) continue;
                        printf("  access %d: %s pred=%ld status=%d\n", rid, e->type == LOCK_EX ? "W" : "R",
                               e->pred == RCC_NO_PRED ? -1L : (long) e->pred, (int) e->status);
                        accesses[rid]->orig_row->manager->stuck_dump(this);
                    }
                }
#endif
                fflush(stdout);
            }
#endif
            PAUSE
        }
        wait_ctx = 0;                          // leave commit-wait
#if CC_ALG == RCC && BB_PF_PW
        if (rc == RCOK) {   // logged in time order
            const uint64_t pw_c = get_sys_clock();
#if RCC_ERW_ON
            if (pw_rq1 > pw_rq0) bb_pw_own(pw_rq0, pw_rq1, BB_PW_DCS);          // asking the detector for approval
#endif
            bb_pw_commit_begin(cw_start, pw_c, pw_waited, pw_rq1, pw_z ? pw_z : pw_c);
#if RCC_ERW_ON
            if (pw_z && pw_c > pw_z) bb_pw_own(pw_z, pw_c, BB_PW_DCS);           // barrier 0, waiting for the approval
#endif
        }
#endif
#if RCC_ERW_PROBE
        if (rc == RCOK) {
            rp_txn[get_thd_id()][3]++;
#if RCC_ERW_ON
            rp_txn[get_thd_id()][5] += rp_rq;
            if (!rccs_self_ok) rp_txn[get_thd_id()][2]++;
#endif
            if (rp_zero) { rp_txn[get_thd_id()][0] += get_sys_clock() - rp_zero; rp_txn[get_thd_id()][1]++; }
        }
#endif
        // [measurement] commit propagation latency: last DEC (commit of the last pred) -> my CAS success.
        // Only when the txn was blocked and then released (last_dec_ts after cw_start).
        if (rc == RCOK && last_dec_ts > cw_start) {
            INC_STATS(get_thd_id(), lat_commit_prop, get_sys_clock() - last_dec_ts);
            INC_STATS(get_thd_id(), lat_commit_prop_cnt, 1);
        }
        // [RCC] the detector slot is reclaimed only by rcc_clear in cleanup()
#if PF_BASIC
        INC_TMP_STATS(get_thd_id(), time_commit, get_sys_clock() - cw_start);   // [orig-acct]
#endif
    }
  {
    // [measurement] cost of commit "processing" itself (returning rows + decrementing successors' barriers).
    // Kept separate from the wait (time_commit) to tell whether chain gaps come from processing delay.
    uint64_t cp_start = get_sys_clock();
    cleanup(rc);
#if BB_PF_PW
    if (rc == RCOK) bb_pw_commit(get_txn_id());   // release over: keep the attempt's log
#endif
    if (rc == RCOK)
        INC_STATS(get_thd_id(), time_commit_proc, get_sys_clock() - cp_start);
  }
#else
  rc = apply_index_changes(rc); // if abort, remove row from the index, flag the row as deleted
  cleanup(rc);
#endif

    uint64_t timespan = get_sys_clock() - starttime;
    INC_TMP_STATS(get_thd_id(), time_man,  timespan);
    INC_STATS(get_thd_id(), time_cleanup,  timespan);

#if TPCC_USER_ABORT
    if (rc == Abort && (ret_rc == ERROR)) {
        return ret_rc;
    }
#endif
    return rc;
}

void txn_man::release() {
    for (int i = 0; i < num_accesses_alloc; i++) {
    #if CC_ALG == BAMOO || CC_ALG == NO_WAIT || CC_ALG == WOUND_WAIT || CC_ALG == WAIT_DIE || CC_ALG == DL_DETEC
        delete accesses[i]->lock_entry;
    #endif
        mem_allocator.free(accesses[i], 0);
    }
    mem_allocator.free(accesses, 0);
#if LATCH == LH_MCSLOCK
    delete mcs_node;
#endif
}

// Validation point of the early-release protocols (Bamboo, RCC / RCC_BATCH): after every predecessor finished and
// before the commit CAS. A predecessor applies its index changes (removed keys, deleted rows) before it releases its
// dependents, so this sees them, and the attempt is not committed yet, so a failure is an ordinary abort. Checks that
// no written row was deleted and that no index node read has changed (phantom).
bool txn_man::commit_check() {
    for (int i = 0; i < row_cnt; ++i) {
        if (accesses[i]->type != WR) continue;
        row_t * orig = accesses[i]->orig_row;
        if (orig && orig->is_deleted) return false;
    }
#if WORKLOAD == TPCC
    if (validate() != RCOK) return false;
#endif
    return true;
}

RC txn_man::validate() {
    for (auto it : node_map) {
        if (IndexMBTree::extract_version(it.first) != it.second) {
            return Abort;
        }
    }
    return RCOK;
}

#if COMMUTATIVE_OPS
void txn_man::inc_value(int col, uint64_t val) {
  // store operation and execute at commit time
  Access * access = accesses[row_cnt-1];
  access->com_op = COM_INC;
  access->com_val = val;
  access->com_col = col;
}

void txn_man::dec_value(int col, uint64_t val) {
  // store operation and execute at commit time
  Access * access = accesses[row_cnt-1];
  access->com_op = COM_DEC;
  access->com_val = val;
  access->com_col = col;
}
#endif


#if CC_ALG == MOCC
bool
txn_man::is_locked(uint64_t key) {
    for (int i = 0; i < cur_lock_list_head; ++i) {
        if (cur_lock_list[i].row->get_primary_key() == key && cur_lock_list[i].state)
            return true;
    }
    return false;
}

void
txn_man::remove_non_cononical_lock(uint64_t key) {
    for (int i = (cur_lock_list_head - 1); i >= 0; --i) {
        if (!cur_lock_list[i].state)
            continue;

        if (cur_lock_list[i].row->get_primary_key() < key) {
            cur_lock_list[i].row->manager->unlock(this, cur_lock_list[i].lt);
        } else {
            cur_lock_list_head = i + 1;
            return;
        }
    }

    cur_lock_list_head = 0;
}

void
txn_man::insert_cononical_lock(int lt, row_t *row) {
    cur_lock_list[cur_lock_list_head].row = row;
    cur_lock_list[cur_lock_list_head].lt = lt;
    cur_lock_list[cur_lock_list_head].state = true;
    cur_lock_list_head += 1;
}

void
txn_man::remove_cononical_lock(uint64_t key) {
    for (int i = 0; i < cur_lock_list_head; ++i) {
        if (cur_lock_list[i].row->get_primary_key() == key)
            cur_lock_list[i].state = false;
    }
}

void
txn_man::unlock_read_locks_all() {
    for (int i = 0; i < cur_lock_list_head; ++i) {
        if (cur_lock_list[i].lt == LOCK_SH && cur_lock_list[i].state)
            cur_lock_list[i].row->manager->unlock(this, LOCK_SH);
    }
}

void
txn_man::clear_lock_state(RC rc) {
    for (int i = 0; i < cur_lock_list_head; ++i) {
        if (cur_lock_list[i].lt == LOCK_SH && cur_lock_list[i].state)
            cur_lock_list[i].row->manager->unlock(this, LOCK_SH);
        if (cur_lock_list[i].lt == LOCK_EX && cur_lock_list[i].state) {
            cur_lock_list[i].row->manager->unlock(this, LOCK_EX);
        }
    }
    cur_lock_list_head = 0;
}

#endif
