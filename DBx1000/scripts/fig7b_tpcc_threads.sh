#!/bin/bash
# DBx1000 TPC-C - Varying Client Threads (paper Fig. 7(b))
# TPC-C 1 warehouse, NewOrder:Payment = 1:1 (1% of New-Orders abort), SERIALIZABLE.
# WOUND_WAIT, BAMBOO, SILO, RCC on configs/fig7b_tpcc_threads.h.
# Usage: ./fig7b_tpcc_threads.sh [--threads 1,16,32,64,96,128] [--paper] [--results-dir <path>] [--dry-run]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# ==========================================
# CONFIGURATION (paper settings)
# ==========================================
TITLE="DBx1000 TPC-C - Varying Client Threads"
# Format: "name:config:CC_ALG"
RUNS=(
    "WOUND_WAIT:fig7b_tpcc_threads.h:WOUND_WAIT"
    "BAMBOO:fig7b_tpcc_threads.h:BAMBOO"
    "SILO:fig7b_tpcc_threads.h:SILO"
    "RCC:fig7b_tpcc_threads.h:RCC"
)
RESULTS_DIR="$BASE_DIR/results/Fig7b"

parse_args "$@"
run_experiment
