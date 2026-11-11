# RCC: Speculative Write Versioning for PostgreSQL

Implemented on PostgreSQL 16.2 with the `-DRCC` compile flag. RCC on PostgreSQL supports **Repeatable Read**.

---

## 1. Environment Setup

### Prerequisites

- **OS**: Ubuntu 22.04 LTS

```bash
sudo apt-get update
sudo apt-get install -y make gcc libicu-dev pkg-config libreadline-dev zlib1g-dev numactl
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
export rcc_data_dir=/mnt/test_data    # PostgreSQL data directory
export rcc_log_dir=/mnt/test_log      # PostgreSQL log directory
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
bash scripts/fig9a_ycsb_threads.sh     # Fig9a: YCSB
bash scripts/fig9b_tpcc_threads.sh     # Fig9b: TPC-C
```

Options:
```
  --results-dir <path>   Results directory (default: results/Fig9a, results/Fig9b)
```

> **Important:** RCC runs start the server with `-c rcc_bg_detect=on`, so that the asynchronous deadlock detector runs in the background as in the paper. Vanilla builds do not know this setting, so it is not in the shared `postgresql.conf`.

---

## 4. Results

Results are stored in `results/`:

```
results/Fig9b/
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

`results/Fig9a/` has the same layout with `t<threads>/` run directories, a `load.log` per run and no consistency check.

### Extract Results

```bash
grep "transactions:" results/Fig9b/rcc_rr/s1_t64/sysbench.log
grep "transactions:" results/Fig9a/rcc_rr/t64/sysbench.log
```

---

## Experiments Summary

| Experiment | Benchmark | Description |
|------------|-----------|-------------|
| Fig9a | YCSB | 10M rows, zipf theta 0.9, 8 reads + 8 updates per transaction in ascending key order; threads 1, 16, 32, 64, 96, 128 |
| Fig9b | TPC-C | 1 warehouse, NewOrder:Payment = 1:1; threads 1, 16, 32, 64, 96, 128 |

## Build Modes

| Mode | Compile Flags | Isolation |
|------|---------------|-----------|
| vanilla_rc | `-O2` | Read Committed |
| vanilla_rr | `-O2` | Repeatable Read |
| rcc_rr | `-O2 -DRCC` | Repeatable Read |
