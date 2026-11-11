#ifndef RCC0AS_GRAPH_H
#define RCC0AS_GRAPH_H
#include "rcc0duration.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace rcc {
struct History;
using HistoryPtr = std::shared_ptr<History>;
using WaitKey = std::pair<uintptr_t, uint64_t>;
enum class State { active, ready, approved, committed, aborting, aborted };
enum class EdgeKind : uint8_t { wr, ww, rw, native };
struct DependencyEdge { WaitKey from, to; EdgeKind kind; };
struct DependencySnapshot {
  std::map<WaitKey, size_t> prefixes;
  std::map<WaitKey, HistoryPtr> pins;
  std::map<WaitKey, State> states;
  std::vector<WaitKey> requests;
  std::map<WaitKey, uint64_t> dependency_versions;
  std::vector<DependencyEdge> edges;
};
struct WaitObservation {
  DependencySnapshot graph;
  std::set<WaitKey> serialization_approved;
  std::map<WaitKey, unsigned> phases;
};
inline std::vector<DependencyEdge> dependency_cycle(
    const DependencySnapshot &snapshot, bool mixed) {
  struct Node { State state = State::committed; std::vector<size_t> outgoing; };
  std::map<WaitKey, size_t> index;
  std::vector<Node> nodes;
  const auto node = [&](WaitKey key) {
    auto inserted = index.try_emplace(key, nodes.size());
    if (inserted.second) nodes.emplace_back();
    return inserted.first->second;
  };
  for (const auto &entry : snapshot.states) nodes[node(entry.first)].state = entry.second;
  for (size_t i = 0; i < snapshot.edges.size(); ++i) {
    const auto &e = snapshot.edges[i];
    const auto from = node(e.from); node(e.to);
    const auto state = nodes[from].state;
    if (mixed) {
      const auto target = nodes[index.at(e.to)].state;
      if (e.kind != EdgeKind::native && target == State::committed) continue;
      if (e.kind == EdgeKind::rw ||
          (e.kind != EdgeKind::native && state != State::ready)) continue;
    } else {
      const auto to = nodes[index.at(e.to)].state;
      if (e.kind == EdgeKind::native ||
          state == State::aborting || state == State::aborted ||
          to == State::aborting || to == State::aborted) continue;
    }
    nodes[from].outgoing.push_back(i);
  }
  struct Frame { size_t node, next, incoming; };
  std::vector<unsigned char> color(nodes.size(), 0);
  std::vector<size_t> position(nodes.size(), 0);
  std::vector<Frame> stack;
  for (size_t root = 0; root < nodes.size(); ++root) {
    if (color[root]) continue;
    stack.push_back({root, 0, 0}); color[root] = 1; position[root] = 0;
    while (!stack.empty()) {
      auto &f = stack.back();
      if (f.next == nodes[f.node].outgoing.size()) {
        color[f.node] = 2; stack.pop_back(); continue;
      }
      const auto edge_id = nodes[f.node].outgoing[f.next++];
      const auto &edge = snapshot.edges[edge_id];
      const auto to = index.at(edge.to);
      if (color[to] == 2) continue;
      if (!color[to]) {
        color[to] = 1; position[to] = stack.size();
        stack.push_back({to, 0, edge_id}); continue;
      }
      const auto first = position[to];
      bool eligible = false;
      for (size_t i = first; i < stack.size(); ++i) {
        const auto state = nodes[stack[i].node].state;
        eligible |= state == State::ready || (!mixed && state == State::active);
      }
      if (mixed && !eligible) continue;
      std::vector<DependencyEdge> cycle;
      for (size_t i = first + 1; i < stack.size(); ++i)
        cycle.push_back(snapshot.edges[stack[i].incoming]);
      cycle.push_back(edge);
      return cycle;
    }
  }
  return {};
}
struct History {
  explicit History(WaitKey k) : key(k) {}
  const WaitKey key;
  std::atomic<bool> retained{true};
};

struct GraphStats {
  uint64_t nodes = 0, edges = 0, peak_nodes = 0, peak_edges = 0;
  uint64_t reclaimed = 0, approvals = 0, stale = 0, scans = 0;
  uint64_t serial_victims = 0, mixed_victims = 0;
  uint64_t precheck_reused = 0, precheck_missed = 0, precheck_invalidated = 0;
};
}  // namespace rcc
#include "rcc0trx_graph.h"
#endif
