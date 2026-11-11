# RCC: Speculative Write Versioning for DBx1000

Implemented on DBx1000 as the concurrency control algorithm `CC_ALG = RCC`, next to `WOUND_WAIT`, `BAMBOO` and `SILO`.
The code base is [DBx1000](https://github.com/yxymit/DBx1000) with the Wound-Wait, Bamboo and Silo implementations of [Bamboo-Public](https://github.com/ScarletGuo/Bamboo-Public).

---

## 1. Environment Setup

### Prerequisites

- **OS**: Ubuntu 22.04 LTS
- **Paper server**: AMD EPYC 9754 (128 cores, 256 hardware threads, 2.25 GHz)

```bash
sudo apt-get update
sudo apt-get install -y g++ make numactl libnuma-dev libtbb-dev
```

> **Note:** `CPU_FREQ` in `config-std.h` and `configs/*.h` is the TSC rate of the paper server (2.25 GHz). On another machine, set it to that machine's TSC rate (`dmesg | grep tsc`); timing and throughput are computed from it.

---

## 2. Build & Run

The build reads `config.h`. Each file of `configs/` is the configuration of one experiment (RCC, 128 threads):

```bash
cp configs/fig7a_ycsb_threads.h config.h
make -j
# RCC: the detector on CPU 127, the workers off its core (CPUs 127 and 255 of the paper server); see Section 3
RCC_DETECT_CPUS=127 UNPINNED_CPUS=$(seq -s, 0 126),$(seq -s, 128 254) numactl --interleave all ./rundb
```

The algorithm and the thread count are `CC_ALG` and `THREAD_CNT` in `config.h`. RCC's techniques are switched by:

| Knob | Technique |
|------|-----------|
| `RCC_LUI` | Lazy update installation (lightweight rollback) |
| `RCC_ODH` | Optimistic deadlock handling with the asynchronous deadlock detector (`RCC_ADD` follows `RCC_ODH`) |
| `RCC_ERW` | Elimination of RW commit wait |

---

## 3. Reproducing Experiments

Each script runs every algorithm of an experiment for every thread count: it writes `config.h`, builds `rundb` and runs it for 30 seconds under `numactl --interleave all`.

> **Important:** RCC's asynchronous deadlock detector (ADD) must have one physical core to itself: the whole core, both of its hardware threads, is given to the detector thread, and no worker runs on it. The scripts do this for every RCC run: `RCC_DETECT_CPUS` pins the detector to the core, and `UNPINNED_CPUS` keeps the workers off both of its hardware threads (recorded in `run.env` of each run). The CPUs outside that core must be enough for the workers; otherwise the detector shares CPUs with the workers and the results do not match the paper.

```bash
bash scripts/fig7a_ycsb_threads.sh     # Fig7a
bash scripts/fig7b_tpcc_threads.sh     # Fig7b
bash scripts/fig8_ablation.sh          # Fig8
```

Options:
```
  --threads <list>       Comma-separated thread counts (default: 1,16,32,64,96,128)
  --paper                3 runs per point, as in the paper (default: 1)
  --results-dir <path>   Results directory (default: results/Fig7a, results/Fig7b, results/Fig8)
  --dry-run              Print the runs and their CPUs; build and run nothing
```

> **Note:** A run takes 0.5 to 2 minutes (build, data load, 30 s run). With `--paper`, each script makes 72 runs.

---

## 4. Results

Results are stored in `results/`:

```
results/Fig7a/
├── summary.txt                   # throughput and abort rate of every run
├── RCC_128_r1/
│   ├── config.h                  # configuration of the run
│   ├── build.log
│   ├── run.env                   # worker and detector CPUs
│   └── run.out                   # rundb output
└── ...
```

### Extract Results

```bash
cat results/Fig7a/summary.txt
grep "^\[summary\]" results/Fig7a/RCC_128_r1/run.out
```

---

## Experiments Summary

| Experiment | Benchmark | Description | Algorithms |
|------------|-----------|-------------|------------|
| Fig7a | YCSB | 10M rows, zipf theta 0.9, 16 requests per transaction in ascending key order, read:update = 1:1, varying threads | WOUND_WAIT, BAMBOO, SILO, RCC |
| Fig7b | TPC-C | 1 warehouse, NewOrder:Payment = 1:1, varying threads | WOUND_WAIT, BAMBOO, SILO, RCC |
| Fig8 | YCSB | Ablation: 1M rows, zipf theta 0.9, write ratio 0.9, varying threads | BAMBOO, RCC (LUI), RCC (LUI+ODH), RCC |

All experiments run at **Serializable** isolation.

## Configurations

| Config | Experiment | RCC knobs |
|--------|------------|-----------|
| `configs/fig7a_ycsb_threads.h` | Fig7a | all on |
| `configs/fig7b_tpcc_threads.h` | Fig7b | all on |
| `configs/fig8_ablation_rcc_lui.h` | Fig8, RCC (LUI) | `RCC_ODH false`, `RCC_ERW false` |
| `configs/fig8_ablation_rcc_lui_odh.h` | Fig8, RCC (LUI+ODH) | `RCC_ERW false` |
| `configs/fig8_ablation_rcc.h` | Fig8, RCC and BAMBOO | all on |
