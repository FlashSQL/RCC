#include "rcc_srdet.h"
#if RCC_ERW_ON
#include "txn.h"
#include "manager.h"
#include <algorithm>
#include <cstdio>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>

RccSrDet rcc_srdet;

void RccSrDet::init(int threads) {
    nthreads = threads;
    logs = (Log *) _mm_malloc(sizeof(Log) * threads, 64);
    slots = (Slot *) _mm_malloc(sizeof(Slot) * threads, 64);
    fastc = (FastCnt *) _mm_malloc(sizeof(FastCnt) * threads, 64);
    sleeping = 0;
    for (int i = 0; i < threads; i++) {
        logs[i].head = 0; logs[i].tail = 0; logs[i].tail_seen = 0;
        logs[i].buf = (RsEvent *) _mm_malloc(sizeof(RsEvent) * LOG_SIZE, 64);
        slots[i].approved = (uint64_t) -1; slots[i].floor = 0; fastc[i].n = 0;
    }
    retained.assign(threads, std::set<uint64_t>());
    vis_att.assign(threads, (uint64_t) -1);
    vis_n.assign(threads, 0);
    done_id.assign(threads, 0);
    halt.store(false);
    visit_epoch = 0; changed = false;
    st_rounds = st_events = st_requests = st_approvals = st_cycles = st_ww_cycles = st_victims = 0;
    st_reader_victims = st_stuck = st_max_nodes = st_gc = st_dfs = st_dfs_ns = st_sleeps = 0;
    st_edges[0] = st_edges[1] = st_edges[2] = 0;
    rb_full = 0;
}

// The smallest attempt id of thread t that may still be in the graph: ids below it are gone for good. Attempts of one
// thread run one after another, so every id below done_id[t] has ended; the ended ones still retained keep the floor.
void RccSrDet::set_floor(int t) {
    uint64_t f = done_id[t];
    if (!retained[t].empty() && *retained[t].begin() < f) f = *retained[t].begin();
    if (slots[t].floor != f) __atomic_store_n(&slots[t].floor, f, __ATOMIC_RELEASE);
}

RccSrDet::Node * RccSrDet::get(uint64_t id) {
    auto it = nodes.find(id);
    if (it != nodes.end()) return it->second;
    const int t = (int) (id % nthreads);
    if (id < done_id[t]) return NULL;   // that attempt ended and has been collected
    Node * n = new Node();
    n->id = id; n->state = ST_ACTIVE; n->requested = n->approved = false;
    n->color = 0; n->visit = 0; n->pos = -1;
    nodes[id] = n;
    retained[t].insert(id);
    if (nodes.size() > st_max_nodes) st_max_nodes = nodes.size();
    return n;
}

static inline void erase_arc(std::vector<RccSrDet::Arc> & v, const void * n) {
    for (size_t i = 0; i < v.size(); ) {
        if ((const void *) v[i].n == n) { v[i] = v.back(); v.pop_back(); } else ++i;
    }
}

void RccSrDet::remove(Node * first) {
    stackv.clear();
    stackv.push_back(first);
    while (!stackv.empty()) {
        Node * n = stackv.back(); stackv.pop_back();
        if (nodes.find(n->id) == nodes.end()) continue;   // already removed through another path
        for (Arc & a : n->preds) erase_arc(a.n->succs, n);
        for (Arc & a : n->succs) {
            Node * s = a.n;
            erase_arc(s->preds, n);
            if (s->state == ST_COMMITTED && s->preds.empty()) stackv.push_back(s);   // GC: committed, no predecessor
        }
        const int t = (int) (n->id % nthreads);
        nodes.erase(n->id);
        retained[t].erase(n->id);
        set_floor(t);
        delete n;
        ++st_gc;
    }
}

void RccSrDet::maybe_collect(Node * n) {
    if (n->state == ST_COMMITTED && n->preds.empty()) remove(n);
}

void RccSrDet::add_edge(uint64_t pred, uint64_t succ, uint32_t kind) {
    Node * s = get(succ);
    Node * p = get(pred);
    if (!s || !p || s == p) return;
    if (s->state == ST_ABORTING || p->state == ST_ABORTING) return;
    for (const Arc & a : s->preds) if (a.n == p && a.kind == kind) return;   // duplicate
    s->preds.push_back({p, kind});
    p->succs.push_back({s, kind});
    ++st_edges[kind < 3 ? kind : 0];
    changed = true;
}

// Read the records thread t's current attempt appended since the last visit. A record is kept only if the attempt id
// is the same before and after reading it (a new attempt resets the count, then the id, then overwrites the records).
bool RccSrDet::visit(int t) {
    txn_man * tx = glob_manager->get_txn_man(t);   // not cached: each phase (warmup, measured run) gets a new txn_man
    if (!tx) return false;
    const uint64_t a = __atomic_load_n(&tx->rs_rec_att, __ATOMIC_ACQUIRE);
    if (a != vis_att[t]) { vis_att[t] = a; vis_n[t] = 0; }
    const uint32_t n = __atomic_load_n(&tx->rs_rec_n, __ATOMIC_ACQUIRE);
    if (n <= vis_n[t] || a == (uint64_t) -1) return false;
    static thread_local std::vector<txn_man::RsRec> buf;
    buf.assign(tx->rs_rec + vis_n[t], tx->rs_rec + n);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&tx->rs_rec_att, __ATOMIC_ACQUIRE) != a) return false;   // the attempt ended meanwhile
    vis_n[t] = n;
    for (const auto & r : buf) add_edge(r.pred, a, r.kind);
    return true;
}

void RccSrDet::ingest(const RsEvent & e) {
    ++st_events;
    switch (e.type) {
    case RSE_EDGE:   // only when the transaction object's record was full
        add_edge(e.a, e.b, e.kind);
        return;
    case RSE_REQUEST: {
        visit((int) (e.a % nthreads));   // every record of this attempt was appended before its request
        Node * n = get(e.a);
        if (!n) return;
        n->requested = true;
        pending.push_back(e.a);
        ++st_requests;
        changed = true;
        return;
    }
    case RSE_COMMIT: {
        const int t = (int) (e.a % nthreads);
        Node * n = get(e.a);
        if (e.a + 1 > done_id[t]) done_id[t] = e.a + 1;
        if (n) { n->state = ST_COMMITTED; maybe_collect(n); }
        set_floor(t);
        return;
    }
    case RSE_ABORT: {
        const int t = (int) (e.a % nthreads);
        auto it = nodes.find(e.a);
        if (e.a + 1 > done_id[t]) done_id[t] = e.a + 1;
        if (it != nodes.end()) remove(it->second);
        set_floor(t);
        return;
    }
    }
}

bool RccSrDet::drain() {
    bool any = false;
    for (int t = 0; t < nthreads; t++) {
        any |= visit(t);   // the records appended since the last round: cycles are looked for before anyone asks
        Log & l = logs[t];
        const uint64_t head = __atomic_load_n(&l.head, __ATOMIC_ACQUIRE);
        uint64_t tail = l.tail;
        if (tail == head) continue;
        any = true;
        const uint64_t end = std::min(head, tail + 4096);   // bounded per log and round
        for (; tail < end; ++tail) ingest(l.buf[tail & (LOG_SIZE - 1)]);
        __atomic_store_n(&l.tail, tail, __ATOMIC_RELEASE);
    }
    return any;
}

// One DFS over the graph. On a cycle, abort its victim(s) and return true (the caller looks again).
bool RccSrDet::break_one_cycle() {
    ++visit_epoch;
    for (auto & kv : nodes) {
        Node * root = kv.second;
        if (root->state == ST_ABORTING || root->visit == visit_epoch) continue;
        frames.clear();
        root->visit = visit_epoch; root->color = 1; root->pos = 0;
        frames.push_back({root, 0, RS_WR});
        while (!frames.empty()) {
            const size_t top = frames.size() - 1;
            Node * u = frames[top].n;
            if (frames[top].next < u->succs.size()) {
                const Arc a = u->succs[frames[top].next++];
                Node * v = a.n;
                if (v->state == ST_ABORTING) continue;
                if (v->visit != visit_epoch) {
                    v->visit = visit_epoch; v->color = 1; v->pos = (int) frames.size();
                    frames.push_back({v, 0, a.kind});
                } else if (v->color == 1) {
                    // back edge u -> v: the cycle is frames[v->pos .. top] and the edge top -> v
                    ++st_cycles;
                    const size_t start = (size_t) v->pos;
                    bool has_rw = a.kind == RS_RW;
                    for (size_t i = start + 1; i <= top; i++) has_rw |= frames[i].in_kind == RS_RW;
                    std::vector<Node *> victims;
                    if (!has_rw) {
                        ++st_ww_cycles;   // wr / ww only: every member waits for the next one's commit
                        for (size_t i = start; i <= top; i++) {
                            Node * m = frames[i].n;
                            if (m->state == ST_ACTIVE && !m->approved) victims.push_back(m);
                        }
                    } else {
                        // a reader of the cycle: the source of one of its rw edges (hop i -> i+1, closing hop top -> start)
                        Node * best = NULL;
                        for (size_t i = start; i <= top; i++) {
                            const uint32_t k = (i == top) ? a.kind : frames[i + 1].in_kind;
                            Node * m = frames[i].n;
                            if (k == RS_RW && m->state == ST_ACTIVE && !m->approved && (!best || m->id > best->id)) best = m;
                        }
                        if (best) ++st_reader_victims;
                        else {
                            for (size_t i = start; i <= top; i++) {
                                Node * m = frames[i].n;
                                if (m->state == ST_ACTIVE && !m->approved && (!best || m->id > best->id)) best = m;
                            }
                        }
                        if (best) victims.push_back(best);
                    }
                    if (victims.empty()) {
                        ++st_stuck;
                        fprintf(stderr, "[rcc_srdet] invariant: a cycle of %zu members has no unapproved member\n", top - start + 1);
                        abort();
                    }
                    for (Node * m : victims) {
                        m->state = ST_ABORTING;
                        ++st_victims;
                        txn_man * tx = glob_manager->get_txn_man(m->id % nthreads);
                        bool won = false;
                        if (tx) tx->set_abort(ABORT_SRC_WOUND, &won, m->id);
                    }
                    changed = true;
                    return true;
                }
            } else {
                u->color = 2;
                frames.pop_back();
            }
        }
    }
    return false;
}

void RccSrDet::wake() { syscall(SYS_futex, &sleeping, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0); }

// [wake] is there anything drain() would read: a new record in a TLA, or an event in a log? Read only.
bool RccSrDet::pending_work() {
    for (int t = 0; t < nthreads; t++) {
        if (__atomic_load_n(&logs[t].head, __ATOMIC_SEQ_CST) != logs[t].tail) return true;
        txn_man * tx = glob_manager->get_txn_man(t);
        if (!tx) continue;
        const uint64_t a = __atomic_load_n(&tx->rs_rec_att, __ATOMIC_SEQ_CST);
        const uint32_t n = __atomic_load_n(&tx->rs_rec_n, __ATOMIC_SEQ_CST);
        if (a != (uint64_t) -1 && n > (a == vis_att[t] ? vis_n[t] : 0)) return true;
    }
    return false;
}

// [wake] announce sleeping, look for work once more (a worker whose record or event came before it could see
// sleeping == 1 does not wake us), then wait on the futex until a worker clears sleeping, or WAKE_MAX_SLEEP_NS.
void RccSrDet::sleep_wait() {
    __atomic_store_n(&sleeping, 1U, __ATOMIC_SEQ_CST);
    if (!halt.load(std::memory_order_acquire) && !pending_work()) {
        struct timespec ts = {(time_t) (WAKE_MAX_SLEEP_NS / 1000000000UL), (long) (WAKE_MAX_SLEEP_NS % 1000000000UL)};
        syscall(SYS_futex, &sleeping, FUTEX_WAIT_PRIVATE, 1, &ts, NULL, 0);
        ++st_sleeps;
    }
    __atomic_store_n(&sleeping, 0U, __ATOMIC_SEQ_CST);
}

void RccSrDet::loop() {
    uint64_t idle_since = 0;
    for (;;) {
        const bool any = drain();
        if (changed) {
            changed = false;
            const uint64_t t0 = get_sys_clock();
            for (int guard = 0; break_one_cycle() && guard < 1000000; guard++) {}
            ++st_dfs; st_dfs_ns += get_sys_clock() - t0;
            // the graph is acyclic now: every transaction that asked is approved
            for (uint64_t id : pending) {
                auto it = nodes.find(id);
                if (it == nodes.end()) continue;
                Node * n = it->second;
                if (n->state != ST_ACTIVE || !n->requested || n->approved) continue;
                n->approved = true;
                ++st_approvals;
                __atomic_store_n(&slots[id % nthreads].approved, id, __ATOMIC_RELEASE);
            }
            pending.clear();
            changed = false;
        }
        ++st_rounds;
        if (!any) {
            if (halt.load(std::memory_order_acquire)) break;
            const uint64_t now = get_sys_clock();
            if (!idle_since) idle_since = now;
            if (now - idle_since >= WAKE_SPIN_NS) { sleep_wait(); idle_since = 0; continue; }
            for (int i = 0; i < 16; i++) PAUSE
        }
        else idle_since = 0;
    }
}

void RccSrDet::print() {
    uint64_t fast = 0;
    for (int t = 0; t < nthreads; t++) fast += fastc[t].n;
    printf("[rcc_srdet] rounds=%lu events=%lu edges_wr=%lu edges_ww=%lu edges_rw=%lu requests=%lu approvals=%lu "
           "fast_approvals=%lu cycles=%lu ww_cycles=%lu victims=%lu reader_victims=%lu stuck=%lu max_nodes=%lu gc=%lu "
           "dfs=%lu dfs_avg_us=%.2f live_nodes=%zu rb_full=%lu sleeps=%lu\n",
           st_rounds, st_events, st_edges[RS_WR], st_edges[RS_WW], st_edges[RS_RW], st_requests, st_approvals, fast,
           st_cycles, st_ww_cycles, st_victims, st_reader_victims, st_stuck, st_max_nodes, st_gc, st_dfs,
           st_dfs ? st_dfs_ns / 1e3 / st_dfs : 0.0, nodes.size(), (uint64_t) rb_full, st_sleeps);
}
#endif
