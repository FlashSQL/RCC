#include "rcc0duration.h"
#include "rcc0async.h"
#ifdef ASYNC
#include "dict0dict.h"
#include "rem0rec.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "lock0lock.h"
#include "rcc0as_profile.h"
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <cstdio>

namespace {
struct WitnessTrace {
  std::mutex mutex;
  FILE *file = nullptr;
  uint64_t count = 0;
  bool failed = false;
  WitnessTrace() {
    const char *path = std::getenv("RCC_WITNESS_PATH");
    if (path && *path) {
      file = std::fopen(path, "wx");
      if (file) std::fprintf(file, "RCC_EXACT_WITNESS_V1\n");
      else std::fprintf(stderr, "RCC witness: cannot create trace file\n");
    }
  }
  ~WitnessTrace() {
    if (file) {
      std::fprintf(file, "Z %llu %u\n", (unsigned long long)count, failed);
      std::fclose(file);
    }
  }
} witness_trace;
void trace_event(char kind, const std::array<uint64_t, 8> &v) {
  std::lock_guard<std::mutex> guard(witness_trace.mutex);
  auto *file = witness_trace.file;
  if (std::fprintf(file, "%c", kind) < 0) witness_trace.failed = true;
  for (auto value : v)
    if (std::fprintf(file, " %llu", (unsigned long long)value) < 0)
      witness_trace.failed = true;
  if (std::fputc('\n', file) == EOF) witness_trace.failed = true;
  ++witness_trace.count;
}
const bool profile_enabled = [] {
  const char *value = std::getenv("RCC_PROFILE");
  return value && std::strcmp(value, "1") == 0;
}();
std::atomic<uint64_t> retire_ns{0}, retire_count{0};
std::atomic<uint64_t> graph_wait_ns{0}, value_wait_ns{0};
thread_local uint64_t write_start_ns = 0;
uint64_t profile_now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
const bool abort_profile_enabled = [] {
  const char *value = std::getenv("RCC_ABORT_PROFILE");
  return value && std::strcmp(value, "1") == 0;
}();
const bool wait_profile_enabled = [] {
  const char *v = std::getenv("RCC_WAIT_PROFILE");
  return v && std::strcmp(v, "1") == 0;
}();
std::array<std::atomic<uint64_t>, 4> abort_without_descriptor{};
rcc::AbortCause engine_abort_cause(const trx_t *trx) {
  const auto hint = trx->rcc_engine_abort_hint.load(std::memory_order_relaxed);
  return hint ? static_cast<rcc::AbortCause>(hint) : rcc::AbortCause::external;
}
rcc::Registry registry{rcc_srd_notify,
                           profile_enabled, abort_profile_enabled,
                           witness_trace.file ? trace_event : nullptr};

rcc::WaitKey wait_key(const trx_t *trx) {
  return {reinterpret_cast<uintptr_t>(trx), trx->version.load()};
}

std::string row_key(const dict_index_t *index, const rec_t *rec,
                    const ulint *offsets) {
  std::string key;
  const uint64_t table = index->table->id;
  key.append(reinterpret_cast<const char *>(&table), sizeof(table));
  const ulint n = dict_index_get_n_unique(index);
  for (ulint i = 0; i < n; ++i) {
    ulint len;
    const auto *field = rec_get_nth_field(index, rec, offsets, i, &len);
    const uint64_t length = len;
    key.append(reinterpret_cast<const char *>(&length), sizeof(length));
    if (len != UNIV_SQL_NULL) key.append(reinterpret_cast<const char *>(field), len);
  }
  return key;
}
}  // namespace

std::string rcc_wait_sample() {
  if (!wait_profile_enabled) return "off";
  return rcc::summarize_waits(registry.observe_waits()).str();
}
void rcc_profile_phase(trx_t *trx, unsigned phase) {
  if (wait_profile_enabled && trx->rcc_trx) registry.profile_phase(wait_key(trx), phase);
}

void rcc_note_engine_abort(trx_t *trx, rcc::AbortCause cause) {
  if (!abort_profile_enabled) return;
  uint8_t expected = 0;
  trx->rcc_engine_abort_hint.compare_exchange_strong(
      expected, static_cast<uint8_t>(cause), std::memory_order_relaxed);
}
uint64_t rcc_profile_value(size_t index) {
  if (index == 173 || index == 174) return registry.per_trx_graph();
  if (index == 168) return graph_wait_ns.load(std::memory_order_relaxed);
  if (index == 169) return value_wait_ns.load(std::memory_order_relaxed);
  if (index == 37 || index == 38 || index == 39 || (index >= 160 && index <= 172)) {
    const auto stats = registry.graph_metrics();
    switch (index) {
      case 170: return stats.precheck_reused;
      case 171: return stats.precheck_missed;
      case 172: return stats.precheck_invalidated;
      case 37: return stats.scans;
      case 38: return stats.serial_victims;
      case 39: return stats.mixed_victims;
      case 160: return stats.nodes;
      case 161: return stats.edges;
      case 162: return stats.peak_nodes;
      case 163: return stats.peak_edges;
      case 164: return stats.reclaimed;
      case 165: return stats.approvals;
      case 166: return stats.stale;
      default: return 1;
    }
  }
  if (index >= 33 && index < 37)
    return abort_without_descriptor[index - 33].load(std::memory_order_relaxed);
  if (index == 31) return retire_count.load(std::memory_order_relaxed);
  if (index == 32) return retire_ns.load(std::memory_order_relaxed);
  return registry.profile_value(index);
}
void rcc_profile_retired() {
  if (!profile_enabled || !write_start_ns) return;
  retire_ns.fetch_add(profile_now() - write_start_ns, std::memory_order_relaxed);
  retire_count.fetch_add(1, std::memory_order_relaxed);
  write_start_ns = 0;
}
bool rcc_table(const dict_index_t *index) {
  return index->is_clustered() && !index->table->is_temporary() &&
         !index->table->is_intrinsic() &&
         std::strncmp(index->table_name, "mysql/", 6) != 0;
}

rec_t *rcc_read(trx_t *trx, dict_index_t *index, const rec_t *rec,
                    const ulint *offsets, conflict_t *conf) {
  if (!rcc_table(index)) return nullptr;
  if (conf == nullptr) {
    ib::warn() << "RCC READ missing conflict snapshot: table=" << index->table_name
               << " trx=" << trx->id;
    rcc_duration::mark_abort(trx);
    trx->conflict_abort = TRX_ABORT_MARKED;
    trx->abort = true;
    trx->error_state = DB_DEADLOCK;
    return nullptr;
  }
  const bool wh_first = rcc_duration::enabled &&
      rcc_duration::is_table(index->table_name, "tpcc/warehouse") &&
      rcc_duration::warehouse_matches(trx) && !rcc_duration::warehouse.read_end;
  if (!trx->rcc_trx) {
    rcc_duration::Scope begin_time(rcc_duration::wh_descriptor_begin, wh_first);
    trx->rcc_trx = registry.begin(wait_key(trx));
  }
  struct CopyContext { trx_t *trx; conflict_t *conf; } context{trx, conf};
  const auto copy = [](void *opaque, const unsigned char *data, size_t size,
                       size_t extra) -> unsigned char * {
    auto &c = *static_cast<CopyContext *>(opaque);
    if (c.conf->future_vers == nullptr) {
      auto *buffer = static_cast<byte *>(mem_heap_alloc(c.trx->conflict_heap, size));
      memcpy(buffer, data, size);
      c.conf->future_vers = buffer + extra;
    }
    return c.conf->future_vers - extra;
  };
  const ulint extra = rec_offs_extra_size(offsets);
  rcc::Snapshot snapshot;
  {
    rcc_duration::Scope read_time(rcc_duration::wh_core_read, wh_first);
    snapshot = registry.read(trx->rcc_trx, row_key(index, rec, offsets),
                             rec - extra, rec_offs_size(offsets), extra, copy, &context);
  }
  if (!snapshot) {
    rcc_duration::mark_abort(trx);
    trx->conflict_abort = TRX_ABORT_MARKED;
    trx->abort = true;
    trx->error_state = DB_DEADLOCK;
    return nullptr;
  }
  return const_cast<rec_t *>(snapshot.data() + snapshot.extra);
}

dberr_t rcc_write(trx_t *trx, dict_index_t *index, const rec_t *physical,
                       const rec_t *future, const ulint *offsets,
                       void (*mutate)(void *), void *context) {
  if (!rcc_table(index)) { mutate(context); return DB_SUCCESS; }
  if (!trx->rcc_trx) {
    ib::warn() << "RCC UPDATE missing transaction descriptor: table=" << index->table_name
               << " trx=" << trx->id;
    rcc_duration::mark_abort(trx);
    trx->conflict_abort = TRX_ABORT_MARKED;
    trx->abort = true;
    trx->error_state = DB_DEADLOCK;
    return DB_DEADLOCK;
  }
  if (profile_enabled) write_start_ns = profile_now();
  const auto extra = rec_offs_extra_size(offsets);
  bool written;
  {
    rcc_duration::Scope write_time(rcc_duration::wh_core_write,
        rcc_duration::enabled && rcc_duration::is_table(index->table_name, "tpcc/warehouse"));
    rcc_duration::WriteDetail detail(rcc_duration::enabled &&
        rcc_duration::is_table(index->table_name, "tpcc/warehouse"));
    written = registry.write(trx->rcc_trx, row_key(index, physical, offsets),
                              future - extra, rec_offs_size(offsets), extra,
                              true, mutate, context);
  }
  if (!written) {
    write_start_ns = 0;
    rcc_duration::mark_abort(trx);
    trx->conflict_abort = TRX_ABORT_MARKED;
    trx->abort = true;
    return DB_DEADLOCK;
  }
  return DB_SUCCESS;
}

dberr_t rcc_certify(trx_t *trx) {
  rcc_duration::PhaseScope phase(trx, rcc_duration::certify);
  if (!trx->rcc_trx) return DB_SUCCESS;
  const auto deadline = std::chrono::steady_clock::now() + trx_lock_wait_timeout_get(trx);
  for (;;) {
    trx_mutex_enter(trx);
    const bool canceled = trx->conflict_abort != TRX_NOT_ABORTED || trx->abort;
    trx_mutex_exit(trx);
    if (canceled) { registry.abort(trx->rcc_trx, engine_abort_cause(trx)); return DB_DEADLOCK; }
    const auto result = registry.certify(trx->rcc_trx);
    if (result == rcc::Decision::accepted) return DB_SUCCESS;
    if (result == rcc::Decision::aborted) return DB_DEADLOCK;
    if (std::chrono::steady_clock::now() >= deadline) {
      registry.abort(trx->rcc_trx, rcc::AbortCause::timeout);
      return DB_LOCK_WAIT_TIMEOUT;
    }
    const auto wait_start = std::chrono::steady_clock::now();
    const bool graph_wait = registry.wait_tick(trx->rcc_trx);
    const auto elapsed = std::chrono::steady_clock::now() - wait_start;
    trx->lock_commit_time += std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    (graph_wait ? graph_wait_ns : value_wait_ns).fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(),
        std::memory_order_relaxed);
  }
}
void rcc_abort(trx_t *trx) { registry.abort(trx->rcc_trx, engine_abort_cause(trx)); }
void rcc_finish(trx_t *trx, bool committed) {
  rcc_duration::PhaseScope phase(trx, rcc_duration::metadata_finish);
  rcc_profile_phase(trx, 3);
  if (!trx->rcc_trx) {
    if (!committed && abort_profile_enabled) {
      const auto cause = engine_abort_cause(trx);
      const size_t bucket = cause == rcc::AbortCause::engine_cascade ? 0 :
          cause == rcc::AbortCause::native_deadlock ? 1 :
          cause == rcc::AbortCause::engine_reupdate ? 2 : 3;
      ++abort_without_descriptor[bucket];
    }
    return;
  }
  registry.finish(trx->rcc_trx, committed);
  trx->rcc_trx.reset();
}
void rcc_reset(trx_t *trx) {
  if (trx->rcc_trx) rcc_finish(trx, false);
}
void rcc_srd_detect() { registry.detect_async(); }
std::vector<rcc::WaitKey> rcc_take_aborts() {
  return registry.drain_engine_aborts();
}
#endif
