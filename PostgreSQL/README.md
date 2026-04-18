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

All experiment drivers live in the top-level `plots/` directory; each runs the benchmark and then calls its matching `plot_<fig>.sh` to produce CSVs and PDFs. Use `--postgres` to run PostgreSQL-only.

### Synthetic Benchmarks (Fig4a, Fig4b)

```bash
cd plots
bash fig4a_varying_transaction_length.sh --postgres
bash fig4b_varying_hotspot_position.sh --postgres
```

> **Note:** Database: `synt`, Table: `bench_hotread`

### TPC-C Benchmarks (Fig5a, Fig5b)

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
cd plots
bash fig5a_varying_client_threads.sh --postgres
bash fig5b_varying_warehouse_count.sh --postgres
```

> **Note:** Database: `tpcc`. PostgreSQL TPC-C runs at **Repeatable Read** isolation (`sysbench-tpcc --trx_level=RR`); the consistency check has been verified under this configuration.

---

## 4. Results

Results are stored in `plots/fig*/results/`:

```
plots/fig4_varying_transaction_length/results/
├── postgres/vanilla/output.log
└── postgres/rcc/output.log

plots/fig6_varying_client_threads/results/
├── postgres/vanilla/s1_t1/sysbench.log
└── postgres/rcc/s1_t128/sysbench.log
```

### Extract Results

```bash
# Synthetic benchmark
grep "^[0-9]" plots/fig4_varying_transaction_length/results/postgres/rcc/output.log

# TPC-C benchmark
grep "transactions:" plots/fig6_varying_client_threads/results/postgres/rcc/s1_t64/sysbench.log
```

---

## Experiments Summary

| Experiment | Benchmark | Description |
|------------|-----------|-------------|
| Fig4a | Synthetic | Varying transaction length: 64, 128, 256 reads/tx |
| Fig4b | Synthetic | Varying hotspot position: 0.0, 0.25, 0.5, 0.75, 1.0 |
| Fig5a | TPC-C     | Varying client threads: 1, 8, 16, 32, 64, 128 |
| Fig5b | TPC-C     | Varying warehouse count: 1, 2, 4, 8 |

## Build Modes

| Mode | Compile Flags | Description |
|------|---------------|-------------|
| vanilla | `-O2` | Vanilla PostgreSQL |
| rcc | `-O2 -DRCC` | PostgreSQL with RCC |

---

## Plotting Results

```bash
cd plots
bash plot_fig4.sh    # Fig4a
bash plot_fig5.sh    # Fig4b
bash plot_fig6.sh    # Fig5a
bash plot_fig7.sh    # Fig5b
```
