#ifndef RCC0AS_PROFILE_H
#define RCC0AS_PROFILE_H
#include "rcc0as_graph.h"
#include <array>
#include <deque>
#include <sstream>
namespace rcc {
enum WaitOrigin : unsigned {
  EXECUTION=1, APPROVAL=2, DISPATCH=4, APPLY=8, ENGINE=16,
  FINISH=32, NATIVE=64, ROLLBACK=128, CYCLE=256, UNKNOWN=512
};
struct WaitSummary {
  uint64_t ready=0, value=0, own=0, both=0, runnable=0, mixed=0;
  std::array<uint64_t,10> only{}, marginal{};
  std::array<uint64_t,4> approved{};
  std::string str() const {
    std::ostringstream o;
    o << "v1 ready=" << ready << " value=" << value << " own=" << own
      << " both=" << both << " runnable=" << runnable << " mixed=" << mixed;
    for (size_t i=0;i<10;++i) o << " r" << i << '=' << only[i] << " m" << i << '=' << marginal[i];
    for (size_t i=0;i<4;++i) o << " c" << i << '=' << approved[i];
    return o.str();
  }
};
inline WaitSummary summarize_waits(const WaitObservation &s) {
  WaitSummary out;
  std::map<WaitKey,std::vector<WaitKey>> inputs;
  std::set<WaitKey> native;
  for (const auto &e : s.graph.edges) {
    if (e.kind == EdgeKind::native) { native.insert(e.from); continue; }
    if (e.kind != EdgeKind::wr && e.kind != EdgeKind::ww) continue;
    auto target=s.graph.states.find(e.to);
    if (target != s.graph.states.end() && target->second != State::committed)
      inputs[e.from].push_back(e.to);
  }
  std::map<WaitKey,unsigned> masks;
  std::map<WaitKey,std::vector<WaitKey>> consumers;
  std::deque<WaitKey> work;
  for (const auto &entry:s.graph.states) {
    auto key=entry.first; auto state=entry.second; unsigned mask=0;
    if (state==State::aborting || state==State::aborted) mask=ROLLBACK;
    else if (native.count(key)) mask=NATIVE;
    else if (state==State::active) mask=EXECUTION;
    else if (state==State::approved) {
      auto p=s.phases.find(key); unsigned phase=p==s.phases.end()?0:p->second;
      mask=phase==1?APPLY:phase==2?ENGINE:phase==3?FINISH:DISPATCH;
    } else if (state==State::ready) {
      if (!s.serialization_approved.count(key)) mask|=APPROVAL;
      for (auto next:inputs[key]) consumers[next].push_back(key);
      if (!mask && inputs[key].empty()) mask=DISPATCH;
    }
    masks[key]=mask; if (mask) work.push_back(key);
  }
  const auto propagate = [&] {
    while (!work.empty()) {
      auto key=work.front();work.pop_front();
      for (auto consumer:consumers[key]) {
        auto next=masks[consumer] | masks[key];
        if (next!=masks[consumer]) {masks[consumer]=next;work.push_back(consumer);}
      }
    }
  };
  propagate();
  for (const auto &entry:s.graph.states)
    if (entry.second==State::ready && !masks[entry.first]) {
      masks[entry.first]=CYCLE;work.push_back(entry.first);
    }
  propagate();
  for (const auto &entry:s.graph.states) {
    if (entry.second==State::approved) {
      auto p=s.phases.find(entry.first); unsigned phase=p==s.phases.end()?0:p->second;
      if (phase<4) ++out.approved[phase];
    }
    if (entry.second!=State::ready) continue;
    ++out.ready;
    const bool waiting=!inputs[entry.first].empty();
    const bool unapproved=!s.serialization_approved.count(entry.first);
    if (waiting) {
      ++out.value; if (unapproved) ++out.both;
      unsigned mask=0;
      for (auto next:inputs[entry.first]) mask|=masks[next];
      if (!mask) mask=UNKNOWN;
      if (mask & (mask-1)) ++out.mixed;
      for (size_t i=0;i<10;++i) {
        if (mask & (1u<<i)) ++out.marginal[i];
        if (mask == (1u<<i)) ++out.only[i];
      }
    } else if (unapproved) ++out.own;
    else ++out.runnable;
  }
  return out;
}
} // namespace rcc
#endif
