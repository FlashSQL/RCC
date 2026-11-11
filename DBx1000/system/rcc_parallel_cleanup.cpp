#include "rcc_parallel_cleanup.h"
#if CC_ALG == RCC && RCC_PARALLEL_CLEANUP
#include "txn.h"
#include "row_rcc.h"
#include <atomic>
#include <cstdlib>

#if COMMUTATIVE_OPS
#error "RCC parallel cleanup currently requires COMMUTATIVE_OPS=false"
#endif
namespace {
// A generation-tagged task count replaces the former per-row spinlock. A
// successful claim owns one completion credit, keeping owner/accesses alive.
// A helper stalled before CAS cannot claim a reused attempt with an old tag.
constexpr uint64_t COUNT_MASK = 0xffff;
constexpr uint64_t GENERATION_STEP = COUNT_MASK + 1;
static_assert(MAX_ROW_PER_TXN <= COUNT_MASK, "cleanup task count must fit");
struct alignas(64) CleanupSlot {
    std::atomic<uint64_t> work{0};
    txn_man *owner = nullptr;
    alignas(64) std::atomic<unsigned> remaining{0};
};
struct alignas(64) ActiveWord { std::atomic<uint64_t> bits{0}; };
CleanupSlot slots[THREAD_CNT];
ActiveWord active[(THREAD_CNT + 63) / 64];
#if RCC_PARALLEL_CLEANUP_TEST
void hook(txn_man *owner, txn_man *executor, Access *a, int phase);
#endif
// Called without a row latch. Fail once and return; never spin as a helper.
bool take(unsigned slot_id, txn_man *executor) {
    CleanupSlot &s = slots[slot_id];
    uint64_t observed = s.work.load(std::memory_order_acquire);
    unsigned count = observed & COUNT_MASK;
    if (!count) return false;
#if RCC_PARALLEL_CLEANUP_TEST
    // No owner or Access may be read yet: a concurrent finish can reuse them.
    hook(nullptr, executor, nullptr, 3);
#endif
    if (!s.work.compare_exchange_strong(observed, observed - 1,
            std::memory_order_acq_rel, std::memory_order_relaxed)) return false;
    // The successful RMW acquires the publication and reserves a completion
    // credit. Owner cannot reset this slot or its buffers until we finish.
    txn_man *owner = s.owner;
    Access *a = owner->accesses[count - 1];
    if (count == 1)
        active[slot_id / 64].bits.fetch_and(~(uint64_t(1) << (slot_id % 64)),
                                           std::memory_order_relaxed);
#if RCC_PARALLEL_CLEANUP_TEST
    hook(owner, executor, a, 1);
#endif
    row_t *row = a->orig_row;
    if (row) {
        row->manager->lock_release(a->lock_entry, RCOK, executor);
        a->orig_row = nullptr;
        INC_STATS(executor->get_thd_id(), rcc_pc_rows, 1);
        if (executor != owner) { INC_STATS(executor->get_thd_id(), rcc_pc_help_rows, 1); }
    }
#if RCC_PARALLEL_CLEANUP_TEST
    hook(owner, executor, a, 2);
#endif
    // Last access to owner/access/buffers. Release sequence collects every
    // completion into the owner's acquire load before it reuses the attempt.
    s.remaining.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}
}
#if RCC_PARALLEL_CLEANUP_TEST
extern void rcc_test_cleanup_hook(txn_man *, txn_man *, Access *, int);
namespace { void hook(txn_man *o, txn_man *e, Access *a, int phase) {
    rcc_test_cleanup_hook(o, e, a, phase);
} }
#endif

bool rcc_cleanup_help(txn_man *executor) {
    // Spread the first probes across slots. Losing a claim backs off locally:
    // no atomic write or shared-bitmap read is needed during the skipped calls.
    static thread_local unsigned cursor = executor->get_thd_id();
    static thread_local unsigned skip = 0, failures = 0;
    if (skip) { --skip; return false; }
    constexpr unsigned words = (THREAD_CNT + 63) / 64;
    for (unsigned n = 0; n < words; ++n) {
        unsigned w = ((cursor / 64) + n) % words;
        uint64_t mask = active[w].bits.load(std::memory_order_relaxed);
        if (!mask) continue;
        unsigned shift = w == cursor / 64 ? cursor % 64 : 0;
        uint64_t later = mask & (~uint64_t(0) << shift);
        unsigned bit = __builtin_ctzll(later ? later : mask);
        unsigned id = w * 64 + bit;
        cursor = (id + 1) % THREAD_CNT;
        uint64_t start = get_sys_clock();
        bool worked = take(id, executor);
        uint64_t duration = get_sys_clock() - start;
        if (worked) {
            failures = 0;
            INC_STATS(executor->get_thd_id(), rcc_pc_help_time, duration);
        } else {
            if (failures < 4) ++failures;
            skip = (1u << failures) - 1 + (executor->get_thd_id() & 3);
            INC_STATS(executor->get_thd_id(), rcc_pc_failed_help, 1);
            INC_STATS(executor->get_thd_id(), rcc_pc_failed_help_time, duration);
        }
        return worked;
    }
    return false;
}

void rcc_cleanup_rows(txn_man *owner) {
    if ((owner->commit_barriers & 3UL) != COMMITED ||
        owner->get_thd_id() >= THREAD_CNT) std::abort();
    unsigned id = owner->get_thd_id();
    CleanupSlot &s = slots[id];
    uint64_t previous = s.work.load(std::memory_order_relaxed);
    if ((previous & COUNT_MASK) || s.remaining.load(std::memory_order_acquire))
        std::abort();
    uint64_t generation = (previous & ~COUNT_MASK) + GENERATION_STEP;
    if (!generation) std::abort(); // never wrap a tag and permit an ABA claim
    s.owner = owner;
    unsigned count = owner->row_cnt;
    s.remaining.store(count, std::memory_order_relaxed);
    // Advertise BEFORE work publication: a stale scanner may still see count=0,
    // but no last claimant can clear the hint before the owner sets it.
    if (count) active[id / 64].bits.fetch_or(uint64_t(1) << (id % 64),
                                           std::memory_order_relaxed);
    s.work.store(generation | count, std::memory_order_release);
    INC_STATS(id, rcc_pc_batches, 1);
#if RCC_PARALLEL_CLEANUP_TEST
    hook(owner, owner, nullptr, 0);
#endif
    // All transaction sizes participate. Owner makes progress even if every
    // other worker is busy, backing off, or has stopped at benchmark shutdown.
    while (s.work.load(std::memory_order_relaxed) & COUNT_MASK) {
        if (!take(id, owner)) { PAUSE }
    }
    if (s.remaining.load(std::memory_order_acquire)) {
        uint64_t start = get_sys_clock();
        while (s.remaining.load(std::memory_order_acquire)) { PAUSE }
        INC_STATS(id, rcc_pc_join_time, get_sys_clock() - start);
    }
}
#endif
