# Optimistic Deadlock Handling with Redo Log-Based Speculative Versioning under High Contention

## Overview

This repository contains the implementation of **RCC**, a Redo log-based Concurrency Control scheme for high write contention, on MySQL, PostgreSQL and DBx1000.

With immediate, in-place updates, transactions that update the same record are serialized by write-write conflicts. In RCC, a transaction updates a record out of place by creating a speculative write version from its redo log entry, so conflicting transactions are pipelined without waiting for their predecessors to commit. RCC guarantees serializability with lock-based access-time conflict ordering and dependency graph tracking. Updates are installed lazily at commit, so a transaction aborts by discarding its speculative versions. On top of this, RCC introduces two techniques:
- **Optimistic deadlock handling** grants locks without checking for deadlocks and detects cycles asynchronously in the dependency graph.
- **Columnar RCC (RCC-C)** removes record-level false conflicts with column-level redo logs.

**Repository layout**
- `MySQL/` — MySQL with RCC, benchmarks and experiment scripts (see [`MySQL/README.md`](MySQL/README.md))
- `DBx1000/` — DBx1000 with RCC, experiment configurations and scripts (see [`DBx1000/README.md`](DBx1000/README.md))
- `PostgreSQL/` — PostgreSQL 16.2 with RCC, benchmarks, experiment and plot scripts (see [`PostgreSQL/README.md`](PostgreSQL/README.md))
- `plots/` — plot scripts and gnuplot templates of the paper's figures

Each system's README covers its environment setup, build, experiments and results.

> **Correctness:** All TPC-C runs on MySQL and PostgreSQL passed the consistency checks of BenchmarkSQL (`MySQL/scripts/consistency_check.sql`, `PostgreSQL/scripts/consistency_check.sql`), confirming that RCC preserves the consistency conditions defined in the TPC-C specification.

## Hardware Configuration

**Testing Configuration** (from paper experiments):
- AMD EPYC 9754 CPU (128 cores at 2.25GHz)
- 256GB DRAM
- Ubuntu 22.04

## Reproducing the Paper's Figures

| Figure | Benchmark | System |
|--------|-----------|--------|
| Fig. 4 | YCSB, varying client threads: (a) throughput, (b) time breakdown | [MySQL](MySQL/README.md) |
| Fig. 5 | TPC-C, varying client threads: (a) throughput, (b) time breakdown | [MySQL](MySQL/README.md) |
| Fig. 6 | YCSB, varying deadlock probability (64 threads): (a) throughput, (b) abort rate | [MySQL](MySQL/README.md) |
| Fig. 7 | (a) YCSB, (b) TPC-C, varying client threads: throughput | [DBx1000](DBx1000/README.md) |
| Fig. 8 | YCSB, ablation of RCC: (a) throughput, (b) time breakdown | [DBx1000](DBx1000/README.md) |

PostgreSQL has its own experiments (YCSB and TPC-C with varying client threads, comparing vanilla Read Committed, vanilla Repeatable Read and RCC); see [`PostgreSQL/README.md`](PostgreSQL/README.md).

## Plotting

`plots/` holds a plot script per figure (`plot_<fig>.sh`) and the gnuplot template of every figure (`templates/fig<N>*.gnu`). A plot script reads the results directory of its experiment, writes the plotted data there, and renders the figure into a PDF in the same directory. It requires gnuplot (`sudo apt-get install -y gnuplot`).

```bash
bash plots/plot_fig7a.sh               # DBx1000/results/Fig7a/fig7a_ycsb.pdf
bash plots/plot_fig7b.sh               # DBx1000/results/Fig7b/fig7b_tpcc.pdf
bash plots/plot_fig8.sh                # DBx1000/results/Fig8/fig8a_tps.pdf, fig8b_time.pdf
bash plots/plot_fig7a.sh /path/to/results/Fig7a   # another results directory
```
