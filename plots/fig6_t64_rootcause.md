# Fig6 (PostgreSQL): why RCC throughput collapses past 32 client threads

TPC-C, 1 warehouse, NewOrder + Payment, RR isolation.
Analysis date 2026-09-19. Machine: 2-socket AMD EPYC, 256 cores, 2 NUMA nodes, 3 TB RAM.

---

## 1. The measurement

`plots/plot_fig6_pg.sh` → `fig6_varying_client_threads/postgres/varying_clients_threads_pg.{pdf,png}`

| threads | 1 | 8 | 16 | **32** | 64 | 128 | 256 |
|---|---|---|---|---|---|---|---|
| VAN-RC | 1585 | 2132 | 2376 | 1813 | 1475 | 977 | 732 |
| VAN-RR | 1562 | 3502 | 3528 | 2826 | 2112 | 1332 | 732 |
| **RCC-RR** | 1556 | 8628 | 13515 | **18664** | 11602 | 4040 | 2182 |

Peak at t32, then −38% at t64, −78% at t128, −88% at t256.

Properties of the data that constrain any explanation:

- **TPS is flat across the 50 s window at every thread count.** Not bloat, not vacuum, not
  any time-accumulating effect.
- **TPS × avg-latency = N exactly** at every point. The system is saturated everywhere; the
  question is only why the bottleneck's service rate falls as N rises.
- **`TPS × N` is flat** (597 K / 743 K / 517 K / 559 K at t32–t256), i.e. TPS ∝ 1/N — the
  signature of per-operation cost growing linearly with concurrency.
- **queries/txn is flat at ~29** across all thread levels. The client asks for the same work;
  the server does more of it.

---

## 2. Everything is one row

Throughput is pinned to the update rate of the single `warehouse1_w1` row. Using
`n_tup_upd(warehouse1_w1)` from `hot_stats.txt` and the payment fraction `p` from the
`ABORTSTATS` lines in `sysbench.log`:

| N | 8 | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|---|
| predicted TPS = rate/p | 8629 | 13516 | 18666 | 11605 | 4044 | 2190 |
| observed TPS | 8628 | 13515 | 18664 | 11602 | 4040 | 2182 |

Max deviation 0.4%. Every Payment does `SELECT … FOR UPDATE` + `UPDATE w_ytd` on that one row
(`sysbench-tpcc/tpcc_run.lua:332-339`).

**The serial interval W of that row** (elapsed / n_tup_upd):

| N | 1 | 8 | 16 | **32** | 64 | 128 | 256 |
|---|---|---|---|---|---|---|---|
| W (µs) | 1283 | 227 | 141 | **98** | 165 | 471 | 813 |
| of which spin (µs) | 0 | 11 | 25 | 63 | 140 | 388 | 620 |
| spin share of W | – | 5% | 17% | 64% | **85%** | 82% | 76% |

At t32 the row's lane runs at **99.8% utilisation** (capacity 1/98 µs = 9330 payments/s vs
9325/s measured). That is the knee. Past it, a fixed-capacity serial resource would give a
**flat plateau at 18.6 K** with linearly growing latency.

It does not. W itself degrades 98 → 165 → 471 → 813 µs. **That degradation is the entire
collapse**, and it is not inherent.

---

## 3. Root cause: the hot row's lock is held across a client round trip

`tpcc_run.lua:332-339` sends two separate statements:

```lua
con:query_row("SELECT w_street_1, … FROM warehouse1 WHERE w_id = 1 FOR UPDATE")  -- ①
con:query    ("UPDATE warehouse1 SET w_ytd = w_ytd + %d WHERE w_id = 1")          -- ②
```

A pure locker installs **no new version**: after ① the row has xmax set,
`HEAP_XMAX_LOCK_ONLY` set, and **`t_ctid == t_self`**.

The successor acquires the heavyweight tuple lock and then spins
(`heapam.c:6084` acquire → `:6110` spin → `:6511` release; same shape in `heap_update` at
3592 / 3601 / 4377):

```c
while (TransactionIdIsInProgress(xwait) &&
       ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
{
    CHECK_FOR_INTERRUPTS();
    // pg_usleep(10L);   <-- commented out: pure busy spin, no backoff
}
```

Neither clause can go false until the predecessor's **next statement arrives from the wire**.
So the row's serial lane is occupied for the duration of a client-side event, not a quantity
of database work.

The confirming measurement: **W / NewOrder-latency is flat** across an 8× concurrency range and
an 8.6× throughput collapse. (NewOrder never touches the warehouse row, so its latency is an
independent proxy for "how long a generic transaction takes here".)

| N | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|
| rcc_rr | 0.111 | 0.069* | 0.109 | 0.114 | 0.120 |
| vanilla_rr | 0.313 | 0.306 | 0.300 | 0.298 | 0.215 |
| vanilla_rc | 0.385 | 0.396 | 0.424 | 0.392 | 0.405 |

\* t32 is the one off-point because the row is not yet saturated there.

**W is ~11% of end-to-end transaction latency, in every build, at every N.** That closes a
positive feedback loop:

```
more waiters → latency ↑ → W ↑ → fewer handoffs/s → more waiters
```

which is exactly why the curve collapses instead of plateauing.

**RCC's real achievement, stated correctly:** it drops that constant from 0.30–0.40 (vanilla)
to 0.11 — a 3–4× shorter critical section, which is where the 6.6× peak advantage comes from.
It converts "wait for the predecessor's whole transaction" into "wait for one client round trip
inside the predecessor's transaction". It does **not** break the coupling.

### Split of the t128 loss

| | TPS | note |
|---|---|---|
| inherent (single-row HOT update, page in `shared_buffers`, `synchronous_commit=off`) | ~130–380 K | estimate, not measured |
| structural ceiling keeping the two-round-trip SFU→UPDATE shape (1 round trip ≈ 21.4 µs, measured at t1) | ~90 K | |
| measured peak (t32) | 18.6 K | already 4.6× below structural |
| measured t128 | 4.0 K | **78.4% of the t32 plateau lost, all of it non-inherent** |

---

## 4. Contributing factors (ranked, measured where possible)

### #1 Lock held across the client round trip — **the cause**
Above. Spin accounts for **557 µs of the 715 µs (78%)** of W's growth from t32 to t256.
`spin_wait_ms` is 32.1 / 42.4 / 41.2 / 38.3 s per 50 s window at t32–t256, i.e. **0.64–0.85 of
a single serialized slot permanently occupied**. Against total backend-seconds that is 0.3–2%,
which is why "spin is small in absolute ms" is the wrong reading — measure it against the one
serial resource, not against all backends.

### #2 Chain-walk length ∝ N
Under RR the UPDATE's scan lands on the snapshot's committed version and walks to the tip; the
number of in-flight speculative versions on the row is ≈ 0.52 N (Little's law) → 16 hops at t32,
128 at t256. The authors' own comment estimates the same order
(`heapam_handler.c:400-404`: *"~20 hops under contention, each a hot-page buffer lock"*).
Consistent with `exec/commit` growing **exactly linearly in N** (0.889 → 1.324 → 4.054 → 6.887 ms
for t32–t256, N^1.0) while aggregate Execution stays flat at ~800 s per 50 s window — backends
do ~8× more work per transaction, and the per-hop buffer content lock is uninstrumented so it
lands in the Execution residual.

This is real, but see §5: attacking it directly does not help, because the hot path is the SFU,
not the UPDATE.

### #3 `deadlock_timeout = 1` (one millisecond; default 1000)
Mean tuplock wait is 0.219 ms/commit at t32 and 3.438 ms at t64 — the 1 ms threshold falls
exactly in the gap where TPS first collapses, so essentially 100% of hot-row waits above t32
trigger `CheckDeadLock()`, which takes **all 16 lock-manager partition locks exclusively**.
The logs show only 3 real deadlocks across all four runs. Nothing in RCC requires it
(`ProcSleep` is untouched by RCC). **Not measured in the A/B below** — worth a one-line test.

### #4 Commit-time cycle detection (`rcc_bg_detect` off)
With the detector off, every commit runs `RccCommitCycleCheck`, which takes all 64 dependency
partition locks `LW_SHARED` and DFSes the graph (`rcc.c:439-440`), while `RccAddReverseEdge`
needs one of them `LW_EXCLUSIVE` from inside the hot-row tuple lock (`heapam.c:4300`).
**Measured: +1.8% at t64** — the mechanism works (commit wait 0.456 → 0.329 ms/commit, −28%)
but commit wait is not the t64 bottleneck.

---

## 5. What was tried, and what happened

Server started manually (no `numactl`), patched build, 1 warehouse, t64, 50 s warmup + 50 s
measured, fresh DB load per arm. Raw data: per-arm `sysbench.log` / `breakdown.txt`.

| arm | `rcc_bg_detect` | superseded skip | inflight skip | TPS | vs base | tuplock/commit | exec/commit | commit wait |
|---|---|---|---|---|---|---|---|---|
| baseline | off | off | off | **11,767** | — | 3.350 ms | 1.342 ms | 0.456 ms |
| bg | **on** | off | off | 11,984 | **+1.8%** | 3.465 | 1.282 | 0.329 |
| sup | on | **on** | off | 10,409 | **−11.5%** | 4.069 | 1.474 | 0.328 |
| inf | on | on | **on** | 10,766 | **−8.5%** | 4.084 | 1.360 | 0.246 |

Baseline reproduces the published t64 (11,602) to within 1.4% — **without NUMA pinning**, which
also rules out CPU oversubscription as the t64 mechanism.

### The failed patch and why it failed

The patch elided the heavyweight tuple lock in `heap_lock_tuple` on chain-walk interior versions,
guarded by:

```c
!ItemPointerEquals(&tuple->t_self, &t_ctid) &&
!HEAP_XMAX_IS_LOCKED_ONLY(infomask)
```

It targeted #2 (chain-walk hops). But the dominant hot path is ①, the `SELECT … FOR UPDATE`,
where a pure locker leaves `t_ctid == t_self` **and** sets `HEAP_XMAX_LOCK_ONLY` — so both guards
are false and **the elision cannot fire on the hot path at all**. Where it did fire (other
relations' chains) it removed the FIFO ordering that `LockTuple` exists to provide, and tuplock
wait went *up* (3.350 → 4.069 ms/commit).

**Rolled back.** Only `rcc_bg_detect = on` remains, in `PostgreSQL/postgresql.conf`.

Lesson: the chain-walk O(N²) analysis is correct as arithmetic, but it describes the UPDATE
path, and this workload's contention lives in the SFU that precedes it.

---

## 6. What to do next

**The fix has to shorten the window between ① and ②, or stop holding the row's lock across it.**
In increasing order of payoff:

1. **Conjunct order** (one line, zero risk) — swap to
   `ItemPointerEquals(...) && TransactionIdIsInProgress(xwait)` at `heapam.c:3601` and `:6110`.
   Removes a `ProcArrayLock` shared acquire + proc-array scan from every spin iteration. Helps
   the `heap_update` path only; does not shorten W on the SFU path.
2. **Bounded spin + backoff** — restore the commented-out `pg_usleep` (`heapam.c:3648`) as a
   bounded spin then latch wait, woken by the version installer. Saves CPU and `ProcArrayLock`
   traffic; does **not** shorten W.
3. **Release the tuple lock before the spin, re-acquire after.** Stops holding the serial
   resource across a client round trip. Smaller change, but redistributes the wait rather than
   removing it, and gives up FIFO fairness — which the A/B above shows is not free.
4. **Make RCC's speculative versioning cover `SELECT … FOR UPDATE`.** Today speculation only
   fires on `heap_update`, so the one access pattern that dominates this workload gets zero
   relief. If ① installed a speculative lock-only version, the successor's escape predicate
   would fire immediately instead of waiting for ② to arrive from the client. **This is where
   the remaining 5–12× lives.**
5. **Exploit commutativity.** ② is `w_ytd = w_ytd + h_amount` — a commutative increment with no
   true data dependency. RCC's redo-log design is already shaped for this; a delta/escrow path
   for such updates removes the serialization entirely rather than shortening it.

**Cheapest decisive tests, in order:**

- `deadlock_timeout = 1s` at t64 only (one config line, §4 #3 is untested).
- Add two counters to the spin loop: exits via ctid-change vs via xact-end. Settles how much of
  W is install-wait vs commit-wait without any code risk.
- Then #4 above.

---

## 7. Measurement issues found along the way (independent of the above)

1. **The vanilla breakdown counters are all zero.** The `xact_count_*` instrumentation lives only
   in the RCC build, so `lock_wait_ms` / `spin_wait_ms` / `commit_wait_ms` are `0` in all 14
   `vanilla_*/breakdown.txt` files. Vanilla and RCC time breakdowns must not be compared
   side by side.
2. **`--backoff_on=0`** (`run_tpcc_threads.sh:210`) makes the vanilla-RR baseline a zero-backoff
   retry storm: at t32 it commits 140,737 and rolls back **2,706,357** (95% abort), with only
   149 s of the 1524 s total on the committed path. The headline RCC-vs-VAN-RR speedup is
   inflated by this.
3. **The harness changes NUMA policy mid-curve.** `NUMA_PIN_MAX_THREADS=128`
   (`run_tpcc_threads.sh:23-33`) pins server *and* client to node 0 up to t128 and switches to
   `--interleave=all` at t256, so the last point is not an apples-to-apples concurrency step.
   The comment says "2 sockets x 48 logical CPUs", which is not this machine (2 × 128).
   With server and client co-located, t128 is 256 threads on 128 CPUs — 2× oversubscribed.
   Note this does **not** explain t64: the A/B above reproduces the t64 collapse with no pinning
   at all.
