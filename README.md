# Optimistic Deadlock Handling with Redo Log-Based Speculative Versioning under High Contention

## Overview

**RCC** is a Redo log-based Concurrency Control scheme for high write contention. This repository implements it on MySQL, PostgreSQL and DBx1000.

With in-place updates, write-write conflicts serialize transactions that update the same record. RCC instead:
- updates a record out of place, as a speculative version built from the redo log entry, so conflicting transactions do not wait for their predecessors to commit;
- guarantees serializability with lock-based conflict ordering and a dependency graph;
- installs updates at commit, so an abort only discards speculative versions.

On top of this, RCC introduces two techniques:
- **Optimistic deadlock handling** grants locks without deadlock checks and detects cycles asynchronously.
- **Columnar RCC (RCC-C)** removes record-level false conflicts with column-level redo logs.

**Repository layout**
- `MySQL/` — MySQL with RCC (see [`MySQL/README.md`](MySQL/README.md))
- `DBx1000/` — DBx1000 with RCC (see [`DBx1000/README.md`](DBx1000/README.md))
- `PostgreSQL/` — PostgreSQL 16.2 with RCC (see [`PostgreSQL/README.md`](PostgreSQL/README.md))
- `plots/` — plot scripts and gnuplot templates of the paper's figures

Each system's README covers its setup, build, experiments and results.

> **Correctness:** All TPC-C runs on MySQL and PostgreSQL passed the BenchmarkSQL consistency checks (`scripts/consistency_check.sql`). DBx1000 runs the same checks when built with `TPCC_CONSISTENCY true` ([details](DBx1000/README.md#tpc-c-consistency-check)).

## Hardware Configuration

**Testing Configuration** (from paper experiments):
- AMD EPYC 9754 CPU (128 cores at 2.25GHz)
- 256GB DRAM
- Ubuntu 22.04

### CPU Settings

> **Important:** Set the CPU to maximum performance before running any experiment. By default the `schedutil` governor lowers the clock under light load, and idle cores enter deep C-states (C2 on the EPYC 9754: 800 µs exit latency). Both slow down lock handoffs and make throughput vary between runs. These settings reset on reboot, so apply them again after every boot.

```bash
cd /sys/devices/system/cpu
echo performance | sudo tee cpu[0-9]*/cpufreq/scaling_governor > /dev/null   # fixed maximum clock (boost stays on)
echo 1 | sudo tee cpu[0-9]*/cpuidle/state[2-9]/disable > /dev/null           # no C-states deeper than C1

# Check
cat cpu*/cpufreq/scaling_governor | sort | uniq -c                            # every CPU: performance
grep . cpu0/cpuidle/state*/name cpu0/cpuidle/state*/disable                   # C2 and deeper: disable = 1

# Revert
echo schedutil | sudo tee cpu[0-9]*/cpufreq/scaling_governor > /dev/null
echo 0 | sudo tee cpu[0-9]*/cpuidle/state[2-9]/disable > /dev/null
```

`CPU_FREQ` of DBx1000 is the TSC rate (2.25 GHz) and does not change with these settings.

## Reproducing the Paper's Figures

| Figure | Benchmark | System |
|--------|-----------|--------|
| Fig. 4 | sysbench YCSB, 10M rows, varying client threads: (a) throughput, (b) time breakdown | [MySQL](MySQL/README.md) |
| Fig. 5 | TPC-C, varying client threads: (a) throughput, (b) time breakdown | [MySQL](MySQL/README.md) |
| Fig. 6 | sysbench YCSB, varying access range in a 10M-row table (64 threads): (a) throughput, (b) abort rate | [MySQL](MySQL/README.md) |
| Fig. 7 | (a) YCSB, (b) TPC-C, varying client threads: throughput | [DBx1000](DBx1000/README.md) |
| Fig. 8 | YCSB, ablation of RCC: (a) throughput, (b) time breakdown | [DBx1000](DBx1000/README.md) |

PostgreSQL has separate YCSB and TPC-C experiments; see [`PostgreSQL/README.md`](PostgreSQL/README.md).

MySQL Fig. 4–6 use SERIALIZABLE isolation, 30s warmup and 30s measurement.
After building the engine/client binaries, create reusable data and run:

```bash
cd MySQL
bash scripts/load_sysbench_data.sh
bash scripts/load_tpcc_data.sh
bash scripts/fig4_sysbench_threads.sh
bash scripts/fig5_tpcc_threads.sh
bash scripts/fig6_sysbench_rows.sh
```

Each figure supports `--dry-run` to inspect its plan. See [MySQL setup and options](MySQL/README.md#paper-experiments-fig-4-5-and-6).

## Plotting

`plots/` has a plot script per figure (`plot_<fig>.sh`) and the gnuplot templates (`templates/`). Each script reads the results directory of its experiment and writes the plotted data and the PDF there. It needs gnuplot (`sudo apt-get install -y gnuplot`).

```bash
bash plots/plot_fig7a.sh               # DBx1000/results/Fig7a/fig7a_ycsb.pdf
bash plots/plot_fig7b.sh               # DBx1000/results/Fig7b/fig7b_tpcc.pdf
bash plots/plot_fig8.sh                # DBx1000/results/Fig8/fig8a_tps.pdf, fig8b_time.pdf
bash plots/plot_fig7a.sh /path/to/results/Fig7a   # another results directory
```
