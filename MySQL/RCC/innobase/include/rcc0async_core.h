#ifndef RCC0ASYNC_CORE_H
#define RCC0ASYNC_CORE_H

#include "rcc0as_graph.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
enum class Decision { accepted, waiting, aborted };
enum class AbortCause { none, external, early_stamp, cert_stamp, value_cycle,
  mixed_cycle, failed_read_head, unavailable_read, failed_write_head,
  missing_write_access, reupdate_head, reupdate_consumed, bad_state,
  failed_input, cascade, timeout, engine_cascade, native_deadlock, engine_reupdate, serialization_cycle, count };
struct AbortTag { AbortCause root = AbortCause::none; uint64_t id = 0; };
struct Transaction;
using TransactionPtr = std::shared_ptr<Transaction>;
using Bytes = std::vector<unsigned char>;
using Image = std::shared_ptr<const Bytes>;

struct Version {
  uint64_t trace_id = 0;
  AbortTag abort_tag;
  std::weak_ptr<Transaction> owner;
  State terminal = State::committed;
  std::weak_ptr<History> writer_history;
  std::vector<std::weak_ptr<History>> reader_history;
  std::shared_ptr<Version> previous;
  std::weak_ptr<Version> next;
  std::vector<std::weak_ptr<Transaction>> readers;
  Image image;
  const unsigned char *external = nullptr;
  size_t external_size = 0;
  size_t extra = 0;
};
using VersionPtr = std::shared_ptr<Version>;
struct Access {
  VersionPtr source, written, base;
  Image snapshot;
  size_t extra = 0;
  const unsigned char *external = nullptr;
  size_t size = 0;
};
struct Transaction {
  AbortCause abort_cause = AbortCause::none;
  AbortTag abort_tag;
  bool rw_bypassed_at_certification = false;
  uint64_t id = 0;
  HistoryPtr history;
  bool certification_requested = false;
  std::atomic<State> state{State::active};
  std::atomic<size_t> pending_inputs{0};
  std::mutex event_mutex;
  WaitKey wait_key{};
  std::unordered_map<std::string, Access> accesses;
  std::vector<VersionPtr> inputs;
  using Dependent = std::weak_ptr<Transaction>;
  std::vector<Dependent> dependents;
  std::mutex dependents_mutex;
  std::condition_variable changed;
};
struct Snapshot {
  Image bytes;
  size_t extra = 0;
  const unsigned char *external = nullptr;
  const unsigned char *data() const { return external ? external : bytes->data(); }
  explicit operator bool() const { return external || static_cast<bool>(bytes); }
};

enum class ProfileOp { begin, read, write, certify, detect, finish, native, other };
using TraceSink = void (*)(char, const std::array<uint64_t, 8> &);
class Registry {
  static constexpr size_t shard_count = 16;
  struct RowShard {
    std::mutex mutex;
    std::unordered_map<std::string, VersionPtr> rows;
  };
  struct Rows {
    std::array<RowShard, shard_count> shards;
    static size_t index(const std::string &key) {
      return std::hash<std::string>{}(key) % shard_count;
    }
    VersionPtr &operator[](const std::string &key) { return shards[index(key)].rows[key]; }
    VersionPtr &at(const std::string &key) { return shards[index(key)].rows.at(key); }
    size_t size() const {
      size_t n = 0; for (const auto &s : shards) n += s.rows.size(); return n;
    }
  };
  static uint64_t ticks() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  class Guard {
   public:
    Guard(Registry &r, ProfileOp op, bool barrier = true)
        : r(r), op(size_t(op)), lock(r.m_mutex, std::defer_lock) {
      const auto before = r.m_profile ? ticks() : 0;
      cost_before = rcc_duration::detail_write ? rcc_duration::now() : 0;
      lock.lock();
      if(cost_before) cost_acquired=rcc_duration::now();
      if (barrier) freeze_rows();
      if (r.m_profile) {
        acquired = ticks(); ++r.m_profile_data[this->op * 3];
        r.m_profile_data[this->op * 3 + 1] += acquired - before;
      }
    }
    void freeze_rows() {
      for (size_t i = 0; i < shard_count; ++i)
        shards[i] = std::unique_lock<std::mutex>(r.m_rows.shards[i].mutex);
    }
    ~Guard() {
      if (r.m_profile) r.m_profile_data[op * 3 + 2] += ticks() - acquired;
      if(cost_before) {
        const auto end=rcc_duration::now();
        rcc_duration::add(rcc_duration::wh_write_registry_wait,cost_acquired-cost_before);
        rcc_duration::add(rcc_duration::wh_write_registry_hold,end-cost_acquired);
      }
    }
   private:
    uint64_t cost_before=0,cost_acquired=0;
    Registry &r; size_t op;
    std::unique_lock<std::mutex> lock;
    std::array<std::unique_lock<std::mutex>, shard_count> shards;
    uint64_t acquired = 0;
  };
  class RowGuard {
   public:
    RowGuard(Registry &r, const std::string &key, ProfileOp op)
        : r(r), op(size_t(op)), lock(r.m_rows.shards[Rows::index(key)].mutex, std::defer_lock) {
      const auto before = r.m_profile ? ticks() : 0;
      cost_before=rcc_duration::detail_write ? rcc_duration::now() : 0;
      lock.lock();
      if(cost_before) cost_acquired=rcc_duration::now();
      if (r.m_profile) {
        acquired = ticks(); ++r.m_profile_data[this->op * 3];
        r.m_profile_data[this->op * 3 + 1] += acquired - before;
      }
    }
    ~RowGuard() {
      if (r.m_profile) r.m_profile_data[op * 3 + 2] += ticks() - acquired;
      if(cost_before) {
        const auto end=rcc_duration::now(); lock.unlock();
        rcc_duration::add(rcc_duration::wh_write_row_wait,cost_acquired-cost_before);
        rcc_duration::add(rcc_duration::wh_write_row_hold,end-cost_acquired);
      }
    }
   private:
    uint64_t cost_before=0,cost_acquired=0;
    Registry &r; size_t op; std::unique_lock<std::mutex> lock; uint64_t acquired = 0;
  };
 public:
  explicit Registry(void (*notify)() = nullptr, bool profile = false,
                    bool abort_profile = false, TraceSink trace = nullptr)
      : m_profile(profile), m_abort_profile(abort_profile),
        m_notify(notify), m_trace(trace) {}

  bool per_trx_graph() const { return m_graph.per_trx(); }
  ExactGraph::Stats graph_metrics() { return m_graph.metrics(); }
  WaitObservation observe_waits() { return m_graph.observe(); }
  void profile_phase(WaitKey key, unsigned phase) { m_graph.profile_phase(key, phase); }

  uint64_t profile_value(size_t index) {
    if (index == 40) return m_abort_profile;
    if (index >= 41 && index <= 50)
      return m_rw_profile[index - 41].load(std::memory_order_relaxed);
    if (index >= 64 && index < 64 + m_abort_counts.size())
      return m_abort_counts[index - 64].load(std::memory_order_relaxed);
    if (index < m_profile_data.size()) return m_profile_data[index].load();
    Guard guard(*this, ProfileOp::other, index == 24);
    switch (index) {
      case 24: return m_rows.size();
      case 25: return m_active.size();
      case 26: case 27: case 28: case 29: case 30: return 0;
      default: return 0;
    }
  }

  void detect_async() {
    ExactGraph::Check snapshot;
    if (!m_graph.inspect(snapshot)) return;
    const auto &cycle = snapshot.cycle;
    if (!cycle.empty()) {
      const auto key = m_graph.reject(cycle);
      if (key != WaitKey{}) {
        Guard guard(*this, ProfileOp::detect, false);
        const auto it = m_bound.find(key);
        if (it != m_bound.end()) {
          if (auto t = it->second.lock())
            abort_locked(t, AbortCause::serialization_cycle);
        }
      }
    } else {
      const auto serialization_approved = m_graph.approve(snapshot);
      std::vector<TransactionPtr> wake;
      {
        Guard guard(*this, ProfileOp::detect, false);
        for (auto key : serialization_approved) {
          const auto it = m_bound.find(key);
          if (it != m_bound.end()) if (auto t = it->second.lock()) wake.push_back(t);
        }
      }
      for (const auto &t : wake) signal(t);
    }
    if (m_graph.needs_check()) request_graph_check();
  }

  TransactionPtr begin(WaitKey engine = {}) {
    Guard guard(*this, ProfileOp::begin, false);
    auto t = std::make_shared<Transaction>();
    t->id = ++m_identity;
    if (m_trace) m_trace('B', {t->id});
    t->wait_key = engine.first ? engine : WaitKey{0, t->id};
    m_bound[t->wait_key] = t;
    m_active.emplace(t->id, t);
    t->history = m_graph.begin(t->wait_key);
    return t;
  }

  using Copy = unsigned char *(*)(void *, const unsigned char *, size_t, size_t);
  Snapshot read(const TransactionPtr &t, const std::string &key,
                const unsigned char *physical, size_t size, size_t extra,
                Copy copy = nullptr, void *context = nullptr) {
    AbortCause cause = AbortCause::bad_state;
    AbortTag parent;
    const auto result = [&]() -> Snapshot {
      RowGuard guard(*this, key, ProfileOp::read);
      if (t->state != State::active) return {};
      auto old = t->accesses.find(key);
      if (old != t->accesses.end()) {
        const auto &a = old->second;
        const auto *data = a.external ? a.external : a.snapshot->data();
        if (copy) return {{}, a.extra, copy(context, data, a.size, a.extra)};
        if (a.snapshot) return {a.snapshot, a.extra};
        return {std::make_shared<Bytes>(data, data + a.size), a.extra};
      }
      auto &head = m_rows[key];
      if (!head) {
        head = std::make_shared<Version>();
        if (m_trace) {
          head->trace_id = ++m_trace_identity;
          m_trace('V', {head->trace_id});
        }
      }
      const State state = state_of(head);
      if (failed(state)) {
        cause = AbortCause::failed_read_head; parent = tag_of(head); return {};
      }
      const unsigned char *data = physical;
      if (head->external) {
        data = head->external; size = head->external_size; extra = head->extra;
      } else if (head->image) {
        data = head->image->data(); size = head->image->size(); extra = head->extra;
      } else if (state != State::committed) {
        cause = AbortCause::unavailable_read; return {};
      }
      Image bytes;
      unsigned char *external = nullptr;
      if (copy) external = copy(context, data, size, extra);
      else bytes = std::make_shared<Bytes>(data, data + size);
      head->readers.emplace_back(t);
      auto &history = head->reader_history;
      history.erase(std::remove_if(history.begin(), history.end(), [](const auto &ref) {
        auto h = ref.lock(); return !h || !h->retained.load(std::memory_order_acquire);
      }), history.end());
      history.emplace_back(t->history);
      if (!add_source(t, head)) {
        cause = AbortCause::failed_read_head; parent = tag_of(head); return {};
      }
      graph_value(t, head, EdgeKind::wr);
      auto &a = t->accesses.emplace(key, Access{head, {}, {}, bytes, extra}).first->second;
      a.external = external; a.size = size;
      if (m_trace) m_trace('R', {t->id, head->trace_id});
      return {bytes, extra, external};
    }();
    if (!result) abort(t, cause, parent);
    return result;
  }

  bool write(const TransactionPtr &t, const std::string &key,
             const unsigned char *data, size_t size, size_t extra,
             bool external = false, void (*mutate)(void *) = nullptr,
             void *context = nullptr) {
    AbortCause cause = AbortCause::bad_state;
    AbortTag parent;
    const bool result = [&]() {
      RowGuard guard(*this, key, ProfileOp::write);
      if (t->state != State::active) return false;
      auto it = t->accesses.find(key);
      if (it == t->accesses.end()) { cause = AbortCause::missing_write_access; return false; }
      auto &a = it->second;
      auto &head = m_rows.at(key);
      if (a.written) {
        if (head != a.written) { cause = AbortCause::reupdate_head; return false; }
        for (const auto &ref : head->readers) {
          const auto reader = ref.lock();
          if (reader && reader != t && !failed(reader->state)) { cause = AbortCause::reupdate_consumed; return false; }
        }
      } else {
        if (failed(state_of(head))) { cause = AbortCause::failed_write_head; parent = tag_of(head); return false; }
        a.base = head;
        if (a.base != a.source && !add_source(t, a.base)) {
          cause = AbortCause::failed_write_head; parent = tag_of(a.base); return false;
        }
        graph_value(t, a.base, EdgeKind::ww);
        for (const auto &ref : a.base->reader_history) {
          auto reader = ref.lock();
          if (reader && reader != t->history && reader->retained.load(std::memory_order_acquire))
            m_graph.edge(t->history, reader, EdgeKind::rw);
        }
        auto version = std::make_shared<Version>();
        if (m_trace) {
          version->trace_id = ++m_trace_identity;
          m_trace('W', {t->id, version->trace_id, head->trace_id});
        }
        version->owner = t;
        version->writer_history = t->history;
        version->terminal = State::active;
        version->previous = head;
        head->next = version;
        a.written = head = std::move(version);
      }
      if (mutate) mutate(context);
      a.snapshot = external ? Image{} : std::make_shared<Bytes>(data, data + size);
      a.external = external ? data : nullptr; a.size = size;
      a.extra = extra;
      a.written->image = a.snapshot;
      a.written->external = a.external;
      a.written->external_size = size;
      a.written->extra = extra;
      return true;
    }();
    if (!result) abort(t, cause, parent);
    return result;
  }

  Decision certify(const TransactionPtr &t) {
    std::optional<Guard> diagnostic;
    if (m_abort_profile) diagnostic.emplace(*this, ProfileOp::certify);
    if (failed(t->state)) return Decision::aborted;
    if (t->state == State::approved) return Decision::accepted;
    State expected = State::active;
    const bool first_ready = t->state.compare_exchange_strong(expected, State::ready);
    if (failed(t->state)) return Decision::aborted;
    if (m_abort_profile && first_ready) {
      ++m_rw_profile[0];
      if (live_rw_readers(t)) ++m_rw_profile[1];
    }
    if (first_ready) m_graph.state(t->history, State::ready);
    if (!t->certification_requested) {
      t->certification_requested = true;
      m_graph.request(t->history);
      request_graph_check();
    }
    if (t->pending_inputs.load(std::memory_order_acquire)) return Decision::waiting;
    if (!m_graph.enter_commit(t->history)) return Decision::waiting;
    expected = State::ready;
    if (!t->state.compare_exchange_strong(expected, State::approved))
      return Decision::aborted;
    if (m_abort_profile) {
      ++m_rw_profile[9];
      const auto readers = live_rw_readers(t);
      t->rw_bypassed_at_certification = readers != 0;
      if (readers) { ++m_rw_profile[2]; m_rw_profile[3].fetch_add(readers); }
    }
    if (m_trace) m_trace('C', {t->id, 0, 0, 0, 1});
    return Decision::accepted;
  }

  void abort(const TransactionPtr &t, AbortCause cause = AbortCause::external,
             AbortTag parent = {}) {
    if (!t) return;
    Guard guard(*this, ProfileOp::other, false);
    abort_locked(t, cause, parent);
  }

  void finish(const TransactionPtr &t, bool committed) {
    if (!t) return;
    decltype(t->accesses) retired_accesses;
    decltype(t->inputs) retired_inputs;
    decltype(t->dependents) retired_dependents;
    if (!committed) {
      abort(t);
      t->state.store(State::aborted, std::memory_order_release);
      if (m_trace) m_trace('F', {t->id, 0});
    }
    if (m_abort_profile) {
      std::optional<Guard> diagnostic;
      if (committed) diagnostic.emplace(*this, ProfileOp::finish);
      ++m_abort_counts[committed ? 0 : 1];
      if (t->rw_bypassed_at_certification) ++m_rw_profile[committed ? 4 : 7];
      if (committed) {
        const auto readers = live_rw_readers(t);
        if (readers) { ++m_rw_profile[5]; m_rw_profile[6].fetch_add(readers); }
      } else {
        ++m_abort_counts[3 + size_t(t->abort_cause)];
        ++m_abort_counts[3 + size_t(AbortCause::count) + size_t(t->abort_tag.root)];
      }
    }
    {
      std::lock_guard<std::mutex> lock(t->dependents_mutex);
      if (committed) {
        if (t->state != State::approved) std::terminate();
        if (m_trace) m_trace('F', {t->id, 1});
        t->state.store(State::committed, std::memory_order_release);
      }
      t->dependents.swap(retired_dependents);
    }
    m_graph.state(t->history, t->state);
    for (const auto &ref : retired_dependents) {
      if (!committed) continue;
      if (auto consumer = ref.lock()) {
        std::lock_guard<std::mutex> lock(consumer->event_mutex);
        const auto before = consumer->pending_inputs.fetch_sub(1, std::memory_order_acq_rel);
        if (!before) std::terminate();
        if (before == 1) consumer->changed.notify_one();
      }
    }
    request_graph_check();
    for (auto &entry : t->accesses) {
      RowGuard row(*this, entry.first, ProfileOp::finish);
      auto &a = entry.second;
      auto &readers = a.source->readers;
      readers.erase(std::remove_if(readers.begin(), readers.end(),
                                  [&](const std::weak_ptr<Transaction> &ref) {
                                    auto reader = ref.lock(); return !reader || reader == t;
                                  }), readers.end());
      if (!a.written) continue;
      a.written->terminal = t->state;
      if (m_abort_profile && !committed) a.written->abort_tag = t->abort_tag;
      a.written->owner.reset();
      a.written->image.reset();
      a.written->external = nullptr;
      if (committed) a.written->previous.reset();
      if (a.base->next.lock() == a.written) a.base->next.reset();
      auto &head = m_rows.at(entry.first);
      while (state_of(head) == State::aborted && head->previous) head = head->previous;
    }
    t->accesses.swap(retired_accesses);
    t->inputs.swap(retired_inputs);
    {
      Guard guard(*this, ProfileOp::finish, false);
      m_active.erase(t->id); m_bound.erase(t->wait_key);
    }
  }

  bool aborted(const TransactionPtr &t) { return t && failed(t->state); }

  bool wait_tick(const TransactionPtr &t) {
    std::unique_lock<std::mutex> lock(t->event_mutex);
    const bool graph_wait = t->pending_inputs.load(std::memory_order_acquire) == 0;
    t->changed.wait_for(lock, std::chrono::milliseconds(10), [&] {
      if (failed(t->state)) return true;
      if (!graph_wait) return t->pending_inputs.load(std::memory_order_acquire) == 0;
      return m_graph.serialization_approved(t->history);
    });
    return graph_wait;
  }
  std::vector<WaitKey> drain_engine_aborts() {
    Guard guard(*this, ProfileOp::detect, false);
    std::vector<WaitKey> result;
    result.swap(m_engine_aborts);
    return result;
  }

  size_t active_count() {
    Guard guard(*this, ProfileOp::other, false);
    return m_active.size();
  }

 private:
  size_t live_rw_readers(const TransactionPtr &t) const {
    std::unordered_set<uint64_t> readers;
    for (const auto &entry : t->accesses) {
      const auto &a = entry.second;
      if (!a.written || !a.base) continue;
      for (const auto &ref : a.base->readers) {
        const auto reader = ref.lock();
        if (reader && reader != t && !failed(reader->state) &&
            reader->state != State::committed)
          readers.insert(reader->id);
      }
    }
    return readers.size();
  }
  static bool failed(State s) { return s == State::aborting || s == State::aborted; }
  static AbortTag tag_of(const VersionPtr &v) {
    const auto owner = v->owner.lock();
    return owner ? owner->abort_tag : v->abort_tag;
  }
  void tag_abort(const TransactionPtr &t, AbortCause cause, AbortTag parent = {}) {
    if (!m_abort_profile) return;
    t->abort_cause = cause;
    t->abort_tag = parent.root == AbortCause::none ? AbortTag{cause, t->id} : parent;
  }
  static State state_of(const VersionPtr &v) {
    const auto owner = v->owner.lock();
    return owner ? owner->state.load(std::memory_order_acquire) : v->terminal;
  }
  static const std::vector<VersionPtr> &sources(const TransactionPtr &t) {
    return t->inputs;
  }
  void graph_value(const TransactionPtr &t, const VersionPtr &v, EdgeKind kind) {
    auto owner = v->writer_history.lock();
    if (owner && owner != t->history && owner->retained.load(std::memory_order_acquire))
      m_graph.edge(t->history, owner, kind);
    request_graph_check();
  }
  bool add_source(const TransactionPtr &t, const VersionPtr &source) {
    t->inputs.push_back(source);
    auto owner = source->owner.lock();
    if (owner && owner != t) {
      rcc_duration::DetailLock guard(owner->dependents_mutex,
          rcc_duration::wh_write_source_wait,rcc_duration::wh_write_source_hold);
      if (failed(owner->state)) return false;
      if (owner->state != State::committed) {
        t->pending_inputs.fetch_add(1, std::memory_order_relaxed);
        owner->dependents.emplace_back(t);
      }
    }
    return true;
  }
  static void signal(const TransactionPtr &t) {
    std::lock_guard<std::mutex> lock(t->event_mutex);
    t->changed.notify_one();
  }
  void request_graph_check() {
    rcc_duration::Scope notify_time(rcc_duration::wh_write_notify,rcc_duration::detail_write);
    if (m_notify) m_notify();
  }

  void abort_locked(const TransactionPtr &t,
                    AbortCause cause = AbortCause::external, AbortTag parent = {}) {
    {
      std::lock_guard<std::mutex> lock(t->dependents_mutex);
      if (failed(t->state) || t->state == State::committed) return;
      tag_abort(t, cause, parent);
      t->state.store(State::aborting, std::memory_order_release);
    }
    propagate_abort_locked(t);
  }
  void propagate_abort_locked(const TransactionPtr &t) {
    std::vector<TransactionPtr> pending{t};
    for (size_t i = 0; i < pending.size(); ++i) {
      const auto current = pending[i];
      if (m_trace) m_trace('A', {current->id, uint64_t(current->abort_cause),
          uint64_t(current->abort_tag.root), current->abort_tag.id});
      m_graph.state(current->history, current->state);
      if (current->wait_key.first) {
        m_engine_aborts.push_back(current->wait_key);
      }
      signal(current);
      decltype(current->dependents) dependents;
      {
        std::lock_guard<std::mutex> lock(current->dependents_mutex);
        dependents.swap(current->dependents);
      }
      for (const auto &ref : dependents) {
        auto consumer = ref.lock();
        if (!consumer) continue;
        std::lock_guard<std::mutex> lock(consumer->dependents_mutex);
        if (failed(consumer->state) || consumer->state == State::committed) continue;
        tag_abort(consumer, AbortCause::cascade, current->abort_tag);
        consumer->state.store(State::aborting, std::memory_order_release);
        pending.push_back(std::move(consumer));
      }
    }
    request_graph_check();
  }
  std::mutex m_mutex;
  std::vector<WaitKey> m_engine_aborts;
  const bool m_profile;
  const bool m_abort_profile;
  std::array<std::atomic<uint64_t>, 10> m_rw_profile{};
  std::array<std::atomic<uint64_t>, 3 + 2 * size_t(AbortCause::count)> m_abort_counts{};
  std::array<std::atomic<uint64_t>, 24> m_profile_data{};
  void (*const m_notify)();
  ExactGraph m_graph;
  const TraceSink m_trace;
  std::atomic<uint64_t> m_trace_identity{0};
  uint64_t m_identity = 0;
  std::map<WaitKey, std::weak_ptr<Transaction>> m_bound;
  Rows m_rows;
  std::unordered_map<uint64_t, TransactionPtr> m_active;
};
}  // namespace rcc
#endif
