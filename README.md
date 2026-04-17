# RCC: Speculative Write Versioning with Redo Logs

## Overview

This repository contains the implementation of **RCC**, a concurrency control mechanism that leverages redo logs to resolve write-write (WW) conflicts, integrated with MySQL and PostgreSQL.

**Repository layout**
- `MySQL/` — MySQL variants (`Vanilla`, `RCC`, `RCC-W`, `RCC-D`, `RCC-C`), benchmarks (TPC-C, YCSB), and run scripts
- `PostgreSQL/` — PostgreSQL with `-DRCC` build flag, benchmarks, and run scripts
- `plots/` — experiment drivers (Fig4–Fig8b) and gnuplot templates

> **Correctness:** All TPC-C runs passed BenchmarkSQL's built-in consistency checker, confirming that RCC preserves the consistency conditions defined in the TPC-C specification. See [TPC-C Consistency Check](#tpc-c-consistency-check) for how to reproduce.

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

All experiment drivers live under `plots/`. Each figure script runs the benchmark and then calls the matching `plot_<fig>.sh` to produce CSVs and PDFs.

| Figure | Benchmark | Description | DBMS |
|--------|-----------|-------------|------|
| Fig4a  | Synthetic | Varying transaction length (64, 128, 256 reads/tx) | MySQL + PostgreSQL |
| Fig4b  | Synthetic | Varying hotspot position (0.0 ~ 1.0) | MySQL + PostgreSQL |
| Fig5a  | TPC-C     | Varying client threads (1 ~ 128) | MySQL + PostgreSQL |
| Fig5b  | TPC-C     | Varying warehouse count (1 ~ 8) | MySQL + PostgreSQL |
| Fig6   | TPC-C     | RCC variants × (RR, SR) | MySQL |
| Fig8b  | YCSB      | Varying client threads (1 ~ 128), 10M rows, read-modify-write | MySQL |

Each fig script:
1. Cleans `rcc_data_dir` / `rcc_log_dir`
2. Builds/starts the server(s) via helper scripts
3. Runs the benchmark
4. Stores results under `plots/<fig_name>/results/...`
5. Calls the matching `plot_<fig>.sh`

### Run All Experiments

```bash
export rcc_data_dir=/path/to/data
export rcc_log_dir=/path/to/log

cd plots
bash run_all_experiments.sh
```

Runs fig4a → fig4b → fig5a → fig5b → fig6 → fig8b sequentially and prompts for sudo password once.

### Run Experiments Individually

For the dual-DBMS figures (Fig4a/4b/5a/5b), each script accepts `--mysql`, `--postgres`, or `--all` (default). MySQL-only figures (Fig6, Fig8b) take no DB flag.

```bash
cd plots
bash fig4a_varying_transaction_length.sh            # both DBMSes
bash fig4b_varying_hotspot_position.sh --mysql      # MySQL only
bash fig5a_varying_client_threads.sh --postgres     # PostgreSQL only
bash fig5b_varying_warehouse_count.sh
bash fig6_rcc_variants.sh                           # MySQL only (TPC-C)
bash fig8b_ycsb_varying_thread.sh                   # MySQL only (YCSB)
```

> **Note:** PostgreSQL TPC-C (Fig5a/Fig5b) requires sysbench. See `PostgreSQL/README.md`.
> **Note:** Fig8b (YCSB) requires Maven + JDK for building the JDBC binding under `MySQL/YCSB_benchmark/`.

### Plotting Only

Regenerate plots from existing results:

```bash
cd plots
bash plot_fig4.sh
bash plot_fig5.sh
bash plot_fig6.sh
bash plot_fig6_rcc_variants.sh
bash plot_fig7.sh
bash plot_fig8b.sh
```

PDFs and CSVs land in `plots/<fig_name>/` (or `plots/<fig_name>/{mysql,postgres}/` for dual-DBMS figures).

### DBx1000 (YCSB) Experiments

For DBx1000-based YCSB experiments used as an additional comparison point, see the RebirthRetire codebase:

- Source: https://github.com/gitzhqian/RebirthRetire.git
- Plot template: [`plots/templates/dbx1000_ycsb.gnu`](plots/templates/dbx1000_ycsb.gnu)

Run the experiments per that repository's instructions, then feed the resulting CSV(s) through `dbx1000_ycsb.gnu`.

### TPC-C Consistency Check

After running any TPC-C experiment (Fig5a, Fig5b, Fig6), verify the database satisfies the TPC-C consistency conditions:

```bash
# MySQL
MySQL/<variant>/mysql-8.4.5-build/bin/mysql -uroot tpcc < MySQL/scripts/consistency_check.sql

# PostgreSQL
PostgreSQL/rcc_postgresql/bld/bin/psql -U postgres -d tpcc -f PostgreSQL/scripts/consistency_check.sql
```

The SQL files are adapted from [BenchmarkSQL](https://github.com/wieck/benchmarksql.git).
