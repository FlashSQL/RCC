#include "tpcc_check.h"
#if WORKLOAD == TPCC && TPCC_CONSISTENCY
#include "tpcc.h"
#include "tpcc_query.h"
#include "tpcc_helper.h"
#include "tpcc_const.h"
#include "table.h"
#include "row.h"
#include "index_base.h"
#include <cmath>
#include <vector>

// Per-thread tallies of what committed (no atomics on the hot path).
struct ChkThd {
    std::vector<double> d_amt;      // distKey -> sum of committed Payment h_amount
    std::vector<uint64_t> no_cnt;   // distKey -> committed New-Order count
    std::vector<uint64_t> no_ol;    // distKey -> sum of committed New-Order ol_cnt
    double pm_amt = 0; uint64_t pm_cnt = 0, pm_byname = 0;
    char pad[64];
};
static ChkThd * g_chk = NULL;
static uint64_t g_chk_keys = 0;

// Every worker must observe fully initialized vectors before recording a commit.
static pthread_once_t g_chk_once = PTHREAD_ONCE_INIT;
static void init_check_tallies() {
    g_chk_keys = (g_num_wh + 1) * DIST_PER_WARE + 1;
    g_chk = new ChkThd[g_thread_cnt];
    for (uint64_t i = 0; i < g_thread_cnt; i++) {
        g_chk[i].d_amt.assign(g_chk_keys, 0.0);
        g_chk[i].no_cnt.assign(g_chk_keys, 0);
        g_chk[i].no_ol.assign(g_chk_keys, 0);
    }
}

void tpcc_check_commit(uint64_t thd_id, tpcc_query * q) {
    pthread_once(&g_chk_once, init_check_tallies);
    ChkThd & c = g_chk[thd_id];
    if (q->type == TPCC_PAYMENT) {
        auto & a = q->args.payment;
        c.d_amt[distKey(a.d_id, a.w_id)] += a.h_amount;
        c.pm_amt += a.h_amount; c.pm_cnt++; if (a.by_last_name) c.pm_byname++;
    } else if (q->type == TPCC_NEW_ORDER) {
        auto & a = q->args.new_order;
        c.no_cnt[distKey(a.d_id, a.w_id)]++; c.no_ol[distKey(a.d_id, a.w_id)] += a.ol_cnt;
    }
}

static row_t * lookup(index_base * idx, uint64_t key, int part) {
    itemid_t * item = NULL;
    if (idx->index_read(key, item, part, 0) != RCOK || item == NULL) return NULL;
    return (row_t *) item->location;
}

static int g_fail = 0;
static void result(const char * name, uint64_t errors, const char * detail = "") {
    printf("[consistency] %-5s %-64s errors=%lu %s\n", errors ? "ERROR" : "OK", name, errors, detail);
    if (errors) g_fail++;
}

void tpcc_consistency_check(tpcc_wl * wl) {
    const double EPS = 0.01;
    if (!g_chk) { printf("[consistency] no committed transactions recorded\n"); return; }
    // merge per-thread tallies
    std::vector<double> d_amt(g_chk_keys, 0.0); std::vector<uint64_t> no_cnt(g_chk_keys, 0), no_ol(g_chk_keys, 0);
    double pm_amt = 0; uint64_t pm_cnt = 0, pm_byname = 0, no_total = 0;
    for (uint64_t i = 0; i < g_thread_cnt; i++) {
        for (uint64_t k = 0; k < g_chk_keys; k++) { d_amt[k] += g_chk[i].d_amt[k]; no_cnt[k] += g_chk[i].no_cnt[k]; no_ol[k] += g_chk[i].no_ol[k]; no_total += g_chk[i].no_cnt[k]; }
        pm_amt += g_chk[i].pm_amt; pm_cnt += g_chk[i].pm_cnt; pm_byname += g_chk[i].pm_byname;
    }
    printf("[consistency] committed: new-order=%lu payment=%lu (by last name %lu) payment amount=%.2f\n", no_total, pm_cnt, pm_byname, pm_amt);

    uint64_t e_wytd = 0, e_wytd_amt = 0, e_dytd = 0, e_next = 0, e_order_missing = 0, e_no_missing = 0, e_no_extra = 0,
             e_carrier = 0, e_ol_cnt = 0, e_ol_missing = 0, e_ol_extra = 0, e_orphan = 0, e_ol_sum = 0;
    double diff_w = 0, diff_d = 0;
    uint64_t orders_seen = 0, ol_seen = 0;
    for (uint64_t w = 1; w <= g_num_wh; w++) {
        int part = wh_to_part(w);
        row_t * wh = lookup(wl->i_warehouse, w, part);
        double w_ytd = 0; if (wh) wh->get_value(W_YTD, w_ytd); else { e_wytd++; continue; }
        double sum_d_ytd = 0, w_amt = 0;
        for (uint64_t d = 1; d <= DIST_PER_WARE; d++) {
            uint64_t dk = distKey(d, w);
            row_t * dist = lookup(wl->i_district, dk, part);
            if (!dist) { e_dytd++; continue; }
            double d_ytd = 0; int64_t next = 0;
            dist->get_value(D_YTD, d_ytd); dist->get_value(D_NEXT_O_ID, next);
            sum_d_ytd += d_ytd; w_amt += d_amt[dk];
            // D_YTD = initial 30000 + committed Payment amounts of this district
            if (std::fabs(d_ytd - (30000.0 + d_amt[dk])) > EPS) { e_dytd++; diff_d += std::fabs(d_ytd - (30000.0 + d_amt[dk])); }
            // D_NEXT_O_ID = 3001 + committed New-Orders of this district
            if ((uint64_t) next != 3001 + no_cnt[dk]) e_next++;
            // every order below D_NEXT_O_ID exists, with its NEW_ORDER row and exactly O_OL_CNT order lines
            uint64_t ol_sum_committed = 0;
            for (int64_t o = 1; o < next; o++) {
                row_t * ord = lookup(wl->i_order, orderKey(o, d, w), part);
                if (!ord) { e_order_missing++;
                    if (e_order_missing <= 20) printf("[consistency-detail] missing ORDER w=%lu d=%lu o_id=%ld (D_NEXT_O_ID=%ld) new_order=%d ol1=%d\n", w, d, o, next, lookup(wl->i_neworder, neworderKey(o, d, w), part) != NULL, lookup(wl->i_orderline, orderlineKey(1, o, d, w), part) != NULL);
                    continue; }
                orders_seen++;
                int64_t ol_cnt = 0, carrier = 0; ord->get_value(O_OL_CNT, ol_cnt); ord->get_value(O_CARRIER_ID, carrier);
                bool has_no = lookup(wl->i_neworder, neworderKey(o, d, w), part) != NULL;
                if (o >= 2101) { if (!has_no) e_no_missing++; if (carrier != 0) e_carrier++; }     // undelivered: must be in NEW_ORDER
                else           { if (has_no) e_no_extra++;   if (carrier == 0) e_carrier++; }     // delivered at load: must not be
                if (o >= 3001) ol_sum_committed += ol_cnt;
                for (int64_t n = 1; n <= ol_cnt; n++) if (!lookup(wl->i_orderline, orderlineKey(n, o, d, w), part)) e_ol_missing++;
                for (int64_t n = ol_cnt + 1; n <= 15; n++) if (lookup(wl->i_orderline, orderlineKey(n, o, d, w), part)) e_ol_extra++;
                if (ol_cnt < 1 || ol_cnt > 15) e_ol_cnt++;   // duplicate items are removed from an order, so counts below 5 are legitimate
                ol_seen += ol_cnt;
            }
            // orders committed in this run must account for exactly the order lines they inserted
            if (ol_sum_committed != no_ol[dk]) e_ol_sum++;
            // nothing may exist at or beyond D_NEXT_O_ID (leaked keys of aborted New-Orders)
            for (int64_t o = next; o < next + 2000; o++) {
                if (lookup(wl->i_order, orderKey(o, d, w), part)) { e_orphan++; if (e_orphan <= 30) printf("[consistency-detail] orphan ORDER w=%lu d=%lu o_id=%ld (next=%ld)\n", w, d, o, next); }
                if (lookup(wl->i_neworder, neworderKey(o, d, w), part)) { e_orphan++; if (e_orphan <= 30) printf("[consistency-detail] orphan NEW_ORDER w=%lu d=%lu o_id=%ld (next=%ld)\n", w, d, o, next); }
                for (int64_t n = 1; n <= 15; n++) if (lookup(wl->i_orderline, orderlineKey(n, o, d, w), part)) { e_orphan++; if (e_orphan <= 30) printf("[consistency-detail] orphan ORDER_LINE w=%lu d=%lu o_id=%ld ol=%ld (next=%ld)\n", w, d, o, n, next); }
            }
        }
        if (std::fabs(w_ytd - sum_d_ytd) > EPS) { e_wytd++; diff_w += std::fabs(w_ytd - sum_d_ytd); }
        if (std::fabs(w_ytd - (300000.0 + w_amt)) > EPS) e_wytd_amt++;
    }
    // customers: sums over all customers must match the committed Payments (by-name Payments included)
    double c_bal = 0, c_ytd = 0; uint64_t c_pcnt = 0, e_cust_missing = 0;
    for (uint64_t w = 1; w <= g_num_wh; w++) for (uint64_t d = 1; d <= DIST_PER_WARE; d++) for (uint64_t c = 1; c <= g_cust_per_dist; c++) {
        row_t * cu = lookup(wl->i_customer_id, custKey(c, d, w), wh_to_part(w));
        if (!cu) { e_cust_missing++; continue; }
        double b = 0, y = 0; uint64_t n = 0; cu->get_value(C_BALANCE, b); cu->get_value(C_YTD_PAYMENT, y); cu->get_value(C_PAYMENT_CNT, n);
        c_bal += b + 10.0; c_ytd += y - 10.0; c_pcnt += n - 1;
    }
    char buf[256];
    result("T6  W_YTD = sum(D_YTD) per warehouse", e_wytd, (snprintf(buf, sizeof buf, "diff=%.2f", diff_w), buf));
    result("    W_YTD = 300000 + committed Payment amounts", e_wytd_amt);
    result("    D_YTD = 30000 + committed Payment amounts per district", e_dytd, (snprintf(buf, sizeof buf, "diff=%.2f", diff_d), buf));
    result("    D_NEXT_O_ID = 3001 + committed New-Orders per district", e_next);
    result("T3/ ORDER row exists for every o_id < D_NEXT_O_ID", e_order_missing, (snprintf(buf, sizeof buf, "orders=%lu", orders_seen), buf));
    result("T1  undelivered ORDERs (o_id >= 2101) present in NEW_ORDER", e_no_missing);
    result("T2  delivered ORDERs (o_id < 2101) absent from NEW_ORDER", e_no_extra);
    result("    O_CARRIER_ID consistent with delivery state", e_carrier);
    result("T5  O_OL_CNT in 1..15 (duplicates removed at generation)", e_ol_cnt);
    result("T5  ORDER_LINE 1..O_OL_CNT present", e_ol_missing, (snprintf(buf, sizeof buf, "order lines=%lu", ol_seen), buf));
    result("T4  no ORDER_LINE beyond O_OL_CNT", e_ol_extra);
    result("    sum(O_OL_CNT) of committed orders = inserted order lines", e_ol_sum);
    result("T3/4 no ORDER/NEW_ORDER/ORDER_LINE at or beyond D_NEXT_O_ID", e_orphan);
    result("    CUSTOMER rows present", e_cust_missing);
    result("T8  sum(C_BALANCE + 10) = -committed Payment amount", std::fabs(c_bal + pm_amt) > EPS, (snprintf(buf, sizeof buf, "sum=%.2f expected=%.2f", c_bal, -pm_amt), buf));
    result("T7  sum(C_YTD_PAYMENT - 10) = committed Payment amount", std::fabs(c_ytd - pm_amt) > EPS, (snprintf(buf, sizeof buf, "sum=%.2f expected=%.2f", c_ytd, pm_amt), buf));
    result("    sum(C_PAYMENT_CNT - 1) = committed Payments", c_pcnt != pm_cnt, (snprintf(buf, sizeof buf, "sum=%lu expected=%lu", c_pcnt, pm_cnt), buf));
    printf("[consistency] %s (%d failing checks)\n", g_fail ? "FAILED" : "PASSED", g_fail);
}
#endif
