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

## First checkout: build, load, and run Fig. 4/5/6

For a fresh checkout on **Ubuntu 22.04**, the block below installs dependencies,
builds all five engines and both benchmark clients, loads both datasets, then
runs Fig. 4, 5 and 6 with **no workload warmup and 5 measured seconds per case**.
Run it in Bash as root; for a regular user, prefix the two `apt-get` commands
with `sudo`. Change the `cd` path if the checkout is not at `~/RCC`.

The default `my.cnf` uses a **32 GiB buffer pool**, so allow additional RAM for
the server and clients. The five builds, dataset snapshots and temporary copies
also need free disk space. `BUILD_JOBS="$(nproc)"` uses one build job per available
CPU (128 jobs when `nproc` reports 128); lower it if compilation runs out of
memory. Downloads require network access.

```bash
(
  set -euo pipefail
  cd "$HOME/RCC/MySQL"

  # 1. Install build dependencies and NUMA tools (root, or use sudo).
  apt-get update
  apt-get install -y \
    build-essential cmake bison git wget ca-certificates python3 perl \
    autoconf automake libtool pkg-config \
    libssl-dev libncurses-dev libaio-dev libnuma-dev libudev-dev \
    libtirpc-dev rpcsvc-proto zlib1g-dev numactl

  # Use Ubuntu's GCC/G++ (GCC >= 10), overriding the scripts' gcc-10 default.
  export CC=gcc CXX=g++ BUILD_JOBS="$(nproc)"

  # 2. Build and install all engines sequentially. Vanilla supplies client tools.
  bash run_server.sh --vanilla --build-only
  bash run_server.sh --rcc --build-only
  bash run_server.sh --rcc-c --build-only
  bash run_server.sh --bamboo --build-only
  bash run_server.sh --rebirth --build-only

  # 3. Build the Zipfian-capable sysbench revision against this Vanilla install.
  export MYSQL_PREFIX="$PWD/Vanilla/mysql-8.4.5-install"
  export LD_LIBRARY_PATH="$MYSQL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  mkdir -p tools
  if [ ! -d tools/sysbench ]; then
    git clone https://github.com/akopytov/sysbench.git tools/sysbench
  fi
  git -C tools/sysbench checkout 3ceba0b1e115f8c50d1d045a4574d8ed643bd497
  (
    cd tools/sysbench
    ./autogen.sh
    ./configure --with-mysql-includes="$MYSQL_PREFIX/include" \
      --with-mysql-libs="$MYSQL_PREFIX/lib" --without-pgsql
    make -j"$BUILD_JOBS"
    ./src/sysbench --version
  )

  # 4. Build the TPC-C loader and measurement client.
  bash scripts/build_clients.sh

  # 5. Load reusable Vanilla snapshots; matching existing snapshots are reused.
  bash scripts/load_sysbench_data.sh   # 10M rows, 128 partitions; Fig. 4 and 6
  bash scripts/load_tpcc_data.sh       # 1 warehouse, 100K items; Fig. 5

  # 6. Inspect the plan, then execute all three figures.
  bash run_all_experiments.sh --warmup 0 --runtime 5 --dry-run
  bash run_all_experiments.sh --warmup 0 --runtime 5
)
```

The surrounding subshell stops at the first failed command without closing your
interactive shell. Its exported variables do not remain set after the block
ends; use the standalone sysbench check below when running the binary manually
in a new shell. The dependency list follows the
[MySQL source prerequisites](https://dev.mysql.com/doc/refman/8.4/en/source-installation-prerequisites.html),
the MySQL 8.4.5 checks for
[RPC](https://github.com/mysql/mysql-server/blob/mysql-8.4.5/cmake/rpc.cmake) and
[libudev](https://github.com/mysql/mysql-server/blob/mysql-8.4.5/cmake/fido2.cmake),
and the [sysbench build instructions](https://github.com/akopytov/sysbench#building-and-installing-from-source).

Keep `--build-only` on the engine build commands. The experiment loaders and
runners start their own servers; no manually started MySQL server or
`rcc_data_dir` / `rcc_log_dir` environment variables are needed.
If NUMA policy changes are unavailable (for example, `set_mempolicy: Operation
not permitted` in a container), add `--numa none` to **both loaders and the
experiment commands**.

After the engines and clients are built, steps 5 and 6 can instead be combined:

```bash
bash run_all_experiments.sh --prepare --warmup 0 --runtime 5
```

`--prepare` only loads missing datasets; it **does not build engines or clients**.
`Missing Vanilla tool: mysqld; build Vanilla first` means step 2 has not installed
`Vanilla/mysql-8.4.5-install/bin/mysqld` successfully. Complete the build before
retrying. `--dry-run` checks the plan only, not installed binaries or datasets.

With the default sweeps there are 24 + 30 + 15 = **69 cases**, one repetition
each. Five seconds applies to each case, so the requested measurement windows
total **345 seconds**. Loading, copying snapshots, starting/stopping servers,
and TPC-C consistency checks take additional time. **`--warmup 0` still performs
the full-table preload for Fig. 4/6** and the statistics refresh for Fig. 5.
Results are saved under `results/Fig4/`, `results/Fig5/` and `results/Fig6/`, in
new timestamped directories containing `summary.csv` and per-case logs.

## Build

From this directory, with the MySQL 8.4 build dependencies installed:

```bash
bash run_server.sh --vanilla --build-only
bash run_server.sh --rcc --build-only
bash run_server.sh --rcc-c --build-only
bash run_server.sh --bamboo --build-only
bash run_server.sh --rebirth --build-only
```

These commands download MySQL 8.4.5 when needed, refresh the InnoDB overlay,
configure, build and install. They do not start a server or access data/log
directories. The default compilers are `gcc-10` and `g++-10`; set `CC` and
`CXX` to use another supported compiler (GCC 10 or newer). `BUILD_JOBS` defaults
to `nproc` (all available CPUs); set it to override build parallelism. RCC and RCC-C share
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
cd "$HOME/RCC/MySQL"
MYSQL_PREFIX="$PWD/Vanilla/mysql-8.4.5-install"
# Or: MYSQL_PREFIX=/path/to/vanilla-install
export LD_LIBRARY_PATH="$MYSQL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p tools
git clone https://github.com/akopytov/sysbench.git tools/sysbench
git -C tools/sysbench checkout 3ceba0b1e115f8c50d1d045a4574d8ed643bd497
(
  cd tools/sysbench
  ./autogen.sh
  ./configure --with-mysql-includes="$MYSQL_PREFIX/include" \
    --with-mysql-libs="$MYSQL_PREFIX/lib" --without-pgsql
  make -j"${BUILD_JOBS:-$(nproc)}"
  ./src/sysbench --version
)
```

The runner looks for `tools/sysbench/src/sysbench`, then
`$HOME/sysbench/src/sysbench`, then `sysbench` on `PATH`. A binary without
`--rand-zipfian-exp` is rejected. Override it with `--sysbench /path/to/sysbench`.

For a standalone check after building, including from a new shell:

```bash
cd "$HOME/RCC/MySQL"
export MYSQL_PREFIX="$PWD/Vanilla/mysql-8.4.5-install"
export LD_LIBRARY_PATH="$MYSQL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./tools/sysbench/src/sysbench --version
./tools/sysbench/src/sysbench --help
```

`error while loading shared libraries: libmysqlclient.so.24` means the runtime
linker cannot find the Vanilla client library. If the library exists under
`$MYSQL_PREFIX/lib`, the exports above supply its location; rebuilding sysbench
is not needed for that error. The experiment scripts already set this library
path for their clients. A block copied separately must also define
`MYSQL_PREFIX` before `./configure`; the sysbench source is in `tools/sysbench`,
while this repository's `sysbench/` directory contains only Lua workloads.

```bash
cd "$HOME/RCC/MySQL"
bash scripts/build_clients.sh
```

This builds `tpcc-mysql/tpcc_load` and `tpcc-mysql/tpcc_start` against
`Vanilla/mysql-8.4.5-install/bin/mysql_config`. To use an existing Vanilla
installation instead:

```bash
bash scripts/build_clients.sh --mysql-config /path/to/vanilla-install/bin/mysql_config
# Pass --loader-prefix /path/to/vanilla-install to the loaders and runs below.
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
after loading and, by default, after every measured run. A failed check stops the run;
the output is retained as `consistency.tsv`. After measurement, the engine
shuts down fully and a separate Vanilla server checks the persisted rows in
an explicit SERIALIZABLE transaction. Validation is outside the measured time;
its server logs are in `consistency-check/`.
To bypass the post-run check temporarily, pass `--skip-consistency-check`.
This skips starting the validation server and records `consistency_check=skipped`
in `run.json`, `summary.json` and `summary.csv`. It does not bypass checks when
loading a reusable dataset, client errors, or server shutdown failures.
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

# Short run: no workload warmup, 5 measured seconds per case:
bash run_all_experiments.sh --prepare --warmup 0 --runtime 5
```

`--dry-run` prints every case without creating files, checking/installing
binaries, loading data, or starting/stopping servers. `--prepare` loads missing
snapshots before running; without it, missing data stops the run with a loader
instruction. Neither option builds engines or clients.

Useful options (pass to any figure script):

| Option | Effect |
| --- | --- |
| `--engines Bamboo,Rebirth,RCC` | Select engine names and order |
| `--threads 1,16,32,64,96,128` | Override thread list; Fig. 6 defaults to 64 |
| `--rows 10M,100K,10K,1K,0.1K` | Sysbench access-range list; Fig. 4 defaults to 10M |
| `--warmup 30 --runtime 30` | Warmup and measured seconds |
| `--repeat 2` / `--paper` | Two repetitions / three repetitions, like DBx1000 |
| `--skip-consistency-check` | Skip post-run TPC-C validation; mark the result as `skipped` |
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

If Fig. 5 stopped at its final RCC-C / 128-thread case because of a consistency
failure, rerun only that case with validation disabled, then run Fig. 6:

```bash
(
  set -e
  bash scripts/fig5_tpcc_threads.sh --engines RCC-C --threads 128 \
    --warmup 0 --runtime 5 --skip-consistency-check
  bash scripts/fig6_sysbench_rows.sh --warmup 0 --runtime 5
)
```

These are new runs, not an in-place resume. The single-point Fig. 5 result is
saved in a new timestamped directory; the previous 29 results and the original
failure logs remain in the old directory. Combine the single-point result with
those earlier points when plotting the full sweep. Passing the option to
`run_all_experiments.sh` also works, but starts all three figures again.

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
New results also include `consistency_check`: `passed` for validated TPC-C runs,
`skipped` when explicitly bypassed, and `not_applicable` for sysbench. Older CSVs
do not have this column.
TPC-C completions use the per-thread `sc + lt` totals for NewOrder and Payment,
not tpmC. Final attempt-time totals are also summed from per-thread counters
after the workers finish. The runner requires every requested worker to finish
successfully. Rebuild the TPC-C client with `scripts/build_clients.sh` after
updating its source. Failed/partial clients and unexpected transaction types
stop the run instead of adding a successful result row.

Successful/aborted attempt times are sums of client-observed wall times, not CPU
time. Wait counters overlap successful attempt time and must not simply be
added to it. Server counter snapshots bracket the separate measurement client,
so they include its connection/drain interval; any configured warmup is excluded.
A missing custom counter is left empty, rather than reported as a measured zero.
Vanilla's lock counter scope can differ from the custom engines' successful-
attempt counters. Original logs and counters are retained for Fig. 4/5 time
breakdown analysis. The static gnuplot templates do not directly consume this
new CSV format.

### 5. Collect results and plot Fig. 4/5/6

```bash
cd "$HOME/RCC/MySQL"
# Plotting dependency, if not already installed:
python3 -m pip install matplotlib
bash scripts/plot_fig4_5_6.sh
```

The script reads `results/Fig4/**/summary.csv`, `Fig5/**/summary.csv` and
`Fig6/**/summary.csv`. For each engine/access range/table size/thread count/
repetition, it keeps the newest completed result by the runner's directory
timestamp. A single-point rerun therefore replaces that point while preserving
the rest of the sweep. Older results fill points missing from newer summaries;
the selected paths and counts are printed so this is visible. Failed cases
without a summary row are not parsed from logs. Repetitions are averaged with
equal weight; throughput and abort plots show sample standard deviations when
there is more than one measurement. Missing points remain gaps, not zeroes.

Outputs are under `results/plots/` by default:

| Directory | Plots (both PDF and PNG) |
| --- | --- |
| `Fig4/` | `fig4a_ycsb`, `fig4b_ycsb_time` |
| `Fig5/` | `fig5a_tpcc`, `fig5b_tpcc_time` |
| `Fig6/` | `fig6a_tps`, `fig6b_abort` |

Each figure directory also contains `collected.csv` (selected measurements with
source paths and consistency status) and `aggregated.csv` (means, standard
deviations, sample counts and time components). `sources.json` records input
files and the selection policy. A `skipped` consistency check remains marked in
the CSV and produces a warning. Legacy results without that field are `unknown`.

Time bars divide each run's client attempt-time totals and server wait counters
by committed transactions before averaging. Where both wait counters are
available, execution is successful-attempt time minus lock wait and commit wait;
wasted time is aborted-attempt time per commit. If a counter is missing or the
wait sum exceeds successful time, the successful total is shown with dotted fill
as **Successful (unsplit)**, alongside wasted time. Missing counters remain empty
in CSVs. This avoids inventing a measured zero or double-counting waits; the
counter scope/window limitations described above still apply. Abort fractions
are converted to percentages. Fig. 6 orders access ranges from largest to
smallest. Non-default row/thread sweeps produce separate files with setting
suffixes instead of averaging different workloads together.

```bash
# Only Fig. 5; explicitly combine an earlier sweep and a single-point rerun.
bash scripts/plot_fig4_5_6.sh --figures fig5 \
  --fig5-dir results/Fig5/<sweep-directory> results/Fig5/<rerun-directory>

# Use another results root or output directory; export SVG as well.
bash scripts/plot_fig4_5_6.sh --results-dir /path/to/results \
  --output-dir /path/to/plots --formats pdf png svg

# Average all stored runs instead of selecting the latest case/repeat.
# Use only comparable runs (same benchmark settings, builds and hardware).
bash scripts/plot_fig4_5_6.sh --all-runs

# Collect/aggregate CSVs without matplotlib or rendering plots.
python3 scripts/plot_figures.py --collect-only
```

The `--fig4-dir`, `--fig5-dir` and `--fig6-dir` overrides each accept one or more
figure directories, timestamped run directories or summary CSV paths. Default
paths are relative to the script, so it can be launched from any working
directory. Re-running overwrites matching output filenames; source results are
read-only.

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
