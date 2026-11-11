#pragma once

#define TMP_METRICS(x, y) \
  x(double, time_wait) x(double, time_man) x(double, time_index) x(double, time_copy) \
  x(double, time_commit) x(double, time_get_latch) x(double, time_get_cs) x(double, time_retire_latch) x(double, time_retire_cs)   /* [orig-acct] commit-only */ \
  x(double, time_bb_ts) x(double, time_bb_wound) x(double, time_bb_scan) x(double, time_bb_copy) y(uint64_t, bb_ts_cnt) y(uint64_t, bb_wound_cnt) y(uint64_t, bb_get_cnt)   /* BB_PF_PREVENT, commit-only */ \
  y(uint64_t, bb_lw_ahead) y(uint64_t, bb_lw_cnt) y(uint64_t, bb_cw_ahead) y(uint64_t, bb_cw_cnt)   /* BB_PF_AHEAD, commit-only */ \
  x(double, time_cws_exec) x(double, time_cws_cascade) x(double, time_cws_release) x(double, time_cws_notice) x(double, time_cws_other) y(uint64_t, bb_cws_cnt)   /* BB_PF_CWSPLIT */ \
  x(double, time_cwf_exec) x(double, time_cwf_release) x(double, time_cwf_notice) x(double, time_cwf_other) y(uint64_t, bb_cwf_depth)   /* BB_PF_CWSPLIT, flattened */
#if CC_ALG == RCC
#define RCC_INDEX_METRICS(y) y(uint64_t, idx_dup_commit)
#else
#define RCC_INDEX_METRICS(y)
#endif
#if BB_PF_CWPOS
#define BB_CWPOS_BUCKETS 33
#define BB_CWPOS_METRICS(x, y) x(double, bb_cwpos_t0) y(uint64_t, bb_cwpos_n0) x(double, bb_cwpos_t1) y(uint64_t, bb_cwpos_n1) x(double, bb_cwpos_t2) y(uint64_t, bb_cwpos_n2) x(double, bb_cwpos_t3) y(uint64_t, bb_cwpos_n3) x(double, bb_cwpos_t4) y(uint64_t, bb_cwpos_n4) x(double, bb_cwpos_t5) y(uint64_t, bb_cwpos_n5) x(double, bb_cwpos_t6) y(uint64_t, bb_cwpos_n6) x(double, bb_cwpos_t7) y(uint64_t, bb_cwpos_n7) x(double, bb_cwpos_t8) y(uint64_t, bb_cwpos_n8) x(double, bb_cwpos_t9) y(uint64_t, bb_cwpos_n9) x(double, bb_cwpos_t10) y(uint64_t, bb_cwpos_n10) x(double, bb_cwpos_t11) y(uint64_t, bb_cwpos_n11) x(double, bb_cwpos_t12) y(uint64_t, bb_cwpos_n12) x(double, bb_cwpos_t13) y(uint64_t, bb_cwpos_n13) x(double, bb_cwpos_t14) y(uint64_t, bb_cwpos_n14) x(double, bb_cwpos_t15) y(uint64_t, bb_cwpos_n15) x(double, bb_cwpos_t16) y(uint64_t, bb_cwpos_n16) x(double, bb_cwpos_t17) y(uint64_t, bb_cwpos_n17) x(double, bb_cwpos_t18) y(uint64_t, bb_cwpos_n18) x(double, bb_cwpos_t19) y(uint64_t, bb_cwpos_n19) x(double, bb_cwpos_t20) y(uint64_t, bb_cwpos_n20) x(double, bb_cwpos_t21) y(uint64_t, bb_cwpos_n21) x(double, bb_cwpos_t22) y(uint64_t, bb_cwpos_n22) x(double, bb_cwpos_t23) y(uint64_t, bb_cwpos_n23) x(double, bb_cwpos_t24) y(uint64_t, bb_cwpos_n24) x(double, bb_cwpos_t25) y(uint64_t, bb_cwpos_n25) x(double, bb_cwpos_t26) y(uint64_t, bb_cwpos_n26) x(double, bb_cwpos_t27) y(uint64_t, bb_cwpos_n27) x(double, bb_cwpos_t28) y(uint64_t, bb_cwpos_n28) x(double, bb_cwpos_t29) y(uint64_t, bb_cwpos_n29) x(double, bb_cwpos_t30) y(uint64_t, bb_cwpos_n30) x(double, bb_cwpos_t31) y(uint64_t, bb_cwpos_n31) x(double, bb_cwpos_t32) y(uint64_t, bb_cwpos_n32)
#else
#define BB_CWPOS_METRICS(x, y)
#endif
#if BB_PF_CWX
#define BB_CWX_METRICS(x, y) x(double, time_cwx_exec) x(double, time_cwx_rel_latch) x(double, time_cwx_rel_work) x(double, time_cwx_notice) x(double, time_cwx_aborted) x(double, time_cwx_other) y(uint64_t, bb_cwx_links) y(uint64_t, bb_cwx_cnt) x(double, time_cwx_rlh0) x(double, time_cwx_rlh1) x(double, time_cwx_rlh2) x(double, time_cwx_rlh3) x(double, time_cwx_rlh4) x(double, time_cwx_rlh5) x(double, time_cwx_rlh6) x(double, time_cwx_rlh7) x(double, time_cwx_rlh8) x(double, time_rlh0) x(double, time_rlh1) x(double, time_rlh2) x(double, time_rlh3) x(double, time_rlh4) x(double, time_rlh5) x(double, time_rlh6) x(double, time_rlh7)
#else
#define BB_CWX_METRICS(x, y)
#endif
#if BB_PF_PW
#define BB_PW_METRICS(x, y) x(double, time_pw_lock) x(double, time_pw_lock_latch) x(double, time_pw_lock_cs) x(double, time_pw_lock_spin) x(double, time_pw_cw) x(double, time_pw_cw_prev) y(uint64_t, bb_pw_cnt) x(double, time_pwl_prev) x(double, time_pwl_rel) x(double, time_pwl_latch) x(double, time_pwl_wake) x(double, time_pwl_other) x(double, time_pwc_prev) x(double, time_pwc_rel) x(double, time_pwc_latch) x(double, time_pwc_wake) x(double, time_pwc_other) x(double, time_pw_untraced) x(double, time_pw_ts) x(double, time_pwl_det) x(double, time_pwc_det) x(double, time_pw_det_x) y(uint64_t, bb_pw_spin_cnt) y(uint64_t, bb_pw_spin_lost) y(uint64_t, bb_pw_cw_cnt) y(uint64_t, bb_pw_cw_lost) y(uint64_t, bb_pw_deep)
#else
#define BB_PW_METRICS(x, y)
#endif
#define ALL_METRICS(x, y, z) \
  y(uint64_t, txn_cnt) y(uint64_t, abort_cnt) y(uint64_t, user_abort_cnt) \
  y(uint64_t, abort_cnt_neworder) y(uint64_t, abort_cnt_payment)          \
  y(uint64_t, abort_cnt_delivery) y(uint64_t, abort_cnt_order_status) y(uint64_t, abort_cnt_stock_level)  \
  x(double, run_time) x(double, time_abort) x(double, time_cleanup)       \
  x(double, time_lockrow) x(double, time_exec) x(double, time_assign) x(double, time_creat_entry) \
  x(double, time_creat_version) x(double, time_read_write) x(double, time_verify)  \
  x(double, time_wound) x(double, time_wound_cascad) \
  x(double, time_abort_processing) x(double, time_commit_processing) \
  x(double, time_query) \
  x(double, time_copy_latch) \
  x(double, time_release_latch) x(double, time_release_cs) x(double, time_semaphore_cs) \
  x(double, total_read_wait) x(double, total_write_wait)   \
  y(uint64_t, abort_position) y(uint64_t, abort_position_cnt) y(uint64_t, abort_hotspot) \
  y(uint64_t, time_ts_alloc) y(uint64_t, wait_cnt) \
  y(uint64_t, latency) y(uint64_t, commit_latency) y(uint64_t, abort_length) \
  y(uint64_t, cascading_abort_times) z(uint64_t, max_abort_length) \
  y(uint64_t, find_circle_abort_depent)\
  y(uint64_t, txn_cnt_long) y(uint64_t, abort_cnt_long) y(uint64_t, cascading_abort_cnt) \
  y(uint64_t, lock_acquire_cnt) y(uint64_t, lock_directly_cnt) y(uint64_t, blind_kill_count)  \
  y(uint64_t, rcc_lock_timeout_cnt) y(uint64_t, rcc_lock_deadlock_cnt) y(uint64_t, rcc_commit_timeout_cnt) \
  y(uint64_t, rcc_join_block_cnt) y(uint64_t, wound_abort_cnt) \
  y(uint64_t, cw_enter_cnt) y(uint64_t, cw_pred_sum) x(double, time_release_phase) y(uint64_t, release_phase_cnt) x(double, time_relay) y(uint64_t, relay_cnt) z(uint64_t, relay_max) \
  y(uint64_t, cwp_exec) y(uint64_t, cwp_lockwait) y(uint64_t, cwp_cw) y(uint64_t, cwp_done) y(uint64_t, cwp_nopred) y(uint64_t, batch_request_cnt) y(uint64_t, batch_latch_cnt) y(uint64_t, batch_covered_cnt) y(uint64_t, time_version_install) y(uint64_t, install_latch_cnt) y(uint64_t, rcc_install_cnt) y(uint64_t, abort_lat_sum) y(uint64_t, abort_lat_cnt) y(uint64_t, idx_read_cnt) y(uint64_t, getrow_cnt) y(uint64_t, acc_pay) y(uint64_t, acc_no) y(uint64_t, acc_del) y(uint64_t, acc_os) y(uint64_t, acc_sl) \
  y(uint64_t, batch_swap_cnt) y(uint64_t, batch_cleanup_help_cnt) y(uint64_t, batch_cleanup_skip_cnt) y(uint64_t, batch_cleanup_late_cnt) \
  x(double, lat_abort_prop) y(uint64_t, lat_abort_prop_cnt) x(double, lat_abort_notice) y(uint64_t, lat_abort_notice_cnt) x(double, lat_commit_prop) y(uint64_t, lat_commit_prop_cnt) x(double, time_commit_proc) \
  y(uint64_t, rcc_pc_batches) y(uint64_t, rcc_pc_rows) y(uint64_t, rcc_pc_help_rows) \
  x(double, rcc_pc_help_time) x(double, rcc_pc_join_time) \
  y(uint64_t, rcc_pc_failed_help) x(double, rcc_pc_failed_help_time) \
  RCC_INDEX_METRICS(y) \
  BB_CWPOS_METRICS(x, y) \
  BB_CWX_METRICS(x, y) \
  BB_PW_METRICS(x, y) \
  TMP_METRICS(x, y)

//y(uint64_t, wound_times)  y(uint64_t, wound_depend_cnt)  y(uint64_t, wound_depent_cnt) \
//  y(uint64_t, wound_retire_cnt)  y(uint64_t, wound_wait_cnt)   \

#define DECLARE_VAR(tpe, name) tpe name;
#define INIT_VAR(tpe, name) name = 0;
#define INIT_TOTAL_VAR(tpe, name) tpe total_##name = 0;
#define SUM_UP_STATS(tpe, name) total_##name += _stats[tid]->name;
#define MAX_STATS(tpe, name) total_##name = max(total_##name, _stats[tid]->name);

#define STR(x) #x
#define XSTR(x) STR(x)
#define STR_X(tpe, name) XSTR(name)
#define VAL_X(tpe, name) total_##name / BILLION         // VAL_X: calculate the metrics that is relevant to time, so it needs to be divided by billion.
#define VAL_Y(tpe, name) total_##name

#define PRINT_STAT_X(tpe, name) \
  std::cout << STR_X(tpe, name) << "= " << VAL_X(tpe, name) << ", ";
#define PRINT_STAT_Y(tpe, name) \
  std::cout << STR_X(tpe, name) << "= " << VAL_Y(tpe, name) << ", ";
#define WRITE_STAT_X(tpe, name) \
  outf << STR_X(tpe, name) << "= " << VAL_X(tpe, name) << ", ";
#define WRITE_STAT_Y(tpe, name) \
  outf << STR_X(tpe, name) << "= " << VAL_Y(tpe, name) << ", ";

class Stats_thd {
public:
    void init(uint64_t thd_id);
    void clear();

    char _pad2[CL_SIZE];

    // Declare all metrics(except for metrics for statistic purpose, eg. total_XX)
    ALL_METRICS(DECLARE_VAR, DECLARE_VAR, DECLARE_VAR)

    uint64_t * all_debug1;
    uint64_t * all_debug2;
    char _pad[CL_SIZE];
};

class Stats_tmp {
public:
    void init();
    void clear();

    // temporary stats only include 3 metrics
    double time_man;
    double time_index;
    double time_wait;
    double time_copy;
    double time_commit, time_get_latch, time_get_cs, time_retire_latch, time_retire_cs;   // [orig-acct]
    double time_bb_ts, time_bb_wound, time_bb_scan, time_bb_copy; uint64_t bb_ts_cnt, bb_wound_cnt, bb_get_cnt;      // BB_PF_PREVENT
    uint64_t bb_lw_ahead, bb_lw_cnt, bb_cw_ahead, bb_cw_cnt;                    // BB_PF_AHEAD
    double time_cws_exec, time_cws_cascade, time_cws_release, time_cws_notice, time_cws_other; uint64_t bb_cws_cnt;   // BB_PF_CWSPLIT
    double time_cwf_exec, time_cwf_release, time_cwf_notice, time_cwf_other; uint64_t bb_cwf_depth;
};

class Stats {
public:
    // PER THREAD statistics
    Stats_thd ** _stats;
    // stats are first written to tmp_stats, if the txn successfully commits,
    // copy the values in tmp_stats to _stats
    Stats_tmp ** tmp_stats;

    // GLOBAL statistics
    double dl_detect_time;
    double dl_wait_time;
    uint64_t cycle_detect;
    uint64_t deadlock;
    // [RCC port]
    uint64_t rcc_false_positive;
    uint64_t rcc_fp_committed;
    uint64_t rcc_cycle_len_sum;
    uint64_t rcc_cycle_len_max;
    uint64_t rcc_dl_lockwait;
    uint64_t rcc_dl_commitwait;
    uint64_t rcc_dl_running;
    uint64_t rcc_confirmed;
    uint64_t rcc_snap_drop;
    uint64_t rcc_snap_time;
    uint64_t rcc_dfs_time;
    uint64_t rcc_round_time;
    uint64_t rcc_rounds;
    uint64_t rcc_persist_filtered;
    double   rcc_derive_time;
    uint64_t rcc_round_cache_sound;
    uint64_t rcc_round_cache_exact;

    int * created_long_txn_cnt;

    void init();
    void init(uint64_t thread_id);
    void clear(uint64_t tid);
    void add_debug(uint64_t thd_id, uint64_t value, uint32_t select);
    void commit(uint64_t thd_id);
    void abort(uint64_t thd_id);
    void print();
    void print_lat_distr();
};

// [RCC port] row_rcc.cpp uses PROF_CS(tid, sec, val); map to the existing time_* sums.
#define PROF_CS(tid, sec, val) INC_STATS(tid, time_##sec, (val))

#if BB_PF_CWPOS
void bb_cwpos_add(uint64_t tid, uint64_t pos, uint64_t time);   // txn.cpp, Bamboo commit wait (stats.cpp)
#endif

#if BB_PF_CWX
void bb_rlh_add(uint64_t tid, int phase, uint64_t time);   // lock_release latch wait by holder phase (stats.cpp)
#endif

// ======================================================================================================
// Measurement-only tracers (off by default, config-std.h); implemented in stats.cpp
// ======================================================================================================
// [prevention wait] BB_PF_PW (config-std.h): how much of a committed Bamboo transaction's waiting -- its lock wait
// (request -> grant of every lock request) and its commit wait -- was caused by deadlock prevention (timestamp
// assignment, scanning the retired / waiter lists, wounding), exactly, along everything that made it wait.
//
// Logged per attempt in the worker's own memory (an aborted attempt's log is kept as a record of its own, so waits
// behind it can be followed; only committed attempts are analysed):
//   K_LATCH  a piece of a row-latch wait (lock_get / lock_retire / lock_release) while the holder was in a prevention
//            phase (row_bamboo.h BB_PH_TS / SCAN / WOUND)
//   K_CS     this transaction's own prevention work while holding the latch in lock_get
//   K_SPIN   a lock wait after lock_get returned WAIT; the transaction that woke it is found from the wake events
//   K_CW     the commit wait; the predecessor that released it is found from the release events (barrier -> 0)
// After the run, bb_pw_analyze() charges each window of waiting to what made it wait: a prevention piece counts as
// prevention; a spin or commit wait is charged to what its waker / releasing predecessor was doing in that window,
// recursively. Anything else (execution, release work, wake-up, latch held for other work) is "other".
// Results (summed over committed transactions, thread 0 of the stats):
//   time_pw_lock       lock wait (request -> grant)      time_pw_cw       commit wait
//   time_pw_lock_latch   ... latch wait, holder in prevention         time_pw_cw_prev  ... caused by prevention
//   time_pw_lock_cs      ... own prevention work under the latch
//   time_pw_lock_spin    ... spin wait, caused by prevention (through the waker)
//   bb_pw_cnt          committed transactions

#if BB_PF_PW
// BB_PW_DCS / BB_PW_LATCH_D (RCC): own deadlock-detection work on the worker path (edges / publishing for the detector,
// the wait for its approval), and a latch wait behind a holder doing such work -- the "detection" category (time_pw*_det)
// BB_PW_LATCH_RAW: a latch wait as one piece; after the run it is split by the latch holders' prevention / detection
// intervals on the same latch (bb_pw_hold) into BB_PW_LATCH / BB_PW_LATCH_D (the rest: BB_PW_LATCH_O with
// BB_PF_PW_FULL, else not prevention).  Nothing is shared at run time: the holder no longer publishes its phase next to
// the latch, whose writes (several per request, in the cache line every spinner polls) lengthened the critical sections
// and changed the run (RCC_ODH false, 128 threads: 18% -> 54% aborts, Bamboo 76% -> 84%).
enum { BB_PW_LATCH = 1, BB_PW_CS = 2, BB_PW_SPIN = 3, BB_PW_CW = 4, BB_PW_LATCH_O = 5, BB_PW_TS = 6, BB_PW_DCS = 7, BB_PW_LATCH_D = 8,
       BB_PW_LATCH_RAW = 9 };
void bb_pw_prepare(uint32_t nthreads);                               // main, before the workers: their buffers, pre-faulted
void bb_pw_thread_init(uint64_t thd_id);                             // worker start: its prepared buffer
// During the run every hook only stores into this worker's pending log (BbPwLocal, inline, no allocation, no shared
// memory); the log is moved into the worker's buffer when the next attempt starts (no lock or latch held then), or when
// it is full.  Work right after a grant or a latch release (copying the log into vectors) delays the holder before
// its retire / the next request, which RCC_ODH false amplifies: 18% -> 45% aborts at 128 threads (YCSB 1M rows, 90%
// writes); with the log moved at the attempt start the traced run aborts as the untraced one.
struct BbPwPend { uint64_t s, e, lid; int k, hk, raw; };   // a piece: kind k, hold kind hk (CS pieces), raw bits below
struct BbPwGet { uint64_t req, grant; };
struct BbPwEv  { uint64_t a, b, t; int rel; };             // wake (woken, waker, t) / release (succ, pred, t)
enum { BB_PW_RAW_S = 1, BB_PW_RAW_E = 2, BB_PW_NO_OWN = 4,  // raw: start / end in bb_pw_ticks(); own piece not logged
       BB_PW_LOC_N = 2048, BB_PW_LOC_G = 512, BB_PW_LOC_EV = 1024 };
struct BbPwLocal {
    int n = 0, ng = 0, nev = 0;
    bool in_cw = false;                      // in the commit wait: own latch pieces are part of it, not logged
    bool commit = false; uint64_t commit_id = 0, cw = 0, cm = 0; bool waited = false;   // this attempt committed
    void * owner = nullptr;                  // the buffer (system/stats.cpp)
    BbPwPend p[BB_PW_LOC_N]; BbPwGet g[BB_PW_LOC_G]; BbPwEv ev[BB_PW_LOC_EV];
};
extern thread_local BbPwLocal * bb_pw_loc;
BbPwLocal * bb_pw_local_slow();                                      // a thread without a prepared buffer
void bb_pw_spill(BbPwLocal * L);                                     // the pending log into the buffer
inline BbPwLocal * bb_pw_local() { BbPwLocal * L = bb_pw_loc; return __builtin_expect(L != nullptr, 1) ? L : bb_pw_local_slow(); }
inline void bb_pw_defer(uint64_t s, uint64_t e, uint64_t lid, int k, int hk, int raw) {
    BbPwLocal * L = bb_pw_local();
    if (__builtin_expect(L->n == BB_PW_LOC_N, 0)) bb_pw_spill(L);
    L->p[L->n++] = BbPwPend{s, e, lid, k, hk, raw};
}
void bb_pw_prepare(uint32_t nthreads);                               // main, before the workers: their buffers, pre-faulted
void bb_pw_thread_init(uint64_t thd_id);                             // worker start: its prepared buffer
void bb_pw_attempt_begin(uint64_t id);                               // txn_man::set_txn_id: a new attempt (the log of the
                                                                     // previous one is moved now)
void bb_pw_ts_pending(uint64_t start, uint64_t end);                 // Wound-Wait: timestamp taken just before the attempt
inline void bb_pw_cw_enter() { bb_pw_local()->in_cw = true; }        // commit wait starts (own latch pieces not logged)
// a piece of this attempt (K_LATCH / K_CS ...), not inside the commit wait
inline void bb_pw_iv(uint64_t start, uint64_t end, int kind) { BbPwLocal * L = bb_pw_local(); if (!L->in_cw) bb_pw_defer(start, end, 0, kind, 0, 0); }
inline void bb_pw_iv_defer(uint64_t start, uint64_t end, int kind) { bb_pw_iv(start, end, kind); }
// a latch wait on latch lid, split after the run by the holders' intervals on it (BB_PW_LATCH_RAW)
inline void bb_pw_latch_wait(uint64_t lid, uint64_t start, uint64_t end) {
    BbPwLocal * L = bb_pw_local(); if (!L->in_cw) bb_pw_defer(start, end, lid, BB_PW_LATCH_RAW, 0, 0);
}
// latch holder: own piece [start, end] of `kind` (BB_PW_CS / BB_PW_DCS), which is also a holder interval of latch lid
// (hold_kind BB_PW_LATCH / BB_PW_LATCH_D); raw: BB_PW_RAW_S / BB_PW_RAW_E for bb_pw_ticks() values, else ns
inline void bb_pw_cs_piece(uint64_t lid, uint64_t start, uint64_t end, int kind, int hold_kind, int raw) {
    BbPwLocal * L = bb_pw_local(); bb_pw_defer(start, end, lid, kind, hold_kind, raw | (L->in_cw ? BB_PW_NO_OWN : 0));
}
// the cheapest clock inside a latch: rdtsc without the conversion of get_sys_clock()
inline uint64_t bb_pw_ticks() { unsigned hi, lo; __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t) lo) | (((uint64_t) hi) << 32); }
inline void bb_pw_spin(uint64_t ws, uint64_t we) { if (we > ws) bb_pw_defer(ws, we, 0, BB_PW_SPIN, 0, 0); }   // a spin wait
inline void bb_pw_get(uint64_t req, uint64_t grant) {                // a lock request of this attempt, granted
    BbPwLocal * L = bb_pw_local(); if (__builtin_expect(L->ng == BB_PW_LOC_G, 0)) bb_pw_spill(L);
    L->g[L->ng++] = BbPwGet{req, grant};
}
inline void bb_pw_event(uint64_t a, uint64_t b, uint64_t t, int rel) {
    BbPwLocal * L = bb_pw_local(); if (__builtin_expect(L->nev == BB_PW_LOC_EV, 0)) bb_pw_spill(L);
    L->ev[L->nev++] = BbPwEv{a, b, t, rel};
}
inline void bb_pw_wake(uint64_t woken_id, uint64_t waker_id, uint64_t t) { bb_pw_event(woken_id, waker_id, t, 0); }   // waker set woken's lock_ready at t
inline void bb_pw_release_event(uint64_t succ_id, uint64_t pred_id, uint64_t t) { bb_pw_event(succ_id, pred_id, t, 1); }   // pred brought succ's barrier to 0
// committed, before its release; the waiting piece is [cw_s, cw_e] if given
inline void bb_pw_commit_begin(uint64_t cw_start, uint64_t commit_ts, bool waited, uint64_t cw_s = 0, uint64_t cw_e = 0) {
    BbPwLocal * L = bb_pw_local(); L->cw = cw_start; L->cm = commit_ts; L->waited = waited; L->in_cw = false;
    if (!cw_e) { cw_s = cw_start; cw_e = commit_ts; }
    if (waited) bb_pw_defer(cw_s, cw_e, 0, BB_PW_CW, 0, 0);
}
inline void bb_pw_own(uint64_t start, uint64_t end, int kind) { bb_pw_defer(start, end, 0, kind, 0, 0); }   // own piece, also inside the commit wait
inline void bb_pw_commit(uint64_t id) { BbPwLocal * L = bb_pw_local(); L->commit = true; L->commit_id = id; }   // release over
void bb_pw_analyze();                                                // end of run, before stats.print()
#endif

// [cw-exact] BB_PF_CWX (config-std.h): exact split of Bamboo commit waits along the chain of predecessors.
//
// During the run each worker logs, in its own memory buffer:
//   - per committed attempt: commit-wait start, commit time, and the row-latch waits of its release phase
//     (lock_release after the commit, until its rows are returned);
//   - per release that brings a successor's commit barrier to 0 (update_entry): successor, releasing attempt, time.
// After the run, bb_cwx_analyze() walks every waiting commit back through the transactions that released it, splitting
// its commit wait exactly by what those predecessors were doing (no proportional estimate), into stats._stats[0]:
//   time_cwx_exec      a predecessor was still executing (before its own commit wait)
//   time_cwx_rel_latch a predecessor was returning its rows after its commit, waiting for a row latch
//   time_cwx_rel_work  ... returning its rows, the rest (latch held, copies, other rows)
//   time_cwx_notice    a waiter was released and had not committed yet (spin + CAS)
//   time_cwx_aborted   released by an aborted attempt: the time before that release
//   time_cwx_other     timestamps that do not fit the chain (should be 0)
//   bb_cwx_links       predecessors walked, bb_cwx_cnt commit waits split
//   time_cwx_rlh<k>    time_cwx_rel_latch by the latch holder's phase k (row_bamboo.h BB_PH_*; 8 = not split)

#if BB_PF_CWX
void bb_cwx_latch(uint64_t start, uint64_t end);                         // a lock_release latch wait (own thread)
void bb_cwx_latch_seg(uint64_t start, uint64_t end, int phase);          // a piece of one, by holder phase (BB_PF_HOLDER)
void bb_cwx_release_event(uint64_t succ_id, uint64_t pred_id, uint64_t t);  // pred brought succ's barrier to 0 at t
void bb_cwx_begin_release(uint64_t cw_start, uint64_t commit_ts, bool waited);  // committed: release phase starts
void bb_cwx_commit(uint64_t id);                                          // release phase over: log the attempt
void bb_cwx_analyze();                                                    // end of run, before stats.print()
#endif
