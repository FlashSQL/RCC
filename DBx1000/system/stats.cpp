#include <iostream>
#include <fstream>
#include <iomanip>
#include "global.h"
#include "helper.h"
#include "stats.h"
#include "mem_alloc.h"

#define BILLION 1000000000UL

/**
 * [Stats_thd] ================================================================================
 */

/**
*
* @param thd_id : UNUSED
*/
void Stats_thd::init(uint64_t thd_id) {
    clear();
    all_debug1 = (uint64_t *)_mm_malloc(sizeof(uint64_t) * g_txn_max_part, 64);
    all_debug2 = (uint64_t *)_mm_malloc(sizeof(uint64_t) * g_txn_max_part, 64);
}

// Initialize all metrics declared in Stats_thd class.
void Stats_thd::clear() {
    ALL_METRICS(INIT_VAR, INIT_VAR, INIT_VAR)
}



/**
 * [Stats_tmp] ================================================================================
 */
void Stats_tmp::init() {
    clear();
}

// Initialize 3 metrics declared in Stats_tmp class.
void Stats_tmp::clear() {
    TMP_METRICS(INIT_VAR, INIT_VAR)
}


/**
 * [Stats] ====================================================================================
 */

/**
 * 1. Malloc space for _stats and tmp_stats of all threads.
 * 2. Initialize GLOBAL metrics.
 */
void Stats::init() {
    if (!STATS_ENABLE)              // STATS_ENABLE == true in config.h
        return;

    // Malloc space for PER_THREAD metrics.
    _stats = (Stats_thd**)_mm_malloc(sizeof(Stats_thd*) * g_thread_cnt, 64);
    tmp_stats = (Stats_tmp**)_mm_malloc(sizeof(Stats_tmp*) * g_thread_cnt, 64);

    // Initialize GLOBAL metrics.
    dl_detect_time = 0;
    dl_wait_time = 0;
    deadlock = 0;
    cycle_detect = 0;
    rcc_false_positive = rcc_fp_committed = rcc_cycle_len_sum = rcc_cycle_len_max = rcc_dl_lockwait = rcc_dl_commitwait = rcc_dl_running = 0;
    rcc_snap_drop = rcc_snap_time = rcc_dfs_time = rcc_round_time = rcc_rounds = rcc_confirmed = rcc_persist_filtered = rcc_round_cache_sound = rcc_round_cache_exact = 0; rcc_derive_time = 0;

    created_long_txn_cnt = (int*)_mm_malloc(sizeof(int) * g_thread_cnt, 64);
    for(int i = 0; i < g_thread_cnt;i++){
        created_long_txn_cnt[i] = 0;
    }

}

/**
 * 1. Malloc space for _stats and tmp_stats object of a specific thread.
 * 2. Initialize all metrics of the specific thread.
 * @param thread_id : id of the specific thread
 */
void Stats::init(uint64_t thread_id) {
    if (!STATS_ENABLE)
        return;

    _stats[thread_id] = (Stats_thd *)_mm_malloc(sizeof(Stats_thd), 64);
    tmp_stats[thread_id] = (Stats_tmp *)_mm_malloc(sizeof(Stats_tmp), 64);

    _stats[thread_id]->init(thread_id);
    tmp_stats[thread_id]->init();
}

/**
 * Clear PER_THREAD and GLOBAL metrics of a specific thread.
 * @param tid : id of the specific thread
 */
void Stats::clear(uint64_t tid) {
    if (STATS_ENABLE) {
        _stats[tid]->clear();
        tmp_stats[tid]->clear();

        dl_detect_time = 0;
        dl_wait_time = 0;
        cycle_detect = 0;
        deadlock = 0;
        rcc_false_positive = rcc_fp_committed = rcc_cycle_len_sum = rcc_cycle_len_max = rcc_dl_lockwait = rcc_dl_commitwait = rcc_dl_running = 0;
        rcc_snap_drop = rcc_snap_time = rcc_dfs_time = rcc_round_time = rcc_rounds = rcc_confirmed = rcc_persist_filtered = rcc_round_cache_sound = rcc_round_cache_exact = 0; rcc_derive_time = 0;
    }
}

/**
 * USELESS(only TICTOC calls it)
 * g_prt_lat_distr: print the transaction latency distribution or not.
 * Since g_prt_lat_distr == false in config.h, this function does nothing.
 */
void Stats::add_debug(uint64_t thd_id, uint64_t value, uint32_t select) {
    if (g_prt_lat_distr && warmup_finish) {           // g_prt_lat_distr == false
        uint64_t tnum = _stats[thd_id]->txn_cnt;
        if (select == 1)
            _stats[thd_id]->all_debug1[tnum] = value;
        else if (select == 2)
            _stats[thd_id]->all_debug2[tnum] = value;
    }
}

/**
 * The transaction in thread(thd_id) commits successfully.
 * Record 3 processing metrics in tmp_stats[thd_id] of this transaction to _stats[thd_id].
 * @param thd_id : thread_ID of a thread which just commits a transaction
 */
void Stats::commit(uint64_t thd_id) {
    if (STATS_ENABLE) {
        _stats[thd_id]->time_man += tmp_stats[thd_id]->time_man;
        _stats[thd_id]->time_index += tmp_stats[thd_id]->time_index;
        _stats[thd_id]->time_wait += tmp_stats[thd_id]->time_wait;
        _stats[thd_id]->time_copy += tmp_stats[thd_id]->time_copy;
        _stats[thd_id]->time_commit += tmp_stats[thd_id]->time_commit;   // [orig-acct]
        _stats[thd_id]->time_get_latch += tmp_stats[thd_id]->time_get_latch; _stats[thd_id]->time_get_cs += tmp_stats[thd_id]->time_get_cs;
        _stats[thd_id]->time_retire_latch += tmp_stats[thd_id]->time_retire_latch; _stats[thd_id]->time_retire_cs += tmp_stats[thd_id]->time_retire_cs;
        _stats[thd_id]->time_bb_ts += tmp_stats[thd_id]->time_bb_ts; _stats[thd_id]->time_bb_wound += tmp_stats[thd_id]->time_bb_wound;   // BB_PF_PREVENT
        _stats[thd_id]->time_bb_scan += tmp_stats[thd_id]->time_bb_scan; _stats[thd_id]->time_bb_copy += tmp_stats[thd_id]->time_bb_copy;
        _stats[thd_id]->bb_ts_cnt += tmp_stats[thd_id]->bb_ts_cnt; _stats[thd_id]->bb_wound_cnt += tmp_stats[thd_id]->bb_wound_cnt;
        _stats[thd_id]->bb_get_cnt += tmp_stats[thd_id]->bb_get_cnt;
        _stats[thd_id]->bb_lw_ahead += tmp_stats[thd_id]->bb_lw_ahead; _stats[thd_id]->bb_lw_cnt += tmp_stats[thd_id]->bb_lw_cnt;   // BB_PF_AHEAD
        _stats[thd_id]->bb_cw_ahead += tmp_stats[thd_id]->bb_cw_ahead; _stats[thd_id]->bb_cw_cnt += tmp_stats[thd_id]->bb_cw_cnt;
        _stats[thd_id]->time_cws_exec += tmp_stats[thd_id]->time_cws_exec; _stats[thd_id]->time_cws_cascade += tmp_stats[thd_id]->time_cws_cascade;   // BB_PF_CWSPLIT
        _stats[thd_id]->time_cws_release += tmp_stats[thd_id]->time_cws_release; _stats[thd_id]->time_cws_notice += tmp_stats[thd_id]->time_cws_notice;
        _stats[thd_id]->time_cws_other += tmp_stats[thd_id]->time_cws_other; _stats[thd_id]->bb_cws_cnt += tmp_stats[thd_id]->bb_cws_cnt;
        _stats[thd_id]->time_cwf_exec += tmp_stats[thd_id]->time_cwf_exec; _stats[thd_id]->time_cwf_release += tmp_stats[thd_id]->time_cwf_release;
        _stats[thd_id]->time_cwf_notice += tmp_stats[thd_id]->time_cwf_notice; _stats[thd_id]->time_cwf_other += tmp_stats[thd_id]->time_cwf_other;
        _stats[thd_id]->bb_cwf_depth += tmp_stats[thd_id]->bb_cwf_depth;
        tmp_stats[thd_id]->init();
    }
}

/**
 * The transaction in thread(thd_id) aborts.
 * Clear 3 processing metrics in tmp_stats[thd_id] of this transaction.
 * @param thd_id
 */
void Stats::abort(uint64_t thd_id) {
    if (STATS_ENABLE)
        tmp_stats[thd_id]->init();
}

/**
 * Calculate processing results_background of this workload.
 */
void Stats::print() {
    // Declare and initialize total_XX metrics for every metric.
    ALL_METRICS(INIT_TOTAL_VAR, INIT_TOTAL_VAR, INIT_TOTAL_VAR)

    // Calculate the sum of x&y metrics and the max of z metric.
//    for (uint64_t tid = 0; tid < g_thread_cnt; tid ++) {
//        ALL_METRICS(SUM_UP_STATS, SUM_UP_STATS, MAX_STATS)
//        // print the proccessing result of each thread to terminal
//        printf("[tid=%lu] txn_cnt=%lu,abort_cnt=%lu, user_abort_cnt=%lu, txn_cnt_long=%lu,abort_cnt_long=%lu, created_long_txn_cnt=%d\n",
//            tid, _stats[tid]->txn_cnt, _stats[tid]->abort_cnt,
//            _stats[tid]->user_abort_cnt, _stats[tid]->txn_cnt_long, _stats[tid]->abort_cnt_long,  created_long_txn_cnt[tid]);
//    }

    // Calculate the average value.
    // Todo: Notice that the value of these three metrics(latency,commit_latency,time_man) isn't divided by BILLION,
    //  because it is already very small. Once we divide it, they will all equal to 0.
//    total_latency = total_latency / total_txn_cnt;
//    total_commit_latency = total_commit_latency / total_txn_cnt;
//    total_time_man = total_time_man - total_time_wait;

    if (output_file != NULL) {
        ofstream outf(output_file,ios::app);

        if (outf.is_open()) {
            for (uint64_t tid = 0; tid < g_thread_cnt; tid ++) {
                ALL_METRICS(SUM_UP_STATS, SUM_UP_STATS, MAX_STATS)
                // print the proccessing result of each thread to terminal
                outf << "[tid=" << tid << "] ";
                outf << "txn_cnt=" << _stats[tid]->txn_cnt << ", ";
                outf << "abort_cnt=" << _stats[tid]->abort_cnt << ", ";
                outf << "user_abort_cnt=" << _stats[tid]->user_abort_cnt << ", ";
                outf << "txn_cnt_long=" << _stats[tid]->txn_cnt_long << ", ";
                outf << "abort_cnt_long=" << _stats[tid]->abort_cnt_long << ", ";
                outf << "created_long_txn_cnt=" << created_long_txn_cnt[tid] << ", ";
                outf << "abort_hotspot=" << _stats[tid]->abort_hotspot << ", ";
                outf << "abort_position=" << _stats[tid]->abort_position << ", ";
                outf << "abort_position_cnt=" << _stats[tid]->abort_position_cnt << ", " "\n";
            }

            total_latency = total_latency / total_txn_cnt;
            total_commit_latency = total_commit_latency / total_txn_cnt;
            total_time_man = total_time_man - total_time_wait;

            outf << "[summary] throughput=" << total_txn_cnt / total_run_time * BILLION * THREAD_CNT << ", ";
            outf << "abort_rate=" << (double)total_abort_cnt / (double)(total_abort_cnt + total_txn_cnt) << ", ";
            ALL_METRICS(WRITE_STAT_X, WRITE_STAT_Y, WRITE_STAT_Y)
            outf << "deadlock_cnt=" << deadlock << ", ";
            outf << "cycle_detect=" << cycle_detect << ", ";
            outf << "dl_detect_time=" << dl_detect_time / BILLION << ", ";
            outf << "dl_wait_time=" << dl_wait_time / BILLION << "\n\n";
            outf.precision(10);
            outf.close();
        }
    }else {

        for (uint64_t tid = 0; tid < g_thread_cnt; tid ++) {
            ALL_METRICS(SUM_UP_STATS, SUM_UP_STATS, MAX_STATS)
            // print the proccessing result of each thread to terminal
            printf("[tid=%lu] txn_cnt=%lu,abort_cnt=%lu, user_abort_cnt=%lu, txn_cnt_long=%lu,abort_cnt_long=%lu, created_long_txn_cnt=%d,abort_hotspot=%lu,abort_position=%lu,abort_position_cnt=%lu\n",
                   tid, _stats[tid]->txn_cnt, _stats[tid]->abort_cnt,
                   _stats[tid]->user_abort_cnt, _stats[tid]->txn_cnt_long, _stats[tid]->abort_cnt_long,  created_long_txn_cnt[tid], _stats[tid]->abort_hotspot, _stats[tid]->abort_position, _stats[tid]->abort_position_cnt);
        }

        total_latency = total_latency / total_txn_cnt;
        total_commit_latency = total_commit_latency / total_txn_cnt;
        total_time_man = total_time_man - total_time_wait;

        std::cout << "[summary] throughput=" << total_txn_cnt / total_run_time * BILLION * THREAD_CNT << ", ";
        std::cout << "abort_rate=" << (double)total_abort_cnt / (double)(total_abort_cnt + total_txn_cnt) << ", ";
        ALL_METRICS(PRINT_STAT_X, PRINT_STAT_Y, PRINT_STAT_Y)
        std::cout << "deadlock_cnt=" << deadlock << ", ";
        std::cout << "rcc_confirmed=" << rcc_confirmed << ", rcc_false_positive=" << rcc_false_positive
                  << ", rcc_persist_filtered=" << rcc_persist_filtered << ", rcc_dl_lockwait=" << rcc_dl_lockwait
                  << ", rcc_dl_commitwait=" << rcc_dl_commitwait << ", rcc_dl_running=" << rcc_dl_running
                  << ", rcc_cycle_len_max=" << rcc_cycle_len_max << ", rcc_derive_time=" << rcc_derive_time / BILLION
                  << ", rcc_round_cache_sound=" << rcc_round_cache_sound
                  << ", rcc_round_cache_exact=" << rcc_round_cache_exact << ", ";
        std::cout << "cycle_detect=" << cycle_detect << ", ";
        std::cout << "dl_detect_time=" << dl_detect_time / BILLION << ", ";
        std::cout << "dl_wait_time=" << dl_wait_time / BILLION << "\n";

    }
    if (g_prt_lat_distr)      // g_prt_lat_distr == false
        print_lat_distr();
}



/**
 * Print the transaction latency distribution.
 */
void Stats::print_lat_distr() {
    FILE * outf;
    if (output_file != NULL) {
        outf = fopen(output_file, "a");
        for (UInt32 tid = 0; tid < g_thread_cnt; tid ++) {
            fprintf(outf, "[all_debug1 thd=%d] ", tid);
            for (uint32_t tnum = 0; tnum < _stats[tid]->txn_cnt; tnum ++)
                fprintf(outf, "%ld,", _stats[tid]->all_debug1[tnum]);
            fprintf(outf, "\n[all_debug2 thd=%d] ", tid);
            for (uint32_t tnum = 0; tnum < _stats[tid]->txn_cnt; tnum ++)
                fprintf(outf, "%ld,", _stats[tid]->all_debug2[tnum]);
            fprintf(outf, "\n");
        }
        fclose(outf);
    }
}

#if BB_PF_CWPOS
void bb_cwpos_add(uint64_t tid, uint64_t pos, uint64_t time) {
    switch (pos < 32 ? pos : 32) {
    case 0: INC_STATS(tid, bb_cwpos_t0, time); INC_STATS(tid, bb_cwpos_n0, 1); break;
    case 1: INC_STATS(tid, bb_cwpos_t1, time); INC_STATS(tid, bb_cwpos_n1, 1); break;
    case 2: INC_STATS(tid, bb_cwpos_t2, time); INC_STATS(tid, bb_cwpos_n2, 1); break;
    case 3: INC_STATS(tid, bb_cwpos_t3, time); INC_STATS(tid, bb_cwpos_n3, 1); break;
    case 4: INC_STATS(tid, bb_cwpos_t4, time); INC_STATS(tid, bb_cwpos_n4, 1); break;
    case 5: INC_STATS(tid, bb_cwpos_t5, time); INC_STATS(tid, bb_cwpos_n5, 1); break;
    case 6: INC_STATS(tid, bb_cwpos_t6, time); INC_STATS(tid, bb_cwpos_n6, 1); break;
    case 7: INC_STATS(tid, bb_cwpos_t7, time); INC_STATS(tid, bb_cwpos_n7, 1); break;
    case 8: INC_STATS(tid, bb_cwpos_t8, time); INC_STATS(tid, bb_cwpos_n8, 1); break;
    case 9: INC_STATS(tid, bb_cwpos_t9, time); INC_STATS(tid, bb_cwpos_n9, 1); break;
    case 10: INC_STATS(tid, bb_cwpos_t10, time); INC_STATS(tid, bb_cwpos_n10, 1); break;
    case 11: INC_STATS(tid, bb_cwpos_t11, time); INC_STATS(tid, bb_cwpos_n11, 1); break;
    case 12: INC_STATS(tid, bb_cwpos_t12, time); INC_STATS(tid, bb_cwpos_n12, 1); break;
    case 13: INC_STATS(tid, bb_cwpos_t13, time); INC_STATS(tid, bb_cwpos_n13, 1); break;
    case 14: INC_STATS(tid, bb_cwpos_t14, time); INC_STATS(tid, bb_cwpos_n14, 1); break;
    case 15: INC_STATS(tid, bb_cwpos_t15, time); INC_STATS(tid, bb_cwpos_n15, 1); break;
    case 16: INC_STATS(tid, bb_cwpos_t16, time); INC_STATS(tid, bb_cwpos_n16, 1); break;
    case 17: INC_STATS(tid, bb_cwpos_t17, time); INC_STATS(tid, bb_cwpos_n17, 1); break;
    case 18: INC_STATS(tid, bb_cwpos_t18, time); INC_STATS(tid, bb_cwpos_n18, 1); break;
    case 19: INC_STATS(tid, bb_cwpos_t19, time); INC_STATS(tid, bb_cwpos_n19, 1); break;
    case 20: INC_STATS(tid, bb_cwpos_t20, time); INC_STATS(tid, bb_cwpos_n20, 1); break;
    case 21: INC_STATS(tid, bb_cwpos_t21, time); INC_STATS(tid, bb_cwpos_n21, 1); break;
    case 22: INC_STATS(tid, bb_cwpos_t22, time); INC_STATS(tid, bb_cwpos_n22, 1); break;
    case 23: INC_STATS(tid, bb_cwpos_t23, time); INC_STATS(tid, bb_cwpos_n23, 1); break;
    case 24: INC_STATS(tid, bb_cwpos_t24, time); INC_STATS(tid, bb_cwpos_n24, 1); break;
    case 25: INC_STATS(tid, bb_cwpos_t25, time); INC_STATS(tid, bb_cwpos_n25, 1); break;
    case 26: INC_STATS(tid, bb_cwpos_t26, time); INC_STATS(tid, bb_cwpos_n26, 1); break;
    case 27: INC_STATS(tid, bb_cwpos_t27, time); INC_STATS(tid, bb_cwpos_n27, 1); break;
    case 28: INC_STATS(tid, bb_cwpos_t28, time); INC_STATS(tid, bb_cwpos_n28, 1); break;
    case 29: INC_STATS(tid, bb_cwpos_t29, time); INC_STATS(tid, bb_cwpos_n29, 1); break;
    case 30: INC_STATS(tid, bb_cwpos_t30, time); INC_STATS(tid, bb_cwpos_n30, 1); break;
    case 31: INC_STATS(tid, bb_cwpos_t31, time); INC_STATS(tid, bb_cwpos_n31, 1); break;
    case 32: INC_STATS(tid, bb_cwpos_t32, time); INC_STATS(tid, bb_cwpos_n32, 1); break;
    }
}
#endif

#if BB_PF_CWX
void bb_rlh_add(uint64_t tid, int phase, uint64_t time) {
    switch (phase) {
    case 0: INC_STATS(tid, time_rlh0, time); break;
    case 1: INC_STATS(tid, time_rlh1, time); break;
    case 2: INC_STATS(tid, time_rlh2, time); break;
    case 3: INC_STATS(tid, time_rlh3, time); break;
    case 4: INC_STATS(tid, time_rlh4, time); break;
    case 5: INC_STATS(tid, time_rlh5, time); break;
    case 6: INC_STATS(tid, time_rlh6, time); break;
    case 7: INC_STATS(tid, time_rlh7, time); break;
    default: break;
    }
}
#endif

// ======================================================================================================
// [BB_PF_CWX] commit-wait chain tracer (declarations and description: stats.h)
// ======================================================================================================
#if BB_PF_CWX
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace cwx {   // [BB_PF_CWX] helpers
struct Rec { uint64_t id, cw_start, commit; uint64_t ioff; uint32_t icnt; uint32_t waited; };
struct Ev  { uint64_t succ, pred, t; };
struct Buf {
    std::vector<Rec> rec; std::vector<uint64_t> iv; std::vector<Ev> ev;   // iv: latch wait pieces (start, end, phase)
    bool collecting = false; uint64_t cur_ioff = 0; uint32_t cur_icnt = 0;
    uint64_t cur_cw = 0, cur_commit = 0; bool cur_waited = false;
};
std::mutex g_mu;
std::vector<Buf *> g_bufs;
thread_local Buf * tb = nullptr;

Buf * buf() {
    if (!tb) {
        tb = new Buf();
        size_t cap = 12000000 / (g_thread_cnt ? g_thread_cnt : 1) + 100000;   // reserved, not touched: no realloc in a run
        tb->rec.reserve(cap); tb->iv.reserve(cap * 3 * 32); tb->ev.reserve(cap * 2);
        std::lock_guard<std::mutex> g(g_mu); g_bufs.push_back(tb);
    }
    return tb;
}
}

void bb_cwx_latch_seg(uint64_t start, uint64_t end, int phase) {
    using namespace cwx;
    Buf * b = buf();
    if (!b->collecting) return;
    b->iv.push_back(start); b->iv.push_back(end); b->iv.push_back((uint64_t) phase); b->cur_icnt++;
}

void bb_cwx_latch(uint64_t start, uint64_t end) { bb_cwx_latch_seg(start, end, 8); }   // not split by holder

void bb_cwx_release_event(uint64_t succ_id, uint64_t pred_id, uint64_t t) {
    using namespace cwx;
    buf()->ev.push_back(Ev{succ_id, pred_id, t});
}

void bb_cwx_begin_release(uint64_t cw_start, uint64_t commit_ts, bool waited) {
    using namespace cwx;
    Buf * b = buf();
    b->collecting = true; b->cur_ioff = b->iv.size() / 3; b->cur_icnt = 0;
    b->cur_cw = cw_start; b->cur_commit = commit_ts; b->cur_waited = waited;
}

void bb_cwx_commit(uint64_t id) {
    using namespace cwx;
    Buf * b = buf();
    if (!b->collecting) return;
    b->rec.push_back(Rec{id, b->cur_cw, b->cur_commit, b->cur_ioff, b->cur_icnt, b->cur_waited ? 1u : 0u});
    b->collecting = false;
}

void bb_cwx_analyze() {
    using namespace cwx;
    // one table of committed attempts, latch waits re-based into one array
    std::vector<Rec> rec; std::vector<uint64_t> iv;
    size_t nrec = 0, niv = 0;
    for (Buf * b : g_bufs) { nrec += b->rec.size(); niv += b->iv.size(); }
    rec.reserve(nrec); iv.reserve(niv);
    for (Buf * b : g_bufs) {
        uint64_t base = iv.size() / 3;
        for (Rec r : b->rec) { r.ioff += base; rec.push_back(r); }
        iv.insert(iv.end(), b->iv.begin(), b->iv.end());
    }
    std::unordered_map<uint64_t, uint32_t> idx; idx.reserve(nrec * 2);
    for (uint32_t i = 0; i < rec.size(); i++) idx[rec[i].id] = i;
    // the release that let each committed attempt commit: the last one (barrier -> 0) at or before its commit
    std::vector<uint64_t> bind_t(rec.size(), 0); std::vector<int64_t> bind_p(rec.size(), -2);   // -2 none, -1 aborted pred
    for (Buf * b : g_bufs)
        for (const Ev & e : b->ev) {
            auto s = idx.find(e.succ); if (s == idx.end()) continue;   // the successor did not commit this attempt
            uint32_t si = s->second;
            if (e.t > rec[si].commit || e.t < bind_t[si]) continue;
            auto p = idx.find(e.pred);
            bind_t[si] = e.t; bind_p[si] = (p == idx.end()) ? -1 : (int64_t) p->second;
        }
    uint64_t rlh[9] = {0};   // chain latch wait by holder phase
    auto latch_in = [&](const Rec & p, uint64_t a, uint64_t z, uint64_t * by) -> uint64_t {   // latch waits of p inside [a, z]
        uint64_t s = 0;
        for (uint32_t k = 0; k < p.icnt; k++) {
            const uint64_t * q = &iv[3 * (p.ioff + k)];
            uint64_t x = std::max(q[0], a), y = std::min(q[1], z);
            if (y > x) { s += y - x; by[q[2] < 9 ? q[2] : 8] += y - x; }
        }
        return s;
    };
    uint64_t ex = 0, rl = 0, rw = 0, no = 0, ab = 0, ot = 0, links = 0, cnt = 0;
    for (uint32_t i = 0; i < rec.size(); i++) {
        if (!rec[i].waited) continue;
        const uint64_t Ts = rec[i].cw_start, Te = rec[i].commit;
        uint64_t s_ex = 0, s_rl = 0, s_rw = 0, s_no = 0, s_ab = 0, s_ln = 0;
        uint32_t cur = i; uint64_t hi = Te;
        for (int step = 0; step < 1000000; step++) {
            uint64_t lo = std::max(Ts, rec[cur].cw_start);
            if (hi <= lo) break;
            if (bind_p[cur] == -2) { if (cur != i) s_no += hi - lo; break; }   // a predecessor that did not wait: commit loop only
            uint64_t d = bind_t[cur];
            if (d < hi) s_no += hi - std::max(d, lo);                          // released, not committed yet
            if (d <= lo) break;
            if (bind_p[cur] == -1) { s_ab += d - lo; break; }                 // released by an aborted attempt
            const Rec & P = rec[bind_p[cur]]; s_ln++;
            if (P.commit > d) break;                                          // out of order (left to "other")
            uint64_t rs = std::max(P.commit, lo);                             // P returning its rows: [rs, d]
            if (d > rs) { uint64_t l = latch_in(P, rs, d, rlh); s_rl += l; s_rw += (d - rs) - l; }
            if (P.commit <= lo) break;
            if (P.cw_start > lo) s_ex += std::min(P.cw_start, P.commit) - lo;  // P still executing
            cur = (uint32_t) bind_p[cur]; hi = P.commit;                      // P's own commit wait: walk on
        }
        uint64_t tot = Te - Ts, sum = s_ex + s_rl + s_rw + s_no + s_ab;
        ex += s_ex; rl += s_rl; rw += s_rw; no += s_no; ab += s_ab; ot += tot > sum ? tot - sum : 0;
        links += s_ln; cnt++;
    }
    Stats_thd * s = stats._stats[0];
    s->time_cwx_exec += ex; s->time_cwx_rel_latch += rl; s->time_cwx_rel_work += rw; s->time_cwx_notice += no;
    s->time_cwx_aborted += ab; s->time_cwx_other += ot; s->bb_cwx_links += links; s->bb_cwx_cnt += cnt;
    s->time_cwx_rlh0 += rlh[0];
    s->time_cwx_rlh1 += rlh[1];
    s->time_cwx_rlh2 += rlh[2];
    s->time_cwx_rlh3 += rlh[3];
    s->time_cwx_rlh4 += rlh[4];
    s->time_cwx_rlh5 += rlh[5];
    s->time_cwx_rlh6 += rlh[6];
    s->time_cwx_rlh7 += rlh[7];
    s->time_cwx_rlh8 += rlh[8];
    printf("[cwx] committed=%zu waited=%lu events=%zu\n", rec.size(), cnt, (size_t) [&]{ size_t n = 0; for (Buf * b : g_bufs) n += b->ev.size(); return n; }());
}
#endif

// ======================================================================================================
// [BB_PF_PW] prevention-wait tracer (declarations and description: stats.h)
// ======================================================================================================
#if BB_PF_PW
#include <mutex>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <functional>

namespace pw {
struct Iv  { uint64_t s; uint32_t dur; uint32_t kref; };   // kref = kind << 24 | index into ext (SPIN)
struct Ext { uint64_t we; };                               // SPIN: end of the spin (wake events are matched to it)
struct Get { uint64_t req; uint64_t grant; };
struct Rec { uint64_t id, cw_start, commit, ioff, goff, eoff; uint32_t icnt, gcnt, buf, waited, aborted; };
struct Ev  { uint64_t a, b, t; };                          // (succ, pred, t) / (woken, waker, t)
struct Hold { uint64_t lid, s, e; int kind; };              // a latch holder's prevention / detection interval
struct Buf {
    std::vector<Iv> iv; std::vector<Ext> ext; std::vector<Get> get; std::vector<Rec> rec; std::vector<Ev> rel, wake;
    std::vector<Hold> hold;
    size_t a_iv = 0, a_ext = 0, a_get = 0;   // where the current attempt's log starts
    uint64_t cur_id = 0;                     // id of the current attempt (0: none yet)
    BbPwLocal loc;                           // the pending log of the current attempt (stats.h)
};
std::mutex g_mu;
std::vector<Buf *> g_bufs;
std::vector<Buf *> g_wbuf;            // the workers' buffers by thread id (bb_pw_prepare)
thread_local Buf * tb = nullptr;

// reserved per attempt (an attempt of a worker): enough that no vector grows during the run (bb_pw_analyze warns)
enum { PW_IV = 48, PW_EXT = 8, PW_GET = 24, PW_REL = 4, PW_WAKE = 4, PW_HOLD = 24 };
Buf * new_buf(size_t cap, bool touch) {
    Buf * b = new Buf(); b->loc.owner = b;
    b->iv.reserve(cap * PW_IV); b->ext.reserve(cap * PW_EXT); b->get.reserve(cap * PW_GET); b->rec.reserve(cap);
    b->rel.reserve(cap * PW_REL); b->wake.reserve(cap * PW_WAKE); b->hold.reserve(cap * PW_HOLD);
    if (touch) {   // fault the pages in now: during the run a first touch would take the kernel's mm lock
        b->iv.resize(b->iv.capacity()); b->iv.clear(); b->ext.resize(b->ext.capacity()); b->ext.clear();
        b->get.resize(b->get.capacity()); b->get.clear(); b->rec.resize(b->rec.capacity()); b->rec.clear();
        b->rel.resize(b->rel.capacity()); b->rel.clear(); b->wake.resize(b->wake.capacity()); b->wake.clear();
        b->hold.resize(b->hold.capacity()); b->hold.clear();
    }
    return b;
}
Buf * buf() {
    if (!tb) {   // a thread without a prepared buffer (e.g. the RCC detector): created on first use, small
        tb = new_buf(100000, false);
        std::lock_guard<std::mutex> g(g_mu); g_bufs.push_back(tb);
    }
    return tb;
}
}
thread_local BbPwLocal * bb_pw_loc = nullptr;
BbPwLocal * bb_pw_local_slow() { pw::Buf * b = pw::buf(); bb_pw_loc = &b->loc; return bb_pw_loc; }
// before the workers start (main, before the warmup): one buffer per worker, its pages faulted in, in parallel
void bb_pw_prepare(uint32_t nthreads) {
    using namespace pw;
    const size_t cap = (size_t) BB_PF_PW_ATTEMPTS / (nthreads ? nthreads : 1) + 100000;
    const uint64_t t0 = get_server_clock();
    g_wbuf.assign(nthreads, nullptr);
    std::vector<std::thread> th;
    for (uint32_t i = 0; i < nthreads; i++) th.emplace_back([i, cap] { g_wbuf[i] = new_buf(cap, true); });
    for (auto & t : th) t.join();
    for (Buf * b : g_wbuf) g_bufs.push_back(b);
    printf("[pw] buffers: %u workers x %zu attempts, prepared in %.1f s\n", nthreads, cap, (get_server_clock() - t0) / 1e9);
}
void bb_pw_thread_init(uint64_t thd_id) {   // warmup and run: same buffer
    using namespace pw; if (thd_id < g_wbuf.size()) { tb = g_wbuf[thd_id]; bb_pw_loc = &tb->loc; }
}
namespace pw {
inline void push_iv(Buf * b, uint64_t s, uint64_t e, int kind, uint32_t ref) {
    if (e <= s) return;
    uint64_t d = e - s; if (d > 0xffffffffULL) d = 0xffffffffULL;
    b->iv.push_back(Iv{s, (uint32_t) d, ((uint32_t) kind << 24) | (ref & 0xffffff)});
}
}
using namespace pw;   // the [BB_PF_PW] functions below (the cwx section comes first: no clash)

namespace pw {
thread_local uint64_t ts_p0 = 0, ts_p1 = 0;
inline uint64_t ns(uint64_t ticks) { return (uint64_t) ((double) ticks / CPU_FREQ); }   // as get_server_clock
}
void bb_pw_ts_pending(uint64_t start, uint64_t end) { ts_p0 = start; ts_p1 = end; }
// the pending log into the buffer, in order: pieces (with their ext / hold entries), requests, events
void bb_pw_spill(BbPwLocal * L) {
    Buf * b = (Buf *) L->owner;
    for (int i = 0; i < L->n; i++) {
        const BbPwPend & q = L->p[i];
        const uint64_t s = q.raw & BB_PW_RAW_S ? ns(q.s) : q.s, e = q.raw & BB_PW_RAW_E ? ns(q.e) : q.e;
        if (q.hk) {   // own prevention / detection piece, and a holder interval of the latch
            if (!(q.raw & BB_PW_NO_OWN)) push_iv(b, s, e, q.k, 0);
            if (e > s) b->hold.push_back(Hold{q.lid, s, e, q.hk});
        } else if (q.k == BB_PW_LATCH_RAW) {
            if (e > s) { push_iv(b, s, e, BB_PW_LATCH_RAW, (uint32_t) (b->ext.size() - b->a_ext)); b->ext.push_back(Ext{q.lid}); }
        } else if (q.k == BB_PW_SPIN) {
            if (e > s) { push_iv(b, s, e, BB_PW_SPIN, (uint32_t) (b->ext.size() - b->a_ext)); b->ext.push_back(Ext{e}); }
        } else push_iv(b, s, e, q.k, 0);
    }
    for (int i = 0; i < L->ng; i++) b->get.push_back(Get{L->g[i].req, L->g[i].grant});
    for (int i = 0; i < L->nev; i++) (L->ev[i].rel ? b->rel : b->wake).push_back(Ev{L->ev[i].a, L->ev[i].b, L->ev[i].t});
    L->n = L->ng = L->nev = 0;
}
// the record of the attempt whose log is complete (pending log moved first): committed, or aborted -- an aborted
// attempt keeps its log as a record of its own: others that waited behind it (woken by its rollback) are followed into
// its timeline; its rollback time is "other". It is not one of the analysed (committed) attempts.
static void bb_pw_close(Buf * b) {
    BbPwLocal * L = &b->loc;
    bb_pw_spill(L);
    if (L->commit)
        b->rec.push_back(Rec{L->commit_id, L->cw, L->cm, b->a_iv, b->a_get, b->a_ext, (uint32_t) (b->iv.size() - b->a_iv),
                             (uint32_t) (b->get.size() - b->a_get), 0, L->waited ? 1u : 0u, 0});
    else if (b->cur_id)
        b->rec.push_back(Rec{b->cur_id, 0, ~(uint64_t) 0, b->a_iv, b->a_get, b->a_ext, (uint32_t) (b->iv.size() - b->a_iv),
                             (uint32_t) (b->get.size() - b->a_get), 0, 0, 1});
    L->commit = false; L->in_cw = false; L->waited = false;
    b->cur_id = 0; b->a_iv = b->iv.size(); b->a_ext = b->ext.size(); b->a_get = b->get.size();
}
void bb_pw_attempt_begin(uint64_t id) {   // no lock or latch is held here: the previous attempt's log is moved now
    Buf * b = buf(); bb_pw_loc = &b->loc;
    bb_pw_close(b);
    b->cur_id = id;
    if (ts_p1) { bb_pw_own(ts_p0, ts_p1, BB_PW_TS); ts_p0 = ts_p1 = 0; }   // the timestamp of this attempt (Wound-Wait)
}

void bb_pw_analyze() {
    // attempts of every worker (committed and aborted); intervals stay in their buffers
    for (Buf * b : g_bufs) bb_pw_close(b);   // the last attempt of every worker
    std::vector<Rec> rec;
    for (uint32_t bi = 0; bi < g_bufs.size(); bi++) for (Rec r : g_bufs[bi]->rec) { r.buf = bi; rec.push_back(r); }
    {   // use of the reserved buffers (largest worker): a vector that outgrew its reservation reallocated during the run
        size_t mr = 0, mi = 0, mg = 0, mh = 0; bool grew = false;
        for (Buf * b : g_wbuf) {
            mr = std::max(mr, b->rec.size()); mi = std::max(mi, b->iv.size()); mg = std::max(mg, b->get.size()); mh = std::max(mh, b->hold.size());
            const size_t cap = (size_t) BB_PF_PW_ATTEMPTS / g_wbuf.size() + 100000;
            grew |= b->rec.size() > cap || b->iv.size() > cap * PW_IV || b->get.size() > cap * PW_GET || b->hold.size() > cap * PW_HOLD
                 || b->ext.size() > cap * PW_EXT || b->rel.size() > cap * PW_REL || b->wake.size() > cap * PW_WAKE;
        }
        printf("[pw] largest worker: %zu attempts, %zu iv, %zu get, %zu hold%s\n", mr, mi, mg, mh,
               grew ? "  WARNING: a buffer outgrew its reservation (raise BB_PF_PW_ATTEMPTS)" : "");
    }
    std::unordered_map<uint64_t, uint32_t> idx; idx.reserve(rec.size() * 2);
    for (uint32_t i = 0; i < rec.size(); i++) idx[rec[i].id] = i;
    const uint32_t NONE = 0xffffffffu;
    // commit wait: the last release (barrier -> 0) at or before the commit
    std::vector<uint64_t> bind_t(rec.size(), 0); std::vector<uint32_t> bind_p(rec.size(), NONE);
    for (Buf * b : g_bufs) for (const Ev & e : b->rel) {
        auto s = idx.find(e.a); if (s == idx.end()) continue;
        uint32_t si = s->second; if (e.t > rec[si].commit || e.t < bind_t[si]) continue;
        auto p = idx.find(e.b); bind_t[si] = e.t; bind_p[si] = p == idx.end() ? NONE : p->second;
    }
    // spin waits: the last wake at or before the end of the spin
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> wakes;   // woken id -> (t, waker id)
    for (Buf * b : g_bufs) for (const Ev & e : b->wake) if (idx.count(e.a)) wakes[e.a].push_back({e.t, e.b});
    for (auto & w : wakes) std::sort(w.second.begin(), w.second.end());
    // latch holders' prevention / detection intervals per latch (one holder at a time: they do not overlap); a
    // BB_PW_LATCH_RAW wait [s, e] on latch lid is split by them
    std::unordered_map<uint64_t, std::vector<Hold>> holds;
    for (Buf * b : g_bufs) for (const Hold & h : b->hold) holds[h.lid].push_back(h);
    for (auto & h : holds) std::sort(h.second.begin(), h.second.end(), [](const Hold & x, const Hold & y) { return x.s < y.s; });
    auto split = [&](uint64_t lid, uint64_t s, uint64_t e, const std::function<void(uint64_t, uint64_t, int)> & f) {
        auto it = holds.find(lid); if (it == holds.end()) return;
        const std::vector<Hold> & H = it->second;
        auto h = std::lower_bound(H.begin(), H.end(), s, [](const Hold & x, uint64_t t) { return x.e <= t; });
        for (; h != H.end() && h->s < e; ++h) { uint64_t a = std::max(h->s, s), z = std::min(h->e, e); if (z > a) f(a, z, h->kind); }
    };
    std::vector<std::vector<uint64_t>> sp_t(g_bufs.size()); std::vector<std::vector<uint32_t>> sp_w(g_bufs.size());
    for (uint32_t bi = 0; bi < g_bufs.size(); bi++) { sp_t[bi].assign(g_bufs[bi]->ext.size(), 0); sp_w[bi].assign(g_bufs[bi]->ext.size(), NONE); }
    for (uint32_t i = 0; i < rec.size(); i++) {
        const Rec & r = rec[i]; Buf * b = g_bufs[r.buf];
        auto w = wakes.find(r.id); if (w == wakes.end()) continue;
        for (uint32_t k = 0; k < r.icnt; k++) {
            const Iv & v = b->iv[r.ioff + k]; if ((v.kref >> 24) != BB_PW_SPIN) continue;
            uint64_t ei = r.eoff + (v.kref & 0xffffff), we = b->ext[ei].we;
            auto it = std::upper_bound(w->second.begin(), w->second.end(), std::pair<uint64_t, uint64_t>(we, ~(uint64_t) 0));
            if (it == w->second.begin()) continue;
            --it; auto p = idx.find(it->second);
            sp_t[r.buf][ei] = it->first; sp_w[r.buf][ei] = p == idx.end() ? NONE : p->second;
        }
    }
    // what transaction xi was doing in [a, z], charged to C[]: 0 prevention, 1 release work (after its commit, not
    // waiting for a latch), 2 latch wait behind non-prevention work or handoff, 3 wake-up (released, not yet going),
    // 4 other (executing, its own lock work). A spin or commit wait is charged to its waker / releasing predecessor.
    enum { C_PREV, C_REL, C_LATCH, C_WAKE, C_OTHER, C_DET, C_N };
    // tracking check: waiting whose cause could not be followed (no wake / release event matched, or it came from an
    // attempt that did not commit) is charged to "other"; it is also summed here, with the recursion-depth cut-offs
    uint64_t untraced = 0, n_spin = 0, n_spin_lost = 0, n_cw = 0, n_cw_lost = 0, n_deep = 0;
    std::function<void(uint32_t, uint64_t, uint64_t, int, uint64_t *)> attr =
        [&](uint32_t xi, uint64_t a, uint64_t z, int depth, uint64_t * C) {
        if (z <= a) return;
        if (depth > 100000) { C[C_OTHER] += z - a; untraced += z - a; n_deep++; return; }
        const Rec & r = rec[xi]; Buf * b = g_bufs[r.buf];
        const Iv * v0 = &b->iv[r.ioff], * v1 = v0 + r.icnt;
        const Iv * v = std::lower_bound(v0, v1, a, [](const Iv & x, uint64_t t) { return x.s + x.dur <= t; });
        uint64_t cov_pre = 0, cov_post = 0;   // covered by a logged piece, before / after r.commit
        for (; v < v1 && v->s < z; v++) {
            uint64_t s = std::max(v->s, a), e = std::min(v->s + v->dur, z); if (e <= s) continue;
            if ((v->kref >> 24) == BB_PW_LATCH_RAW) {   // only the parts behind a holder in prevention / detection
                const uint64_t lid = b->ext[r.eoff + (v->kref & 0xffffff)].we;
                uint64_t got = 0;
                auto cover = [&](uint64_t ps, uint64_t pe) {
                    uint64_t pre = std::min(pe, std::max(ps, r.commit)) - ps; cov_pre += pre; cov_post += (pe - ps) - pre; };
                split(lid, s, e, [&](uint64_t ps, uint64_t pe, int kind) {
                    C[kind == BB_PW_LATCH_D ? C_DET : C_PREV] += pe - ps; got += pe - ps;
                    if (!BB_PF_PW_FULL) cover(ps, pe); });
                if (BB_PF_PW_FULL) { cover(s, e); C[C_LATCH] += (e - s) - got; }   // the rest: behind other work / handoff
                continue;
            }
            uint64_t pre = std::min(e, std::max(s, r.commit)) - s; cov_pre += pre; cov_post += (e - s) - pre;
            switch (v->kref >> 24) {
            case BB_PW_LATCH: case BB_PW_CS: case BB_PW_TS: C[C_PREV] += e - s; break;
            case BB_PW_LATCH_O: C[C_LATCH] += e - s; break;
            case BB_PW_DCS: case BB_PW_LATCH_D: C[C_DET] += e - s; break;
            case BB_PW_SPIN: {
                uint64_t ei = r.eoff + (v->kref & 0xffffff), wt = sp_t[r.buf][ei];
                if (depth == 0) n_spin++;
                if (sp_w[r.buf][ei] == NONE) { C[C_OTHER] += e - s; untraced += e - s; if (depth == 0) n_spin_lost++; break; }
                uint64_t m = std::min(e, std::max(s, wt));
                attr(sp_w[r.buf][ei], s, m, depth + 1, C); C[C_WAKE] += e - m;
                break; }
            case BB_PW_CW: {
                if (depth == 0) n_cw++;
                if (bind_p[xi] == NONE) { C[C_OTHER] += e - s; untraced += e - s; if (depth == 0) n_cw_lost++; break; }
                uint64_t m = std::min(e, std::max(s, bind_t[xi]));
                attr(bind_p[xi], s, m, depth + 1, C); C[C_WAKE] += e - m;
                break; }
            }
        }
        uint64_t pre_len = std::min(z, std::max(a, r.commit)) - a, post_len = (z - a) - pre_len;
        C[C_OTHER] += pre_len > cov_pre ? pre_len - cov_pre : 0;   // executing / own lock work
        C[C_REL] += post_len > cov_post ? post_len - cov_post : 0;   // returning its rows after its commit
    };
    uint64_t lock = 0, lk_latch = 0, lk_cs = 0, lk_spin = 0, cw = 0, cw_prev = 0, ts_own = 0, det_x = 0;
    uint64_t LC[C_N] = {0}, CC[C_N] = {0};
    uint64_t n_commit = 0;
    for (uint32_t i = 0; i < rec.size(); i++) {
        const Rec & r = rec[i]; Buf * b = g_bufs[r.buf];
        if (r.aborted) continue;   // only committed attempts are analysed
        n_commit++;
        const Iv * v0 = &b->iv[r.ioff], * v1 = v0 + r.icnt;
        for (uint32_t g = 0; g < r.gcnt; g++) {   // each lock request: request -> grant
            const Get & q = b->get[r.goff + g]; if (q.grant <= q.req) continue;
            lock += q.grant - q.req;
            attr(i, q.req, q.grant, 0, LC);
            const Iv * v = std::lower_bound(v0, v1, q.req, [](const Iv & x, uint64_t t) { return x.s + x.dur <= t; });
            for (; v < v1 && v->s < q.grant; v++) {   // the prevention parts by kind
                uint64_t s = std::max(v->s, q.req), e = std::min(v->s + v->dur, q.grant); if (e <= s) continue;
                switch (v->kref >> 24) {
                case BB_PW_LATCH: lk_latch += e - s; break;
                case BB_PW_LATCH_RAW:
                    split(b->ext[r.eoff + (v->kref & 0xffffff)].we, s, e, [&](uint64_t ps, uint64_t pe, int kind) { if (kind == BB_PW_LATCH) lk_latch += pe - ps; });
                    break;
                case BB_PW_CS: lk_cs += e - s; break;
                case BB_PW_SPIN: {
                    uint64_t ei = r.eoff + (v->kref & 0xffffff);
                    if (sp_w[r.buf][ei] != NONE) {
                        uint64_t T[C_N] = {0}, u0 = untraced, d0 = n_deep;   // a second walk: not counted again
                        attr(sp_w[r.buf][ei], s, std::min(e, std::max(s, sp_t[r.buf][ei])), 1, T);
                        lk_spin += T[C_PREV]; untraced = u0; n_deep = d0;
                    }
                    break; }
                }
            }
        }
        for (uint32_t k = 0; k < r.icnt; k++) if ((v0[k].kref >> 24) == BB_PW_TS) ts_own += v0[k].dur;   // own timestamp (WW)
        // own detection work outside every lock request and outside the commit wait (it delays only this attempt's
        // execution / release; inside those windows it is counted by attr above)
        for (uint32_t k = 0; k < r.icnt; k++) {
            if ((v0[k].kref >> 24) != BB_PW_DCS) continue;
            uint64_t s0 = v0[k].s, e0 = v0[k].s + v0[k].dur, in = 0;
            auto ov = [&](uint64_t a, uint64_t z) { uint64_t s1 = std::max(s0, a), e1 = std::min(e0, z); if (e1 > s1) in += e1 - s1; };
            for (uint32_t g = 0; g < r.gcnt; g++) ov(b->get[r.goff + g].req, b->get[r.goff + g].grant);
            ov(r.cw_start, r.commit);
            det_x += (e0 - s0) > in ? (e0 - s0) - in : 0;
        }
        cw += r.commit - r.cw_start;
        uint64_t T[C_N] = {0}; attr(i, r.cw_start, r.commit, 0, T);
        for (int k = 0; k < C_N; k++) CC[k] += T[k];
        cw_prev += T[C_PREV];
    }
    Stats_thd * s = stats._stats[0];
    s->time_pw_lock += lock; s->time_pw_lock_latch += lk_latch; s->time_pw_lock_cs += lk_cs; s->time_pw_lock_spin += lk_spin;
    s->time_pw_cw += cw; s->time_pw_cw_prev += cw_prev; s->bb_pw_cnt += n_commit;
    s->time_pwl_prev += LC[C_PREV]; s->time_pwl_rel += LC[C_REL]; s->time_pwl_latch += LC[C_LATCH];
    s->time_pwl_wake += LC[C_WAKE]; s->time_pwl_other += LC[C_OTHER];
    s->time_pwc_prev += CC[C_PREV]; s->time_pwc_rel += CC[C_REL]; s->time_pwc_latch += CC[C_LATCH];
    s->time_pwc_wake += CC[C_WAKE]; s->time_pwc_other += CC[C_OTHER];
    s->time_pw_ts += ts_own;
    s->time_pwl_det += LC[C_DET]; s->time_pwc_det += CC[C_DET]; s->time_pw_det_x += det_x;
    s->time_pw_untraced += untraced; s->bb_pw_spin_cnt += n_spin; s->bb_pw_spin_lost += n_spin_lost;
    s->bb_pw_cw_cnt += n_cw; s->bb_pw_cw_lost += n_cw_lost; s->bb_pw_deep += n_deep;
    printf("[pw] committed=%lu aborted_attempts=%lu\n", (unsigned long) n_commit, (unsigned long) (rec.size() - n_commit));
}
#endif
