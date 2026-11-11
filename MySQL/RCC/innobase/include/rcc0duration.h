#ifndef rcc0duration_h
#define rcc0duration_h
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>
namespace rcc_duration {
inline const bool enabled = [] { const char *v=std::getenv("RCC_DURATION_PROFILE"); return v && std::strcmp(v,"1")==0; }();
inline const char *root_profile_path = std::getenv("RCC_ROOT_PROFILE_PATH");
inline const bool root_enabled = root_profile_path && *root_profile_path;
enum Phase : unsigned { outside, select_sql, update_sql, snapshot_read,
  speculative_write, retire_publish, rollback, rollback_prefix, metadata_finish,
  lock_release, certify, apply, native_wait, warehouse_wait, commit_wait,
  commit_body, phase_count };
template<class T> class PhaseScope {
 T *trx; unsigned previous;
 public:
 PhaseScope(T *t, Phase p):trx(root_enabled ? t : nullptr),previous(0) {
   if(trx) previous=trx->rcc_diag_phase.exchange(p,std::memory_order_relaxed);
 }
 ~PhaseScope() { if(trx) trx->rcc_diag_phase.store(previous,std::memory_order_relaxed); }
 void set(Phase p) { if(trx) trx->rcc_diag_phase.store(p,std::memory_order_relaxed); }
};
enum Metric { row_x, row_abort, row_s, row_other, row_success, x_retire,
              x_snapshot, x_write, abort_release, rollback_preamble,
              abort_cleanup, commit_cleanup, release_abort, release_commit,
              x_unretired, row_before_wake, row_wake_resume, row_resume_cleanup,
              row_wake_unclassified, row_warehouse, row_district, row_customer, row_stock, row_item, row_remaining, x_warehouse, x_district, x_stock, x_remaining, grant_warehouse, grant_district, grant_stock, grant_remaining, cause_unknown, cause_abort_elsewhere, cause_abort_blocker, cause_x_queued, cause_x_running, cause_s_queued, cause_s_running, cause_other, wh_grant_read, wh_read_snapshot, wh_between_sql, wh_update_retire, wh_matched_total, wh_descriptor_begin, wh_core_read, wh_core_write, wh_write_row_wait, wh_write_row_hold, wh_write_graph_wait, wh_write_graph_hold, wh_write_notify, wh_write_source_wait, wh_write_source_hold, wh_write_registry_wait, wh_write_registry_hold, metric_count };
inline constexpr uint64_t bounds[] = {10000,50000,100000,250000,500000,1000000,5000000,20000000};
struct Counter {
 std::atomic<uint64_t> count{0}, ns{0}, max{0};
 std::array<std::atomic<uint64_t>,9> bins{};
};
struct alignas(64) Cell { std::array<Counter,metric_count> metrics; };
inline std::mutex cells_mutex;
inline std::vector<std::unique_ptr<Cell>> cells;
inline Cell &local() {
 thread_local Cell *cell=[] {
  auto c=std::make_unique<Cell>(); auto *p=c.get();
  std::lock_guard<std::mutex> lock(cells_mutex); cells.push_back(std::move(c)); return p;
 }();
 return *cell;
}
inline bool is_table(const char *physical, const char *logical) {
 const size_t n=std::strlen(logical);
 return std::strncmp(physical,logical,n)==0 &&
        (physical[n]=='\0' || physical[n]=='#');
}
inline uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void add(Metric op,uint64_t ns,uint64_t count=1,bool histogram=true) {
 if (!enabled) return;
 auto &c=local().metrics[op];
 c.count.fetch_add(count,std::memory_order_relaxed); c.ns.fetch_add(ns,std::memory_order_relaxed);
 if (!histogram) return;
 if (ns>c.max.load(std::memory_order_relaxed)) c.max.store(ns,std::memory_order_relaxed);
 size_t b=0;while(b<8 && ns>bounds[b]) ++b;
 c.bins[b].fetch_add(1,std::memory_order_relaxed);
}
class Scope {
 Metric op; uint64_t start;
 public: explicit Scope(Metric m,bool active=true):op(m),start(enabled && active ? now():0) {}
 ~Scope() { if(start) add(op,now()-start); }
};
inline thread_local bool detail_write=false;
class WriteDetail {
 bool previous;
 public: explicit WriteDetail(bool active):previous(detail_write) { detail_write=enabled && active; }
 ~WriteDetail() { detail_write=previous; }
};
class DetailLock {
 std::unique_lock<std::mutex> lock;
 Metric wait_metric,hold_metric; uint64_t before=0,acquired=0;
 public:
 DetailLock(std::mutex &mutex,Metric w,Metric h):lock(mutex,std::defer_lock),wait_metric(w),hold_metric(h) {
   if(detail_write) before=now();
   lock.lock(); if(before) acquired=now();
 }
 ~DetailLock() {
   const auto end=before?now():0; lock.unlock();
   if(before) { add(wait_metric,acquired-before);add(hold_metric,end-acquired); }
 }
};
template<class T> inline void mark_abort(T *t) {
 if (!enabled) return; uint64_t zero=0;
 t->rcc_diag_abort_ns.compare_exchange_strong(zero,now(),std::memory_order_relaxed);
}
template<class T> inline void grant_x(T *t) {
 if (!enabled) return; uint64_t zero=0;
 t->rcc_diag_x_ns.compare_exchange_strong(zero,now(),std::memory_order_relaxed);
}
template<class T> inline void end_x(T *t,Metric op,Metric table=metric_count) {
 if (!enabled) return;
 const auto start=t->rcc_diag_x_ns.exchange(0,std::memory_order_relaxed);
 if(start) { const auto elapsed=now()-start; add(op,elapsed);
   if(table!=metric_count) add(table,elapsed); }
}
struct WarehouseTimeline {
 uintptr_t owner=0; uint64_t generation=0, x=0, read_begin=0, read_end=0, update=0;
};
inline thread_local WarehouseTimeline warehouse;
template<class T> class WarehouseSnapshot {
 bool record=false;
 public:
 WarehouseSnapshot(T *trx,const char *table) {
   if(!enabled || !is_table(table,"tpcc/warehouse")) return;
   const auto x=trx->rcc_diag_x_ns.load(std::memory_order_relaxed);
   if(!x) return;
   const auto gen=trx->version.load(std::memory_order_relaxed);
   const auto owner=reinterpret_cast<uintptr_t>(trx);
   if(warehouse.owner!=owner || warehouse.generation!=gen || warehouse.x!=x)
     warehouse={owner,gen,x,0,0,0};
   if(!warehouse.read_begin) { warehouse.read_begin=now(); record=true; }
 }
 ~WarehouseSnapshot() { if(record) warehouse.read_end=now(); }
};
template<class T> inline bool warehouse_matches(T *trx) {
 return warehouse.owner==reinterpret_cast<uintptr_t>(trx) &&
   warehouse.generation==trx->version.load(std::memory_order_relaxed) &&
   warehouse.x==trx->rcc_diag_x_ns.load(std::memory_order_relaxed);
}
template<class T> inline void warehouse_update(T *trx,const char *table) {
 if(enabled && is_table(table,"tpcc/warehouse") && warehouse_matches(trx) &&
    warehouse.read_end && !warehouse.update) warehouse.update=now();
}
template<class T> inline void warehouse_retire(T *trx,const char *table) {
 if(!enabled || !is_table(table,"tpcc/warehouse") || !warehouse_matches(trx)) return;
 const auto end=now(); const auto w=warehouse; warehouse={};
 if(!(w.x && w.x<=w.read_begin && w.read_begin<=w.read_end &&
      w.read_end<=w.update && w.update<=end)) return;
 add(wh_grant_read,w.read_begin-w.x); add(wh_read_snapshot,w.read_end-w.read_begin);
 add(wh_between_sql,w.update-w.read_end); add(wh_update_retire,end-w.update);
 add(wh_matched_total,end-w.x);
}
inline std::string snapshot(size_t metric) {
 if(!enabled) return "off";
 uint64_t n=0,ns=0,mx=0; std::array<uint64_t,9> bins{};
 std::lock_guard<std::mutex> lock(cells_mutex);
 for(auto &p:cells) { auto &c=p->metrics[metric];
  n+=c.count.load(std::memory_order_relaxed); ns+=c.ns.load(std::memory_order_relaxed);
  const auto m=c.max.load(std::memory_order_relaxed);if(m>mx)mx=m;
  for(size_t i=0;i<9;++i)bins[i]+=c.bins[i].load(std::memory_order_relaxed);
 }
 std::ostringstream out;out<<"count="<<n<<" ns="<<ns<<" max_ns="<<mx;
 for(size_t i=0;i<9;++i)out<<" b"<<i<<"="<<bins[i];
 return out.str();
}
} // namespace rcc_duration
#endif
