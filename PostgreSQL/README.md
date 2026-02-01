# RCC: Speculative Write Versioning for PostgreSQL

Implemented on PostgreSQL with the `-DRCC` compile flag.

---

## 1. Environment Setup

### Prerequisites

- **OS**: Ubuntu 22.04 LTS

```bash
sudo apt-get update
sudo apt-get install -y make gcc libicu-dev pkg-config libreadline-dev zlib1g-dev
```

### Configuration

```bash
export rcc_data_dir=/mnt/test_data    # PostgreSQL data directory
export rcc_log_dir=/mnt/test_log      # PostgreSQL log directory
```

---

## 2. Build & Run Server

```bash
./run_server.sh [options]

Options:
  --vanilla      Build vanilla PostgreSQL (without RCC)
  --skip-build   Skip build step (use existing binaries)
  -h, --help     Show help
```

> **Note:** To start fresh, clear data and log directories first:
> ```bash
> rm -rf $rcc_data_dir/* $rcc_log_dir/*
> ```

Examples:
```bash
./run_server.sh                  # Build RCC and start
./run_server.sh --vanilla        # Build vanilla and start
./run_server.sh --skip-build     # Use existing build, just start server
./util/shutdown.sh               # Stop server
```

---

## 3. Reproducing Experiments

### Synthetic Benchmarks (Fig4, Fig5)

```bash
./fig4_varying_transaction_length.sh
./fig5_varying_hotspot_position.sh
```

> **Note:** Database: `stx`, Table: `bench_hotread`

### TPC-C Benchmarks (Fig6, Fig7)

Install sysbench prerequisites:
```bash
sudo apt-get install -y make automake libtool pkg-config libaio-dev libpq-dev
```

Build sysbench:
```bash
cd /tmp
git clone https://github.com/akopytov/sysbench.git
cd sysbench
./autogen.sh
./configure --with-pgsql
make -j$(nproc)
sudo make install
```

Run:
```bash
./fig6_varying_client_threads.sh
./fig7_varying_warehouse_count.sh
```

> **Note:** Database: `tpcc`

---

## 4. Results

Results are stored in `fig*/results/`:

```
fig4_varying_transaction_length/results/
├── vanilla/output.log
└── rcc/output.log

fig6_varying_client_threads/results/
├── vanilla/s1_t1/sysbench.log
└── rcc/s1_t128/sysbench.log
```

### Extract Results

```bash
# Synthetic benchmark
grep "^[0-9]" fig4_varying_transaction_length/results/rcc/output.log

# TPC-C benchmark
grep "transactions:" fig6_varying_client_threads/results/rcc/s1_t64/sysbench.log
```

---

## Experiments Summary

| Experiment | Benchmark | Description |
|------------|-----------|-------------|
| Fig4 | Synthetic | Varying transaction length: 64, 128, 256 reads/tx |
| Fig5 | Synthetic | Varying hotspot position: 0.0, 0.25, 0.5, 0.75, 1.0 |
| Fig6 | TPC-C | Varying client threads: 1, 8, 16, 32, 64, 128 |
| Fig7 | TPC-C | Varying warehouse count: 1, 2, 4, 8 |

## Build Modes

| Mode | Compile Flags | Description |
|------|---------------|-------------|
| vanilla | `-O2` | Vanilla PostgreSQL |
| rcc | `-O2 -DRCC` | PostgreSQL with RCC |

---

## Plotting Results

```bash
cd plots
bash plot_fig4.sh
bash plot_fig5.sh
bash plot_fig6.sh
bash plot_fig7.sh
```
