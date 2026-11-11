#!/usr/bin/env bash
# Fig. 6: sysbench YCSB, Zipf 0.9, ascending keys, 64 threads.
# One 10M-row table, key ranges: 10M,100K,10K,1K,0.1K.
# SERIALIZABLE; 30s warmup + 30s measurement.
set -euo pipefail
exec python3 "$(dirname "$(realpath "$0")")/experiments.py" fig6 "$@"
