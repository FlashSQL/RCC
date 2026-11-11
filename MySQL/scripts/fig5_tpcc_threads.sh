#!/usr/bin/env bash
# Fig. 5: TPC-C, one warehouse, NewOrder:Payment = 1:1 (only these two).
# Threads: 1,16,32,64,96,128; SERIALIZABLE; 30s warmup + 30s measurement.
set -euo pipefail
exec python3 "$(dirname "$(realpath "$0")")/experiments.py" fig5 "$@"
