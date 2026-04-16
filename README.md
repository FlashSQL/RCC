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
    libaio1 libaio-dev

pip3 install psycopg2-binary
```

## Build & Run


### MySQL

```bash
export rcc_data_dir=/path/to/data
export rcc_log_dir=/path/to/log

cd MySQL

# --server: vailla, bamboo, rcc
bash build.sh --server # Build
bash run_server.sh --server # Run MySQL Server
bash shutdown.sh --server
```

### PostgreSQL

```bash
# Set environment variables
export rcc_data_dir=/path/to/data
export rcc_log_dir=/path/to/log

cd PostgreSQL
bash run_server.sh              # Build RCC and start
bash run_server.sh --vanilla    # Build vanilla PostgreSQL
bash util/shutdown.sh
```

## Reproducing Experiments

Benchmark scripts to reproduce the experimental results in the paper:

| Figure | Benchmark | Description |
|--------|-----------|-------------|
| Fig4 | Synthetic | Varying transaction length (64, 128, 256 reads/tx) |
| Fig5 | Synthetic | Varying hotspot position (0.0 ~ 1.0) |
| Fig6 | TPC-C | Varying client threads (1 ~ 128) |
| Fig7 | TPC-C | Varying warehouse count (1 ~ 8) |
| Fig8 | TPC-C | Varying client threads with Serialzable mode (compare to bamboo) (only for mysql) |
| Fig10 | Synthetic | Comparison with OCC (only for mysql) |

### Run All Experiments

#### MySQL

```bash
cd MySQL
bash run_all_experiments.sh
```

#### PostgreSQL

```bash
cd PostgreSQL
bash run_all_experiments.sh
```

### Run Experiments Individually

#### MySQL

```bash
cd MySQL
bash fig4_varying_transaction_length.sh
bash fig5_varying_hotspot_position.sh
bash fig6_varying_client_threads.sh
bash fig7_varying_warehouse_count.sh
bash fig8_serializable.sh
bash fig10_occ.sh
```

#### PostgreSQL

```bash
cd PostgreSQL
bash fig4_varying_transaction_length.sh
bash fig5_varying_hotspot_position.sh
bash fig6_varying_client_threads.sh
bash fig7_varying_warehouse_count.sh
```

> **Note:** TPC-C benchmarks (Fig6, Fig7) require sysbench. See `PostgreSQL/README.md` for installation.

### Plotting Results

> **Note:** Requires `gnuplot`. Install with `sudo apt-get install -y gnuplot`.

#### MySQL

```bash
cd MySQL/plots
bash plot_fig4.sh
bash plot_fig5.sh
bash plot_fig6.sh
bash plot_fig7.sh
```

#### PostgreSQL

```bash
cd PostgreSQL/plots
bash plot_fig4.sh
bash plot_fig5.sh
bash plot_fig6.sh
bash plot_fig7.sh
```
