#!/usr/bin/env bash
# Fig. 4: sysbench YCSB, 10M rows, Zipf 0.9, ascending keys, 8 reads + 8 writes.
# Threads: 1,16,32,64,96,128; SERIALIZABLE; 30s warmup + 30s measurement.
set -euo pipefail
exec python3 "$(dirname "$(realpath "$0")")/experiments.py" fig4 "$@"
