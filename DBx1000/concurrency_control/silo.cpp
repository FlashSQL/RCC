#include "txn.h"
#include "row.h"
#include "row_silo.h"
#include "index_btree.h"
#include "index_hash.h"
#include "index_mbtree.h"   // index_base for the deferred inserts

#if CC_ALG == SILO

RC
txn_man::validate_silo()
{
#if PF_CS
    uint64_t starttime = get_sys_clock();
    uint64_t wait_span = 0;
#endif

    RC rc = RCOK;
    // lock write tuples in the primary key order.
    int write_set[wr_cnt];
    int cur_wr_idx = 0;
    int read_set[row_cnt - wr_cnt];
    int cur_rd_idx = 0;
    for (int rid = 0; rid < row_cnt; rid ++) {
        if (accesses[rid]->type == WR)
            write_set[cur_wr_idx ++] = rid;
        else
            read_set[cur_rd_idx ++] = rid;
    }

    // bubble sort the write set, in primary key order
    for (int i = wr_cnt - 1; i >= 1; i--) {
        for (int j = 0; j < i; j++) {
            if (accesses[ write_set[j] ]->orig_row->get_primary_key() >
                accesses[ write_set[j + 1] ]->orig_row->get_primary_key())
            {
                int tmp = write_set[j];
                write_set[j] = write_set[j+1];
                write_set[j+1] = tmp;
            }
        }
    }

    int num_locks = 0;
    ts_t max_tid = 0;
    bool done = false;
    if (_pre_abort) {
        for (int i = 0; i < wr_cnt; i++) {
            row_t * row = accesses[ write_set[i] ]->orig_row;
            if (row->manager->get_tid() != accesses[write_set[i]]->tid) {
                rc = Abort;
                goto final;
            }
        }
        for (int i = 0; i < row_cnt - wr_cnt; i ++) {
            Access * access = accesses[ read_set[i] ];
            if (access->orig_row->is_deleted){
                rc = Abort;
                goto final;
            }else{
                if (access->orig_row->manager->get_tid() != accesses[read_set[i]]->tid) {
                    rc = Abort;
                    goto final;
                }
            }
        }
    }

    // lock all rows in the write set.
    if (_validation_no_wait) {
        while (!done) {
            num_locks = 0;
            for (int i = 0; i < wr_cnt; i++) {
                row_t * row = accesses[ write_set[i] ]->orig_row;
                if (!row->manager->try_lock()){
                    break;
                }
                row->manager->assert_lock();
                num_locks ++;
                if (row->manager->get_tid() != accesses[write_set[i]]->tid)
                {
                    rc = Abort;
                    goto final;
                }
            }

            if (num_locks == wr_cnt){
                done = true;
            } else {
                uint64_t wait_time = get_sys_clock();
                for (int i = 0; i < num_locks; i++){
                    accesses[ write_set[i] ]->orig_row->manager->release();
                }
                if (_pre_abort) {
                    num_locks = 0;
                    for (int i = 0; i < wr_cnt; i++) {
                        row_t * row = accesses[ write_set[i] ]->orig_row;
                        if (row->manager->get_tid() != accesses[write_set[i]]->tid) {
                            rc = Abort;
                            goto final;
                        }
                    }
                    for (int i = 0; i < row_cnt - wr_cnt; i ++) {
                        Access * access = accesses[ read_set[i] ];
                        if (access->orig_row->manager->get_tid() != accesses[read_set[i]]->tid) {
                            rc = Abort;
                            goto final;
                        }
                    }
                }
                PAUSE

#if PF_CS
                uint64_t timespan = get_sys_clock() - wait_time;
                wait_span = wait_span + timespan;
#endif
            }
        }
    } else {
        for (int i = 0; i < wr_cnt; i++) {
            row_t * row = accesses[ write_set[i] ]->orig_row;
            row->manager->lock();
            num_locks++;
            if (row->manager->get_tid() != accesses[write_set[i]]->tid) {
                rc = Abort;
                goto final;
            }
        }
    }

    // validate rows in the read set
    // for repeatable_read, no need to validate the read set.
    for (int i = 0; i < row_cnt - wr_cnt; i ++) {
        Access * access = accesses[ read_set[i] ];
        bool success = access->orig_row->manager->validate(access->tid, false);
        if (!success) {
            rc = Abort;
            goto final;
        }
        if (access->tid > max_tid)
            max_tid = access->tid;
    }
    // validate rows in the write set
    for (int i = 0; i < wr_cnt; i++) {
        Access * access = accesses[ write_set[i] ];
        bool success = access->orig_row->manager->validate(access->tid, true);
        if (!success) {
            rc = Abort;
            goto final;
        }
        if (access->tid > max_tid)
            max_tid = access->tid;
    }
    if (max_tid > _cur_tid)
        _cur_tid = max_tid + 1;
    else
        _cur_tid ++;
    // validated, write set locked: now the deferred index inserts (txn.h silo_pend_*)
    rc = silo_apply_inserts();

    final:

    rc = apply_index_changes(rc);

    if (rc == Abort) {
        for (int i = 0; i < num_locks; i++)
            accesses[ write_set[i] ]->orig_row->manager->release();
//		cleanup(rc);
    } else {
//        for (UInt32 i = 0; i < insert_cnt; i++) {
//			row_t * row = insert_rows[i];
//      		row->manager->set_tid(_cur_tid);  // unlocking is done as well
//		}

        for (int i = 0; i < wr_cnt; i++) {
            Access * access = accesses[ write_set[i] ];
            access->orig_row->manager->write(access->data, _cur_tid );
            accesses[ write_set[i] ]->orig_row->manager->release();
        }
//		cleanup(rc);
    }

#if PF_CS
    uint64_t endtime = get_sys_clock();
    uint64_t timespan = endtime - starttime;
    uint64_t latchtime = timespan - wait_span;
    // per attempt (merged at commit, dropped at abort): an aborted attempt's whole time is already counted in
    // time_abort, so counting its validation here too made Wait + Wasted exceed the run time
    INC_TMP_STATS(this->get_thd_id(), time_get_latch, latchtime);
    INC_TMP_STATS(this->get_thd_id(), time_commit , wait_span);
    this->wait_latch_time = this->wait_latch_time + timespan;
#endif

    cleanup(rc);

    return rc;
}

// [SILO] the index inserts deferred during the execution. A duplicate key here would be a real conflict (the key's
// writer committed past our validation): abort; the keys inserted so far are recorded in insert_idx_* and removed by
// cleanup() like any aborted insert.
RC txn_man::silo_apply_inserts() {
    for (uint64_t i = 0; i < silo_pend_cnt; i++) {
        row_t * row = silo_pend_row[i];
        row->inserter = this;
        row->inserter_id = get_txn_id();
        if (silo_pend_idx[i]->index_insert(silo_pend_key[i], silo_pend_item[i], silo_pend_part[i]) == ERROR) {
            silo_pend_cnt = 0;
            return Abort;
        }
        assert(insert_idx_cnt < MAX_ROW_PER_TXN);
        insert_idx_idx[insert_idx_cnt] = silo_pend_idx[i];
        insert_idx_key[insert_idx_cnt] = silo_pend_key[i];
        insert_idx_row[insert_idx_cnt] = row;
        insert_idx_part_id[insert_idx_cnt] = silo_pend_part[i];
        insert_idx_cnt++;
    }
    silo_pend_cnt = 0;
    return RCOK;
}
#endif
