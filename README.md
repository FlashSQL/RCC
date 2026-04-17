# RCC: Speculative Write Versioning with Redo Logs

## Overview

This repository contains the implementation of **RCC**, a concurrency control mechanism that leverages redo logs to resolve write-write (WW) conflicts.

## Hardware Configuration

**Testing Configuration** (from paper experiments):
- AMD EPYC 9754 CPU (128 cores at 2.25GHz)
- 256GB DRAM
- 2x SK hynix PS1010 4TB NVMe SSDs (data + log devices)
- Ubuntu 22.04

## Software Dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
    gcc make cmake build-essential \
    python3 python3-pip \
    libreadline-dev zlib1g-dev libssl-dev \
    libicu-dev pkg-config \
    libncurses5-dev bison flex \
    autoconf automake libtool \
    libmysqlclient-dev libpq-dev \
    libaio1 libaio-dev gnuplot

pip3 install psycopg2-binary MySQLdb
```

## Build & Run

### MySQL

MySQL variants live under `MySQL/{Vanilla,RCC,RCC-W,RCC-D,RCC-C}/`. `run_server.sh` downloads MySQL 8.4.5, applies the variant's modified `innobase/`, builds, initializes, and starts the server.

```bash
export rcc_data_dir=/path/to/data
export rcc_log_dir=/path/to/log

cd MySQL

# Build and start (choose one variant)
bash run_server.sh --vanilla
bash run_server.sh --rcc
bash run_server.sh --rcc-w
bash run_server.sh --rcc-d
bash run_server.sh --rcc-c

# Reuse an existing build:
bash run_server.sh --rcc --skip-build

# Stop
bash shutdown.sh --rcc
```

### PostgreSQL

```bash
export rcc_data_dir=/path/to/data
export rcc_log_dir=/path/to/log

cd PostgreSQL
bash run_server.sh              # Build RCC and start
bash run_server.sh --vanilla    # Build vanilla PostgreSQL
bash util/shutdown.sh
```

## Reproducing Experiments

All experiment scripts are unified under `plots/`. Each figure script runs both MySQL and PostgreSQL by default and then calls the matching plot script.

| Figure | Benchmark | Description |
|--------|-----------|-------------|
| Fig4a  | Synthetic | Varying transaction length (64, 128, 256 reads/tx) |
| Fig4b  | Synthetic | Varying hotspot position (0.0 ~ 1.0) |
| Fig5a  | TPC-C     | Varying client threads (1 ~ 128) |
| Fig5b  | TPC-C     | Varying warehouse count (1 ~ 8) |
| Fig6   | TPC-C     | RCC variants (Vanilla, RCC-W, RCC-D, RCC, RCC-C) × (RR, SR) — MySQL only |

### Run All Experiments

```bash
export rcc_data_dir=/path/to/data
export rcc_log_dir=/path/to/log

cd plots
bash run_all_experiments.sh
```

This runs fig4a → fig4b → fig5a → fig5b → fig6 sequentially and prompts for sudo password once.

### Run Experiments Individually

Each script accepts `--mysql`, `--postgres`, or `--all` (default). Fig6 is MySQL only and takes no DB flag.

```bash
cd plots
bash fig4a_varying_transaction_length.sh            # both DBs
bash fig4b_varying_hotspot_position.sh --mysql      # MySQL only
bash fig5a_varying_client_threads.sh --postgres     # PostgreSQL only
bash fig5b_varying_warehouse_count.sh
bash fig6_rcc_variants.sh                           # MySQL only
```

Each fig script:
1. Cleans `rcc_data_dir` / `rcc_log_dir`
2. Builds/starts the server(s) via helper scripts
3. Runs the benchmark
4. Stores results under `plots/<fig_name>/results/{mysql,postgres}/...`
5. Calls the matching `plot_<fig>.sh` to produce CSVs and PDFs

> **Note:** PostgreSQL TPC-C benchmarks (Fig5a, Fig5b) require sysbench. See `PostgreSQL/README.md` for installation.

### Plotting Only

If results already exist, regenerate plots without rerunning experiments:

```bash
cd plots
bash plot_fig4.sh
bash plot_fig5.sh
bash plot_fig6.sh
bash plot_fig6_rcc_variants.sh
bash plot_fig7.sh
```

Outputs land in `plots/<fig_name>/{mysql,postgres}/*.pdf` (fig6 variants go in `plots/fig6_rcc_variants/*.pdf`).

### DBx1000 (YCSB) Experiments

For DBx1000-based YCSB experiments, use the RebirthRetire codebase:

- Source: https://github.com/gitzhqian/RebirthRetire.git
- Plot template: [`plots/templates/dbx1000_ycsb.gnu`](plots/templates/dbx1000_ycsb.gnu)

Run the experiments per that repository's instructions, then feed the resulting CSV(s) through `dbx1000_ycsb.gnu` to produce the plot.

### TPC-C Consistency Check

After running any TPC-C experiment (Fig5a, Fig5b, Fig6), verify the database satisfies TPC-C's 12 consistency invariants:

```bash
# MySQL
MySQL/<variant>/mysql-8.4.5-build/bin/mysql -uroot tpcc < MySQL/scripts/consistency_check.sql

# PostgreSQL
PostgreSQL/rcc_postgresql/bld/bin/psql -U postgres -d tpcc -f PostgreSQL/scripts/consistency_check.sql
```

The SQL files are adapted from [BenchmarkSQL](https://github.com/wieck/benchmarksql.git).
