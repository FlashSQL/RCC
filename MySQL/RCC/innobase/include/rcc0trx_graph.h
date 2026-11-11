#ifndef RCC0TRX_GRAPH_H
#define RCC0TRX_GRAPH_H
#include <array>
#include <cstdlib>
#include <thread>

namespace rcc {
class PerTrxGraph {
  using Link = std::pair<WaitKey, EdgeKind>;
  static constexpr size_t block_size = 64;
  static constexpr uint64_t phase_mask = 7, retired_phase = 6, dependency_version_step = 8;
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "graph control must not introduce a hidden global lock");
  struct ConflictNode;
  using ConflictNodePtr = std::shared_ptr<ConflictNode>;
  struct Entry { Link link{}; std::weak_ptr<ConflictNode> target; };
  struct Block {
    std::array<Entry, block_size> entries;
    std::unique_ptr<Block> next;
  };
 public:
  using Stats = GraphStats;
  struct Check {
    struct Ticket { HistoryPtr history; uint64_t dependency_version; size_t prefix; };
    std::vector<Ticket> tickets;
    std::vector<DependencyEdge> cycle;
#ifdef RCC_TRX_GRAPH_TEST_HOOK
    DependencySnapshot reference;
#endif
  };
 private:
  struct ConflictNode : History {
    explicit ConflictNode(WaitKey key) : History(key), head(new Block), tail(head.get()) {}
    std::mutex mutex;
    alignas(64) std::atomic<uint64_t> control{dependency_version_step};
    std::atomic<uint64_t> checked_dependency_version{0};
    size_t checked_prefix = 0;
    bool requested = false, serialization_approved = false;
    unsigned profile_phase = 0;
    size_t published = 0, final_seq = 0;
    std::unique_ptr<Block> head;
    Block *tail;
    std::set<Link> dedup;
    Stats counters;
  };
  struct ViewNode {
    ConflictNodePtr conflict_node;
    Block *block = nullptr;
    size_t offset = 0, imported = 0, observed_size = 0, final_seq = 0;
    uint64_t control = dependency_version_step;
    bool requested = false, serialization_approved = false, checked = false, sampled = false;
    std::set<Link> predecessors, successors;
    unsigned char color = 0;
    size_t stack_position = 0;
  };
  struct alignas(64) Dirty { std::atomic<bool> value{false}; };

  static uint64_t dependency_version(uint64_t c) { return c >> 3; }
  static State phase(uint64_t c) { return State(c & phase_mask); }
  static bool failed(uint64_t c) {
    const auto p = c & phase_mask;
    return p == uint64_t(State::aborting) || p == uint64_t(State::aborted) || p == retired_phase;
  }
  static ConflictNodePtr conflict_node(const HistoryPtr &h) { return std::static_pointer_cast<ConflictNode>(h); }
  static uint64_t with_phase(uint64_t c, State p) { return (c & ~phase_mask) | uint64_t(p); }
  static void add_counters(Stats &a, const Stats &b) {
    a.approvals += b.approvals; a.precheck_reused += b.precheck_reused;
    a.precheck_missed += b.precheck_missed; a.precheck_invalidated += b.precheck_invalidated;
  }
  static size_t bucket(WaitKey key) {
    uint64_t v = (key.first >> 6) ^ (key.second * UINT64_C(0x9e3779b97f4a7c15));
    v ^= v >> 27; v *= UINT64_C(0x94d049bb133111eb); return (v ^ (v >> 31)) & 63;
  }
  void changed(WaitKey key = {}) { dirty[bucket(key)].value.store(true, std::memory_order_release); }
  bool take_pending() {
    bool result = false;
    for (auto &d : dirty) result |= d.value.exchange(false, std::memory_order_acq_rel);
    return result;
  }
  bool checked(const ConflictNode &r, uint64_t c) const {
    return r.checked_dependency_version.load(std::memory_order_acquire) == dependency_version(c) &&
           r.checked_prefix == r.published;
  }
  bool invalidate_active(ConflictNode &r, ConflictNode &source) {
    auto c = r.control.load(std::memory_order_acquire);
    while (!failed(c) && phase(c) == State::active) {
      if (c > std::numeric_limits<uint64_t>::max() - dependency_version_step) std::terminate();
      const bool was_checked = r.checked_dependency_version.load(std::memory_order_acquire) == dependency_version(c);
      if (r.control.compare_exchange_weak(c, c + dependency_version_step,
                                         std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (was_checked) ++source.counters.precheck_invalidated;
        return true;
      }
    }
    return !failed(c);
  }
  void set_phase(ConflictNode &r, State p) {
    auto c = r.control.load(std::memory_order_acquire);
    while (!r.control.compare_exchange_weak(c, with_phase(c, p),
                                            std::memory_order_acq_rel, std::memory_order_acquire)) {}
  }
  std::vector<ConflictNodePtr> pin_all(Stats *history = nullptr) {
    std::vector<ConflictNodePtr> pins;
    for (;;) {
      std::unique_lock<std::mutex> lock(directory_mutex);
      if (pins.capacity() < conflict_nodes.size()) {
        const auto size = conflict_nodes.size(); lock.unlock(); pins.reserve(size + 32); continue;
      }
      for (const auto &n : conflict_nodes) pins.push_back(n.second);
      if (history) { *history = retired_counters; history->peak_nodes = peak_nodes; }
      return pins;
    }
  }
  ViewNode &ensure_view(const ConflictNodePtr &r) {
    auto result = view.try_emplace(r->key);
    if (result.second) {
      result.first->second.conflict_node = r;
      result.first->second.block = r->head.get();
      result.first->second.control = r->control.load(std::memory_order_acquire);
    }
    return result.first->second;
  }
  WaitKey retry() { stale.fetch_add(1, std::memory_order_relaxed); changed(); return {}; }
  bool contains(const ConflictNode &r, Link edge) const {
    size_t left = r.published;
    for (auto b = r.head.get(); left; b = b->next.get()) {
      const auto n = std::min(left, block_size);
      for (size_t i = 0; i < n; ++i) if (b->entries[i].link == edge) return true;
      left -= n;
    }
    return false;
  }
  void erase_view(std::map<WaitKey, ViewNode>::iterator it,
                  std::set<WaitKey> &candidates) {
    const auto key = it->first;
    for (const auto &e : it->second.predecessors) {
      auto p = view.find(e.first);
      if (p != view.end()) p->second.successors.erase({key, e.second});
    }
    for (const auto &e : it->second.successors) {
      auto s = view.find(e.first);
      if (s == view.end()) continue;
      s->second.predecessors.erase({key, e.second});
      if (phase(s->second.control) == State::committed && s->second.predecessors.empty())
        candidates.insert(e.first);
    }
    view.erase(it);
  }
  bool retire(ViewNode &v) {
    auto r = v.conflict_node;
    Stats completed;
    bool committed;
    {
      std::unique_lock<std::mutex> local(r->mutex, std::try_to_lock);
      if (!local.owns_lock()) { changed(r->key); return false; }
      const auto c = r->control.load(std::memory_order_acquire);
      committed = phase(c) == State::committed;
      if (phase(c) != State::aborted && !(committed && v.predecessors.empty()))
        return false;
      if (committed) {
        assert(v.imported == r->published);
        assert(r->final_seq == r->published);
      }
      r->control.store((c & ~phase_mask) | retired_phase, std::memory_order_release);
      r->retained.store(false, std::memory_order_release);
      completed = r->counters;
    }
    {
      std::lock_guard<std::mutex> lock(directory_mutex);
      conflict_nodes.erase(r->key); add_counters(retired_counters, completed);
    }
    if (committed) reclaimed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  void collect() {
    std::set<WaitKey> candidates;
    for (auto it = view.begin(); it != view.end();) {
      auto current = it++;
      if (phase(current->second.control) == State::aborted) {
        if (retire(current->second)) erase_view(current, candidates);
      } else if (phase(current->second.control) == State::committed && current->second.predecessors.empty())
        candidates.insert(current->first);
    }
    while (!candidates.empty()) {
      const auto key = *candidates.begin(); candidates.erase(candidates.begin());
      auto it = view.find(key);
      if (it != view.end() && retire(it->second)) erase_view(it, candidates);
    }
  }
 public:
  HistoryPtr begin(WaitKey key) {
    auto r = std::make_shared<ConflictNode>(key);
    {
      std::lock_guard<std::mutex> lock(directory_mutex);
      if (!conflict_nodes.emplace(key, r).second) std::terminate();
      peak_nodes = std::max<uint64_t>(peak_nodes, conflict_nodes.size());
    }
    changed(key); return r;
  }
  HistoryPtr lookup(WaitKey key) {
    std::lock_guard<std::mutex> lock(directory_mutex);
    auto it = conflict_nodes.find(key); return it == conflict_nodes.end() ? HistoryPtr{} : it->second;
  }
  void edge(const HistoryPtr &from, const HistoryPtr &to, EdgeKind kind) {
    if (!from || !to || from == to) return;
    auto f = conflict_node(from), t = conflict_node(to);
    if (failed(f->control.load(std::memory_order_acquire)) ||
        failed(t->control.load(std::memory_order_acquire))) return;
    const Link link{t->key, kind};
    if (f->dedup.find(link) != f->dedup.end()) return;
    std::unique_ptr<Block> next;
    if (f->published && f->published % block_size == 0) next.reset(new Block);
    f->dedup.emplace(link);
    rcc_duration::DetailLock local(f->mutex, rcc_duration::wh_write_graph_wait,
                                  rcc_duration::wh_write_graph_hold);
    auto fc = f->control.load(std::memory_order_acquire);
    if (failed(fc) || failed(t->control.load(std::memory_order_acquire))) return;
    assert(phase(fc) == State::active);
    if (!invalidate_active(*f, *f) || !invalidate_active(*t, *f)) return;
    if (next) { f->tail->next = std::move(next); f->tail = f->tail->next.get(); }
    auto &entry = f->tail->entries[f->published % block_size];
    entry.link = link; entry.target = t; ++f->published;
    changed(f->key);
  }
  void state(const HistoryPtr &history, State value) {
    if (!history) return;
    auto r = conflict_node(history);
    if (value != State::committed && value != State::aborted) {
      std::lock_guard<std::mutex> local(r->mutex);
      const auto c = r->control.load(std::memory_order_acquire);
      if ((c & phase_mask) == retired_phase || phase(c) == value ||
          (failed(c) && value != State::aborting)) return;
      if (value == State::ready) r->final_seq = r->published;
      set_phase(*r, value); changed(r->key); return;
    }
    std::lock_guard<std::mutex> local(r->mutex);
    const auto c = r->control.load(std::memory_order_acquire);
    if ((c & phase_mask) == retired_phase || phase(c) == value ||
        (failed(c) && value == State::committed)) return;
    r->final_seq = r->published;
    set_phase(*r, value); changed(r->key);
  }
  void request(const HistoryPtr &history) {
    if (!history) return;
    auto r = conflict_node(history); std::lock_guard<std::mutex> lock(r->mutex);
    const auto c = r->control.load(std::memory_order_acquire);
    if (phase(c) != State::ready || r->requested) return;
    r->requested = true;
    if (checked(*r, c)) { r->serialization_approved = true; ++r->counters.approvals; ++r->counters.precheck_reused; }
    else ++r->counters.precheck_missed;
    changed(r->key);
  }
  bool serialization_approved(const HistoryPtr &history) {
    if (!history) return false;
    auto r = conflict_node(history); std::lock_guard<std::mutex> lock(r->mutex);
    return r->serialization_approved && !failed(r->control.load(std::memory_order_acquire));
  }
  bool enter_commit(const HistoryPtr &history) {
    if (!history) return false;
    auto r = conflict_node(history); std::lock_guard<std::mutex> lock(r->mutex);
    if (!r->serialization_approved || phase(r->control.load(std::memory_order_acquire)) != State::ready) return false;
    set_phase(*r, State::approved); changed(r->key); return true;
  }
 private:
  bool refresh_view() {
    if (!take_pending()) return false;
    try {
      auto pins = pin_all();
      for (auto &entry : view) entry.second.sampled = false;
      for (const auto &r : pins) if (r->retained.load(std::memory_order_acquire)) ensure_view(r);
      for (const auto &r : pins) {
        auto it = view.find(r->key); if (it == view.end()) continue;
        auto &v = it->second;
        std::array<Entry, block_size> delta;
        size_t count = 0, next_offset = v.offset;
        Block *next_block = v.block;
        {
          std::lock_guard<std::mutex> local(r->mutex);
          v.control = r->control.load(std::memory_order_acquire);
          v.final_seq = r->final_seq; v.observed_size = r->published;
          v.requested = r->requested; v.serialization_approved = r->serialization_approved; v.checked = checked(*r, v.control);
          v.sampled = true;
          count = std::min(block_size, r->published - v.imported);
          for (size_t n = 0; n < count; ++n) {
            if (next_offset == block_size) { next_block = next_block->next.get(); next_offset = 0; }
            delta[n] = next_block->entries[next_offset++];
          }
        }
#ifdef RCC_TRX_GRAPH_TEST_HOOK
        if (snapshot_hook) snapshot_hook(r->key);
#endif
        for (size_t n = 0; n < count; ++n) {
          auto target = delta[n].target.lock();
          if (!target || !target->retained.load(std::memory_order_acquire)) continue;
          auto &predecessor = ensure_view(target);
          v.predecessors.insert(delta[n].link);
          predecessor.successors.emplace(r->key, delta[n].link.second);
        }
        v.imported += count; v.block = next_block; v.offset = next_offset;
        if (v.imported != v.observed_size) changed(r->key);
      }
      collect();
      size_t edge_count = 0;
      for (const auto &entry : view) edge_count += entry.second.predecessors.size();
      observed_edges.store(edge_count, std::memory_order_relaxed);
      auto peak = peak_edges.load(std::memory_order_relaxed);
      while (peak < edge_count && !peak_edges.compare_exchange_weak(peak, edge_count)) {}
      scans.fetch_add(1, std::memory_order_relaxed); return true;
    } catch (...) { changed(); throw; }
  }
  static bool covered(const ViewNode &v) {
    return v.sampled && v.imported == v.observed_size &&
        (phase(v.control) != State::ready || v.imported == v.final_seq);
  }
  void export_view(DependencySnapshot &s) {
    s = {};
    for (const auto &entry : view) {
      const auto &v = entry.second;
      const auto state = phase(v.control);
      s.states.emplace(entry.first, state);
      if (covered(v) && (state == State::active || state == State::ready) && !v.serialization_approved && !v.checked) {
        s.dependency_versions.emplace(entry.first, dependency_version(v.control));
        s.prefixes.emplace(entry.first, v.imported); s.pins.emplace(entry.first, v.conflict_node);
      }
      if (state == State::ready && v.requested && !v.serialization_approved) s.requests.push_back(entry.first);
      for (const auto &e : v.predecessors) s.edges.push_back({entry.first, e.first, e.second});
    }
  }
  std::vector<DependencyEdge> view_cycle() {
    using Iterator = std::map<WaitKey, ViewNode>::iterator;
    struct Frame { Iterator node; std::set<Link>::const_iterator next; DependencyEdge incoming; };
    std::vector<Frame> stack;
    stack.reserve(view.size());
    for (auto &entry : view) entry.second.color = 0;
    for (auto root = view.begin(); root != view.end(); ++root) {
      auto &r = root->second;
      if (r.color) continue;
      r.color = 1; r.stack_position = 0;
      stack.push_back({root, r.predecessors.begin(), {}});
      while (!stack.empty()) {
        auto &frame = stack.back();
        auto &v = frame.node->second;
        if (failed(v.control) || frame.next == v.predecessors.end()) {
          v.color = 2; stack.pop_back(); continue;
        }
        const auto link = *frame.next++;
        auto target = view.find(link.first);
        if (target == view.end()) std::terminate();
        auto &t = target->second;
        if (link.second == EdgeKind::native || failed(t.control) || t.color == 2) continue;
        const DependencyEdge edge{frame.node->first, target->first, link.second};
        if (t.color == 0) {
          t.color = 1; t.stack_position = stack.size();
          stack.push_back({target, t.predecessors.begin(), edge});
          continue;
        }
        std::vector<DependencyEdge> cycle;
        cycle.reserve(stack.size() - t.stack_position);
        for (size_t i = t.stack_position + 1; i < stack.size(); ++i)
          cycle.push_back(stack[i].incoming);
        cycle.push_back(edge);
        return cycle;
      }
    }
    return {};
  }
 public:
  bool snapshot(DependencySnapshot &s) {
    std::lock_guard<std::mutex> detector(detector_mutex);
    if (!refresh_view()) return false;
    try { export_view(s); return true; }
    catch (...) { changed(); throw; }
  }
  bool inspect(Check &s) {
    std::lock_guard<std::mutex> detector(detector_mutex);
    if (!refresh_view()) return false;
    try {
      s = {};
      s.tickets.reserve(view.size());
      for (const auto &entry : view) {
        const auto &v = entry.second;
        const auto state = phase(v.control);
        if (covered(v) && (state == State::active || state == State::ready) && !v.serialization_approved && !v.checked)
          s.tickets.push_back({v.conflict_node, dependency_version(v.control), v.imported});
      }
      s.cycle = view_cycle();
#ifdef RCC_TRX_GRAPH_TEST_HOOK
      export_view(s.reference);
#endif
      return true;
    } catch (...) { changed(); throw; }
  }
  std::vector<WaitKey> approve(const Check &s) {
    std::vector<WaitKey> result;
    for (const auto &ticket : s.tickets) {
      auto r = conflict_node(ticket.history); std::lock_guard<std::mutex> local(r->mutex);
      auto c = r->control.load(std::memory_order_acquire);
      if ((phase(c) != State::active && phase(c) != State::ready) || failed(c) || r->serialization_approved ||
          dependency_version(c) != ticket.dependency_version || r->published != ticket.prefix) continue;
      r->checked_prefix = ticket.prefix;
      r->checked_dependency_version.store(ticket.dependency_version, std::memory_order_release);
      if (phase(c) == State::ready && r->requested) {
        r->serialization_approved = true; ++r->counters.approvals; result.push_back(r->key);
      }
    }
    if (!result.empty()) changed(); return result;
  }
  std::vector<WaitKey> approve(const DependencySnapshot &s) {
    std::vector<WaitKey> result;
    for (const auto &entry : s.dependency_versions) {
      auto pin = s.pins.find(entry.first); if (pin == s.pins.end()) continue;
      auto r = conflict_node(pin->second); std::lock_guard<std::mutex> local(r->mutex);
      auto c = r->control.load(std::memory_order_acquire);
      if ((phase(c) != State::active && phase(c) != State::ready) || failed(c) || r->serialization_approved ||
          dependency_version(c) != entry.second || r->published != s.prefixes.at(entry.first)) continue;
      r->checked_prefix = s.prefixes.at(entry.first);
      r->checked_dependency_version.store(entry.second, std::memory_order_release);
      if (phase(c) == State::ready && r->requested) {
        r->serialization_approved = true; ++r->counters.approvals; result.push_back(r->key);
      }
    }
    if (!result.empty()) changed(); return result;
  }
  WaitKey reject(const std::vector<DependencyEdge> &cycle) {
    std::map<WaitKey, ConflictNodePtr> pins;
    {
      std::lock_guard<std::mutex> lock(directory_mutex);
      for (const auto &e : cycle) for (auto key : {e.from, e.to}) {
        auto it = conflict_nodes.find(key); if (it != conflict_nodes.end()) pins.emplace(key, it->second);
      }
    }
    std::vector<std::unique_lock<std::mutex>> locks; locks.reserve(pins.size());
    for (const auto &p : pins) {
      locks.emplace_back(p.second->mutex, std::try_to_lock);
      if (!locks.back().owns_lock()) return retry();
    }
    WaitKey victim{}; bool victim_reader = false;
    for (size_t i = 0; i < cycle.size(); ++i) {
      const auto &e = cycle[i];
      if (e.to != cycle[(i + 1) % cycle.size()].from) return retry();
      auto f = pins.find(e.from), t = pins.find(e.to);
      if (f == pins.end() || t == pins.end() ||
          failed(f->second->control.load(std::memory_order_acquire)) ||
          failed(t->second->control.load(std::memory_order_acquire)) ||
          !contains(*f->second, {e.to, e.kind})) return retry();
      const auto p = phase(f->second->control.load(std::memory_order_acquire));
      if (!f->second->serialization_approved && (p == State::ready || p == State::active)) {
        const auto &incoming = cycle[(i + cycle.size() - 1) % cycle.size()];
        const bool reader = e.kind == EdgeKind::wr || incoming.kind == EdgeKind::rw;
        if (victim == WaitKey{} || reader || !victim_reader) { victim = e.from; victim_reader = reader; }
      }
    }
    if (victim == WaitKey{}) std::terminate();
    set_phase(*pins.at(victim), State::aborting); changed(victim);
    serial_victims.fetch_add(1, std::memory_order_relaxed);
    return victim;
  }
  bool needs_check() const {
    for (const auto &d : dirty) if (d.value.load(std::memory_order_acquire)) return true;
    return false;
  }
  void profile_phase(const HistoryPtr &h, unsigned p) {
    if (!h) return; auto r = conflict_node(h); std::lock_guard<std::mutex> lock(r->mutex); r->profile_phase = p;
  }
  WaitObservation observe() {
    WaitObservation s;
    auto pins = pin_all();
    for (const auto &r : pins) {
      std::lock_guard<std::mutex> local(r->mutex);
      auto c = r->control.load(std::memory_order_acquire);
      if ((c & phase_mask) == retired_phase || phase(c) == State::aborted) continue;
      s.graph.states.emplace(r->key, phase(c));
      if (r->serialization_approved) s.serialization_approved.insert(r->key);
      if (phase(c) == State::approved) s.phases.emplace(r->key, r->profile_phase);
      size_t left = r->published;
      for (auto b = r->head.get(); left; b = b->next.get()) {
        const auto n = std::min(left, block_size);
        for (size_t i = 0; i < n; ++i) {
          const auto &e = b->entries[i]; auto target = e.target.lock();
          if (target && target->retained.load() && phase(target->control.load()) != State::aborted)
            s.graph.edges.push_back({r->key, e.link.first, e.link.second});
        }
        left -= n;
      }
    }
    std::sort(s.graph.edges.begin(), s.graph.edges.end(), [](const auto &a, const auto &b) {
      if (a.from != b.from) return a.from < b.from;
      return std::make_pair(a.to, a.kind) < std::make_pair(b.to, b.kind);
    });
    return s;
  }
  Stats metrics() {
    Stats result; auto pins = pin_all(&result);
    for (const auto &r : pins) {
      std::lock_guard<std::mutex> lock(r->mutex);
      if (!r->retained.load()) continue;
      add_counters(result, r->counters);
      if (phase(r->control.load()) != State::aborted) ++result.nodes;
    }
    result.edges = observed_edges.load(); result.peak_edges = peak_edges.load();
    result.scans = scans.load(); result.stale = stale.load(); result.reclaimed = reclaimed.load();
    result.serial_victims = serial_victims.load();
    return result;
  }
#ifdef RCC_TRX_GRAPH_TEST_HOOK
  void (*snapshot_hook)(WaitKey) = nullptr;
#endif
 private:
  std::mutex directory_mutex, detector_mutex;
  std::map<WaitKey, ConflictNodePtr> conflict_nodes;
  uint64_t peak_nodes = 0;
  Stats retired_counters;
  std::map<WaitKey, ViewNode> view;
  std::array<Dirty, 64> dirty;
  std::atomic<uint64_t> scans{0}, stale{0}, reclaimed{0};
  std::atomic<uint64_t> serial_victims{0}, observed_edges{0}, peak_edges{0};
};

class ExactGraph {
 public:
  using Stats = PerTrxGraph::Stats;
  using Check = PerTrxGraph::Check;
  HistoryPtr begin(WaitKey k) { return graph.begin(k); }
  void edge(WaitKey f, WaitKey t, EdgeKind k) {
    graph.edge(graph.lookup(f),graph.lookup(t),k);
  }
  void edge(const HistoryPtr &f, const HistoryPtr &t, EdgeKind k) {
    if (f && t) graph.edge(f,t,k);
  }
  void state(const HistoryPtr &h, State s) { graph.state(h,s); }
  void state(WaitKey k, State s) { graph.state(graph.lookup(k),s); }
  void request(const HistoryPtr &h) { graph.request(h); }
  void request(WaitKey k) { graph.request(graph.lookup(k)); }
  bool serialization_approved(const HistoryPtr &h) { return graph.serialization_approved(h); }
  bool serialization_approved(WaitKey k) { return graph.serialization_approved(graph.lookup(k)); }
  bool enter_commit(const HistoryPtr &h) { return graph.enter_commit(h); }
  bool enter_commit(WaitKey k) { return graph.enter_commit(graph.lookup(k)); }
  bool snapshot(DependencySnapshot &s) { return graph.snapshot(s); }
  bool inspect(Check &s) { return graph.inspect(s); }
  std::vector<WaitKey> approve(const Check &s) { return graph.approve(s); }
  std::vector<WaitKey> approve(const DependencySnapshot &s) { return graph.approve(s); }
  WaitKey reject(const std::vector<DependencyEdge> &c) { return graph.reject(c); }
  bool needs_check() { return graph.needs_check(); }
  bool per_trx() const { return true; }
  Stats metrics() { return graph.metrics(); }
  WaitObservation observe() { return graph.observe(); }
  void profile_phase(WaitKey k, unsigned p) { graph.profile_phase(graph.lookup(k),p); }
 private:
  PerTrxGraph graph;
};
} // namespace rcc
#endif
