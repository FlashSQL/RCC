# RCC on PostgreSQL

Implemented on PostgreSQL 16.2 with the `-DRCC` compile flag. RCC on PostgreSQL supports **Repeatable Read**.

## Limitations

- **Repeatable Read only.** RCC on PostgreSQL tracks only commit dependencies. Serializable isolation (PostgreSQL's SSI) is not supported.
- **No RCC-C.** Columnar RCC needs redo logs with column-level update deltas, but PostgreSQL's WAL stores full tuple images. RCC on PostgreSQL therefore tracks dependencies at the record level.

---

## 1. Environment Setup

### Prerequisites

- **OS**: Ubuntu 22.04 LTS

```bash
sudo apt-get update
sudo apt-get install -y make gcc libicu-dev pkg-config libreadline-dev zlib1g-dev numactl gnuplot
```

Build sysbench with PostgreSQL support:
```bash
sudo apt-get install -y make automake libtool pkg-config libaio-dev libpq-dev
cd /tmp
git clone https://github.com/akopytov/sysbench.git
cd sysbench
./autogen.sh
./configure --with-pgsql
make -j$(nproc)
sudo make install
```

### Configuration

```bash
export rcc_data_dir=/path/to/data    # PostgreSQL data directory
export rcc_log_dir=/path/to/log      # PostgreSQL log directory
```

> **Note:** The experiment scripts erase both directories before every run. Use dedicated directories.

---

## 2. Build & Run Server

```bash
./run_server.sh [options]

Options:
  --vanilla      Build vanilla PostgreSQL (without RCC)
  --rr           Build RCC (-DRCC) [default]
  --skip-build   Skip build step (use existing binaries)
  --numa <mode>  Server NUMA policy: pin (node 0) [default], interleave, none
  -h, --help     Show help
```

Examples:
```bash
./run_server.sh                  # Build RCC and start
./run_server.sh --vanilla        # Build vanilla and start
./run_server.sh --skip-build     # Use existing build, just start server
./rcc_postgresql/bld/bin/pg_ctl stop -D $rcc_data_dir   # Stop server
```

---

## 3. Reproducing Experiments

Each script sweeps three configurations over the client threads: it builds PostgreSQL, initializes and loads the database, warms up for 50 seconds (discarded) and measures 50 seconds.

```bash
bash scripts/ycsb_threads.sh     # YCSB
bash scripts/tpcc_threads.sh     # TPC-C
```

Options:
```
  --results-dir <path>   Results directory (default: results/ycsb_threads, results/tpcc_threads)
```

---

## 4. Results

Results are stored in `results/`:

```
results/tpcc_threads/
├── rcc_rr/
│   ├── s1_t1/
│   │   ├── summary.txt           # configuration of the run
│   │   ├── sysbench.log          # measured run
│   │   ├── warmup.log
│   │   ├── consistency.txt       # TPC-C consistency check
│   │   └── postgresql.log
│   └── ...
├── vanilla_rc/
└── vanilla_rr/
```

`results/ycsb_threads/` has the same layout with `t<threads>/` run directories, a `load.log` per run and no consistency check.

### Extract Results

```bash
grep "transactions:" results/tpcc_threads/rcc_rr/s1_t64/sysbench.log
grep "transactions:" results/ycsb_threads/rcc_rr/t64/sysbench.log
```

### Plot

The plot scripts and their gnuplot templates are in `plots/`. Each one writes the throughput of every configuration (`tps_128.csv`) into the results directory and renders the graph there:

```bash
bash plots/plot_ycsb_threads.sh     # results/ycsb_threads/ycsb_threads.pdf
bash plots/plot_tpcc_threads.sh     # results/tpcc_threads/tpcc_threads.pdf
```

---

## Experiments Summary

| Experiment | Benchmark | Description |
|------------|-----------|-------------|
| ycsb_threads | YCSB | 10M rows, zipf theta 0.9, 8 reads + 8 updates per transaction in ascending key order; threads 1, 16, 32, 64, 96, 128 |
| tpcc_threads | TPC-C | 1 warehouse, NewOrder:Payment = 1:1; threads 1, 16, 32, 64, 96, 128 |

## Build Modes

| Mode | Compile Flags | Isolation |
|------|---------------|-----------|
| vanilla_rc | `-O2` | Read Committed |
| vanilla_rr | `-O2` | Repeatable Read |
| rcc_rr | `-O2 -DRCC` | Repeatable Read |
