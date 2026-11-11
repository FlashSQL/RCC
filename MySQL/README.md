# MySQL implementations

`RCC/innobase/` contains the shared MySQL 8.4.5 InnoDB source for RCC and RCC-C.
RCC is the default; `WITH_RCC_C=ON` enables RCC-C.

`Bamboo/innobase/` contains the shared Bamboo and Rebirth (Rebirth-Retire)
source imported from the current ShadowTX8_BB working tree. Bamboo is the
default for this source tree; `WITH_REBIRTH=ON` enables Rebirth.

| Implementation | CMake option | Source overlay | Build directory |
| --- | --- | --- | --- |
| RCC | `-DWITH_RCC_C=OFF` (default) | `RCC/innobase/` | `RCC/mysql-8.4.5-build/` |
| RCC-C | `-DWITH_RCC_C=ON` | `RCC/innobase/` | `RCC/mysql-8.4.5-build-C/` |
| Bamboo | `-DWITH_REBIRTH=OFF` (default) | `Bamboo/innobase/` | `Bamboo/mysql-8.4.5-build/` |
| Rebirth | `-DWITH_REBIRTH=ON` | `Bamboo/innobase/` | `Bamboo/mysql-8.4.5-build-RR/` |
| Vanilla MySQL | Unmodified upstream source | None | `Vanilla/mysql-8.4.5-build/` |

## Build

From this directory, with the MySQL 8.4 build dependencies installed:

```bash
bash run_server.sh --rcc --build-only
bash run_server.sh --rcc-c --build-only
bash run_server.sh --bamboo --build-only
bash run_server.sh --rebirth --build-only
bash run_server.sh --vanilla --build-only
```

These commands download MySQL 8.4.5 when needed, refresh the InnoDB overlay,
configure, build and install. They do not start a server or access data/log
directories. The default compilers are `gcc-10` and `g++-10`; set `CC` and
`CXX` to use another supported compiler (GCC 10 or newer). Set `BUILD_JOBS`
to control build parallelism. RCC and RCC-C share
`RCC/mysql-8.4.5/`; build them sequentially. Their build and install outputs
are separate (`-build`/`-install` versus `-build-C`/`-install-C`). Launchers use
the installed executables in `mysql-8.4.5-install[-C]/bin/`. Similarly,
Bamboo and Rebirth share `Bamboo/mysql-8.4.5/` and should be built sequentially.
Their output suffixes are `-build`/`-install` and `-build-RR`/`-install-RR`.
Rebirth uses the same `Bamboo/innobase/` overlay; no separate Rebirth source
directory is needed.

For a manual build, replace a MySQL 8.4.5 source tree's `storage/innobase/`
with `RCC/innobase/` or `Bamboo/innobase/`, then configure separate build
directories using that source tree's options above.

## Paper experiments (Fig. 4, 5 and 6)

The legacy Python synthetic benchmark and Java YCSB benchmark were removed.
The current clients are the YCSB-like Lua workload copied from
`ShadowTX8/sysbench_ycsb/` into `sysbench/`, and the current source/schema from
`/home/hjin/tpcc-mysql` in `tpcc-mysql/`. Source working-tree changes are included;
old binaries and old benchmark launchers are not imported.

| Figure / executable | Workload and fixed settings | Sweep | Default engines |
| --- | --- | --- | --- |
| `scripts/fig4_sysbench_threads.sh` | sysbench YCSB: 10M rows, Zipf theta 0.9, ascending key order, 8 reads + 8 non-index updates per transaction | 1, 16, 32, 64, 96, 128 threads | Vanilla, Bamboo, Rebirth, RCC |
| `scripts/fig5_tpcc_threads.sh` | TPC-C: 1 warehouse, 100K items, NewOrder:Payment = 1:1; no other transaction types | 1, 16, 32, 64, 96, 128 threads | Vanilla, Bamboo, Rebirth, RCC, RCC-C |
| `scripts/fig6_sysbench_rows.sh` | sysbench YCSB: one 10M-row table, Zipf theta 0.9, ascending key order, 8 reads + 8 non-index updates, 64 threads | Access key range 10M, 100K, 10K, 1K, 0.1K (100 keys) | Bamboo, Rebirth, RCC |

All use **SERIALIZABLE**, **30 seconds of warmup**, and **30 seconds of measured
execution**, once per point. Default engines follow the existing figure
templates. `--engines` overrides them, including RCC-C for sysbench if desired.
Fig. 4 has 24 runs, Fig. 5 has 30 runs, and Fig. 6 has 15 runs by default.
Fig. 6 does not shrink the table: `--table-size` controls the Lua key generator's
range during `run`. Fig. 4 and Fig. 6 restore the same 10M-row snapshot.

### 1. Build prerequisites and clients

Build the required engine installations using `run_server.sh --build-only`
as above. The experiment scripts use those installations and never rebuild
engines automatically. Python 3.8+, `make`, a C compiler, and `numactl` are
required. Use `--numa none` where NUMA binding is unavailable; otherwise all
available CPUs are retained and server memory is interleaved across NUMA nodes.

Use sysbench with Zipfian support. The validated source revision is
`3ceba0b1e115f8c50d1d045a4574d8ed643bd497` (sysbench 1.1).
On a new machine, install Autoconf, Automake, Libtool and pkg-config, then build
it against the same Vanilla MySQL installation used for loading:

```bash
cd /home/hjin/RCC/MySQL
MYSQL_PREFIX="$PWD/Vanilla/mysql-8.4.5-install"
# Or: MYSQL_PREFIX=/home/hjin/mysql-8.4.5-install
git clone https://github.com/akopytov/sysbench.git tools/sysbench
git -C tools/sysbench checkout 3ceba0b1e115f8c50d1d045a4574d8ed643bd497
(
  cd tools/sysbench
  ./autogen.sh
  ./configure --with-mysql-includes="$MYSQL_PREFIX/include" \
    --with-mysql-libs="$MYSQL_PREFIX/lib" --without-pgsql
  make -j8
)
```

The runner looks for `tools/sysbench/src/sysbench`, then
`$HOME/sysbench/src/sysbench`, then `sysbench` on `PATH`. A binary without
`--rand-zipfian-exp` is rejected. Override it with `--sysbench /path/to/sysbench`.

```bash
cd /home/hjin/RCC/MySQL
bash scripts/build_clients.sh
```

This builds `tpcc-mysql/tpcc_load` and `tpcc-mysql/tpcc_start` against
`Vanilla/mysql-8.4.5-install/bin/mysql_config`. To use an existing Vanilla
installation instead:

```bash
bash scripts/build_clients.sh --mysql-config /home/hjin/mysql-8.4.5-install/bin/mysql_config
# Pass --loader-prefix /home/hjin/mysql-8.4.5-install to the loaders and runs below.
```

`--loader-prefix` supplies the Vanilla server, MySQL client tools, shared client
libraries, and the Vanilla baseline binary. Modified engines use the RCC/Bamboo
installations listed above. The imported TPC-C client already selects
`seq_init(1,1,0,0,0)`; its `-0` through `-4` options are latency limits, not a
transaction-mix selector. Standard NewOrder rollback behavior is preserved;
extra random abort injection is disabled with `-a 0`.

### 2. Load data once

```bash
bash scripts/load_sysbench_data.sh    # shared 10M-row snapshot for Fig. 4 and 6
bash scripts/load_tpcc_data.sh        # 1 warehouse / 100K items for Fig. 5
```

The loaders initialize their own Vanilla server, create/load the schema,
validate row counts, and shut it down cleanly before publishing a reusable
snapshot. Matching snapshots are reused. No existing user database is needed.
TPC-C also runs the seven active checks in `tpcc-mysql/consistency_check.sql`
after loading and after every measured run. A failed check stops the run;
the output is retained as `consistency.tsv`. After measurement, the engine
shuts down fully and a separate Vanilla server checks the persisted rows in
an explicit SERIALIZABLE transaction. Validation is outside the measured time;
its server logs are in `consistency-check/`.
Sysbench uses HASH(id) with 128 partitions, as in the ShadowTX8 loader; set
`--partitions 0` on both loading and running to disable partitioning. TPC-C uses
the imported schema unchanged, including its district partitioning and indexes.

Snapshots and scratch data live under `benchmark-data/`. To place them on
another disk, pass the same `--work-dir /mnt/rcc-bench` to loaders and runs.
Each measured case restores a fresh copy, starts its own server on a private
socket and loopback port, and stops only that server. Successful scratch copies
are removed; failed copies/logs are retained for inspection. Existing snapshots
are never overwritten. A schema/page-size mismatch requires a newly loaded
snapshot in another work directory.

The experiment runner does not call the older `run_server.sh` startup path and
does not use `rcc_data_dir`, `rcc_log_dir`, `/tmp/mysql.sock`, or global `pkill`.
The original `my.cnf` is a tuning template: per-run copies override data/log/
socket paths and force SERIALIZABLE, without rewriting the template. The default
buffer pool remains 32GB and page size 4KB; `--config /path/to/my.cnf` can select
another common configuration.

### 3. Inspect and run the plans

```bash
bash scripts/fig4_sysbench_threads.sh --dry-run
bash scripts/fig5_tpcc_threads.sh --dry-run
bash scripts/fig6_sysbench_rows.sh --dry-run

bash scripts/fig4_sysbench_threads.sh
bash scripts/fig5_tpcc_threads.sh
bash scripts/fig6_sysbench_rows.sh

# Load missing snapshots automatically, then run all three figures:
bash run_all_experiments.sh --prepare
```

`--dry-run` prints every case without creating files, checking/installing
binaries, loading data, or starting/stopping servers. `--prepare` loads missing
snapshots before running; without it, missing data stops the run with a loader
instruction.

Useful options (pass to any figure script):

| Option | Effect |
| --- | --- |
| `--engines Bamboo,Rebirth,RCC` | Select engine names and order |
| `--threads 1,16,32,64,96,128` | Override thread list; Fig. 6 defaults to 64 |
| `--rows 10M,100K,10K,1K,0.1K` | Sysbench access-range list; Fig. 4 defaults to 10M |
| `--warmup 30 --runtime 30` | Warmup and measured seconds |
| `--repeat 2` / `--paper` | Two repetitions / three repetitions, like DBx1000 |
| `--results-dir /path/to/results` | Parent directory for timestamped results |
| `--work-dir /path/to/data` | Shared snapshot and scratch directory |
| `--loader-prefix /path/to/vanilla-install` | Existing Vanilla tools/installation |
| `--sysbench /path/to/sysbench` | Zipfian-capable sysbench executable |
| `--numa interleave` / `--numa none` | Server memory placement |

`--row-mode range` is the default and the requested Fig. 6 setup. An optional
`--row-mode table` supports a separate actual-table-size experiment; use it on
both loading and running. It is not the default paper experiment.

Example of one selected point, with automatic data preparation:

```bash
bash scripts/fig5_tpcc_threads.sh --engines RCC --threads 64 --prepare
```

Warmup and measurement are separate client invocations against the same running
server. Warmup output is saved but excluded from the summary. Sysbench preloads
the full 10M-row table through explicit SERIALIZABLE transactions before
warmup. TPC-C statistics are refreshed before warmup. The sysbench abort buffer
and TPC-C retry backoff follow the imported reference clients and are recorded
in `run.json`.

### 4. Results

Each invocation writes a new directory such as:

```text
results/Fig6/20261111_190000_12345/
  plan.json
  versions.json
  summary.csv
  RCC_rows100_t64_r1/
    run.json
    my.cnf
    runtime.tsv
    placement.txt
    server-error.log
    warmup.out / warmup.err
    run.out / run.err
    run.command.json
    status_before.tsv / status_after.tsv
    consistency.tsv                 # TPC-C only
    summary.json
```

`summary.csv` records engine, access rows, actual table rows, threads, repetition,
TPS, completed transactions (`commits`), aborts, abort fraction (0–1), successful
attempt time, aborted attempt time, and server lock/commit wait deltas in ms.
TPC-C completions use the per-thread `sc + lt` totals for NewOrder and Payment,
not tpmC. Final attempt-time totals are also summed from per-thread counters
after the workers finish. The runner requires every requested worker to finish
successfully. Rebuild the TPC-C client with `scripts/build_clients.sh` after
updating its source. Failed/partial clients and unexpected transaction types
stop the run instead of adding a successful result row.

Successful/aborted attempt times are sums of client-observed wall times, not CPU
time. Wait counters overlap successful attempt time and must not simply be
added to it. Server counter snapshots bracket the separate measurement client,
so they include its connection/drain interval; the 30-second warmup is excluded.
A missing custom counter is left empty, rather than reported as a measured zero.
Vanilla's lock counter scope can differ from the custom engines' successful-
attempt counters. Original logs and counters are retained for Fig. 4/5 time
breakdown analysis. The static gnuplot templates do not directly consume this
new CSV format.

### Offline verification

```bash
python3 -m unittest discover -s scripts/tests -v
```

These checks cover the requested plans, warmup exclusion, log parsing, clean
snapshot publication and restoration without running a database benchmark.
The source import and script checks do not establish engine correctness or
new performance results.

### Integration verification (2026-11-11)

The local review passed 15 offline tests, all 69 default plan entries, C++ syntax
checks for 152 changed translation units across all four modified engine modes,
a complete RCC `mysqld` build/link, and fresh TPC-C/sysbench client builds.
Both reusable datasets were loaded: 10M rows with 128 partitions, and TPC-C with
one warehouse and 100K items.

Executed checks included Vanilla Fig. 4 at one thread, RCC Fig. 6 at 64 threads
with a 100-key access range in the 10M table, and TPC-C at 16 threads. RCC TPC-C
also completed a 30-second warmup and 30-second measurement; its persisted data
passed all seven consistency checks in Vanilla. The final automatic validation
path was rerun successfully with a short TPC-C case.

These are functional checks, not paper performance results. Sysbench integration
checks used RAM-backed scratch storage with `innodb_flush_method=fsync` after
the disk-based partition conversion hit the previous SQL timeout; loading DDL
now has a separate two-hour limit. The complete 69-case measurement matrix and
full server builds/runs for RCC-C, Bamboo and Rebirth remain to be verified.
