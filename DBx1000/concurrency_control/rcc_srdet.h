#ifndef RCC_SRDET_H
#define RCC_SRDET_H
// ERW: the Serialization Detector (SR Det).
// Every access records its conflict dependencies (wr / ww / rw edges, predecessor -> the accessing transaction) in
// the transaction object (its TLA, txn_man::rs_rec). One background thread visits the transaction objects and collects
// the new records into a private graph, looks for cycles and approves transactions. Requests, commits and aborts
// (once per attempt) come through a per-worker single-producer event log. A transaction commits once it is approved AND its write predecessors (wr/ww)
// have committed; it never waits for a read predecessor (rw). Committed nodes stay in the graph while they still have
// a predecessor, so a later rw edge can still close a cycle through them, and are collected afterwards:
//   GC: aborted, or committed with no predecessor left.
// Victims: a cycle made only of wr/ww edges aborts every unapproved member; otherwise one unapproved reader of the
// cycle (the source of one of its rw edges), else one unapproved member.
// Approval: when the whole graph known to the detector is acyclic, every transaction that asked is approved. A
// transaction asks right before commit, after it logged all its edges; all edges INTO a transaction are logged by
// that transaction, so a cycle that closes later always contains a member that has not asked yet.
#include "global.h"

#if CC_ALG == RCC && RCC_ERW
#define RCC_ERW_ON 1
#else
#define RCC_ERW_ON 0
#endif
// RCC_BATCH = ERW + dep push + asynchronous batch release (config-std.h)
#if RCC_ERW_ON && defined(RCC_BATCH)
#define RCCB 1
#else
#define RCCB 0
#endif
#if CC_ALG == RCC && defined(RCC_BATCH) && !RCC_ERW
#error "RCC_BATCH builds on ERW: set RCC_ERW true"
#endif
#if RCCB && (COMMUTATIVE_OPS || RCC_PARALLEL_CLEANUP)
#error "RCC_BATCH releases committed rows itself: COMMUTATIVE_OPS and RCC_PARALLEL_CLEANUP are not supported"
#endif

#if RCC_ERW_ON
#include <vector>
#include <set>
#include <unordered_map>
#include <atomic>

class txn_man;
enum RsKind { RS_WR = 0, RS_WW = 1, RS_RW = 2 };
enum RsType { RSE_EDGE = 0, RSE_REQUEST = 1, RSE_COMMIT = 2, RSE_ABORT = 3 };
struct RsEvent { uint64_t a; uint64_t b; uint32_t type; uint32_t kind; };   // EDGE: a = pred, b = succ; else a = attempt

class RccSrDet {
  public:
    void init(int threads);
    void loop();                   // the detector thread
    void request_stop() {
        halt.store(true, std::memory_order_release);
        __atomic_store_n(&sleeping, 0U, __ATOMIC_SEQ_CST); wake();
    }
    void print();

    // ---- worker side (own log only) ----
    inline void log(uint64_t thd, uint32_t type, uint64_t a, uint64_t b = 0, uint32_t kind = 0) {
        Log & l = logs[thd];
        const uint64_t h = l.head;
        if (h - l.tail_seen >= LOG_SIZE) {   // the detector's tail is read only when the ring looks full
            l.tail_seen = __atomic_load_n(&l.tail, __ATOMIC_ACQUIRE);
            while (h - l.tail_seen >= LOG_SIZE) { PAUSE; l.tail_seen = __atomic_load_n(&l.tail, __ATOMIC_ACQUIRE); }
        }
        RsEvent & e = l.buf[h & (LOG_SIZE - 1)];
        e.a = a; e.b = b; e.type = type; e.kind = kind;
        __atomic_store_n(&l.head, h + 1, __ATOMIC_RELEASE);
        // only an edge (log full) or a commit request wakes the detector; COMMIT / ABORT wait for its next round (it
        // drains every log before it looks for cycles) or WAKE_MAX_SLEEP_NS: waking it after every commit costs
        // a futex call per transaction when there is nothing to check (1 thread TPC-C: -4~6%)
        if (type == RSE_EDGE || type == RSE_REQUEST) notify();
    }
    // [wake] after an edge record or an event: wake the detector if it sleeps. The fence orders the record / event before
    // the sleeping check (Dekker with sleep_wait, which announces sleeping and then looks for new work once more).
    // While the detector is awake this is a fence and a read of a line that only the detector writes.
    inline void notify() {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&sleeping, __ATOMIC_RELAXED) && __atomic_exchange_n(&sleeping, 0U, __ATOMIC_SEQ_CST)) wake();
    }
    void wake();
    inline bool approved(uint64_t thd, uint64_t id) { return __atomic_load_n(&slots[thd].approved, __ATOMIC_ACQUIRE) == id; }
    // attempts of that attempt's thread below the floor have left the graph: an edge from them is useless
    inline bool gone(uint64_t id) { return id < __atomic_load_n(&slots[id % nthreads].floor, __ATOMIC_ACQUIRE); }
    inline void note_fast(uint64_t thd) { ++fastc[thd].n; }   // worker-written: not in the detector-written slot
    // [diagnostic] this thread's event log and approval slot as the worker sees them
    void dbg_state(uint64_t thd, uint64_t out[5]) {
        out[0] = logs[thd].head; out[1] = __atomic_load_n(&logs[thd].tail, __ATOMIC_ACQUIRE); out[2] = logs[thd].tail_seen;
        out[3] = __atomic_load_n(&slots[thd].approved, __ATOMIC_ACQUIRE); out[4] = __atomic_load_n(&slots[thd].floor, __ATOMIC_ACQUIRE);
    }
    volatile uint64_t rb_full;     // [RCC_BATCH] registrations refused because a writer's dependent list was full

    // graph types (detector thread only)
    enum { ST_ACTIVE = 0, ST_COMMITTED, ST_ABORTING };
    struct Node;
    struct Arc { Node * n; uint32_t kind; };
    struct Node {
        uint64_t id; uint8_t state; bool requested, approved;
        uint32_t color; uint64_t visit; int pos;
        std::vector<Arc> preds, succs;
    };
    struct Frame { Node * n; size_t next; uint32_t in_kind; };

  private:
    static const uint64_t LOG_SIZE = 1UL << 16;
    // [wake] "a worker records an edge in its TLA, then wakes the serialization detector". While there is work the
    // detector reads every worker's TLA and event log in each round (read only). After WAKE_SPIN_NS without new records
    // or events it sleeps on a futex; the next worker that records an edge or asks for approval wakes it, and it wakes by
    // itself after WAKE_MAX_SLEEP_NS at the latest (the MySQL implementation uses 10 ms).
    static const uint64_t WAKE_SPIN_NS = 20000;
    static const uint64_t WAKE_MAX_SLEEP_NS = 1000000;
    // worker line: head, buf, tail_seen (the worker's last read of tail); detector line: tail
    struct alignas(64) Log { volatile uint64_t head; RsEvent * buf; uint64_t tail_seen; char pad[40]; volatile uint64_t tail; char pad2[56]; };
    struct alignas(64) Slot { volatile uint64_t approved; volatile uint64_t floor; char pad[48]; };
    struct alignas(64) FastCnt { uint64_t n; char pad[56]; };

    // read by every worker on every log / approved / gone: a cache line of their own, never written after init
    int nthreads;
    Log * logs;
    Slot * slots;
    FastCnt * fastc;
    alignas(64) volatile uint32_t sleeping;   // [wake] 1 while the detector waits on the futex; read by workers
    char sleeping_pad[60];
    // detector-private from here (the graph's hash map header changes on every node insert / erase)
    alignas(64) std::unordered_map<uint64_t, Node *> nodes;
    std::vector<std::set<uint64_t>> retained;   // per thread: ids of its attempts still in the graph
    std::vector<uint64_t> done_id;              // per thread: last attempt that ended (+1; 0 = none yet)
    std::vector<uint64_t> pending;              // attempts that asked for approval (by id: a node may be removed first)
    std::vector<Node *> stackv;
    std::vector<Frame> frames;
    std::atomic<bool> halt;
    uint64_t visit_epoch;
    bool changed;
    // stats
    uint64_t st_rounds, st_events, st_edges[3], st_requests, st_approvals, st_cycles, st_ww_cycles, st_victims,
             st_reader_victims, st_stuck, st_max_nodes, st_gc, st_dfs, st_dfs_ns, st_sleeps;

    std::vector<uint64_t> vis_att;              // per thread: the attempt whose records were read, and how many
    std::vector<uint32_t> vis_n;
    bool visit(int t);             // read the new dependency records in thread t's transaction object
    void add_edge(uint64_t pred, uint64_t succ, uint32_t kind);
    Node * get(uint64_t id);       // find, or create unless that attempt already left the graph
    void remove(Node * n);
    void maybe_collect(Node * n);
    void ingest(const RsEvent & e);
    bool drain();
    bool break_one_cycle();
    void set_floor(int t);
    bool pending_work();
    void sleep_wait();
};

extern RccSrDet rcc_srdet;
#endif
#endif
