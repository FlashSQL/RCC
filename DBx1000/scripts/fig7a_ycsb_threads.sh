#!/bin/bash
# DBx1000 YCSB - Varying Client Threads (paper Fig. 7(a))
# YCSB 10M rows, zipf theta 0.9, 16 requests per transaction, read:update = 1:1, ascending key order, SERIALIZABLE.
# WOUND_WAIT, BAMBOO, SILO, RCC on configs/fig7a_ycsb_threads.h.
# Usage: ./fig7a_ycsb_threads.sh [--threads 1,16,32,64,96,128] [--paper] [--results-dir <path>] [--dry-run]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# ==========================================
# CONFIGURATION (paper settings)
# ==========================================
TITLE="DBx1000 YCSB - Varying Client Threads"
YCSB_ROWS=10000000
# Format: "name:config:CC_ALG"
RUNS=(
    "WOUND_WAIT:fig7a_ycsb_threads.h:WOUND_WAIT"
    "BAMBOO:fig7a_ycsb_threads.h:BAMBOO"
    "SILO:fig7a_ycsb_threads.h:SILO"
    "RCC:fig7a_ycsb_threads.h:RCC"
)
RESULTS_DIR="$BASE_DIR/results/Fig7a"

parse_args "$@"
run_experiment
