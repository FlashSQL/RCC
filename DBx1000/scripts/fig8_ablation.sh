#!/bin/bash
# DBx1000 Ablation Study (paper Fig. 8)
# YCSB 1M rows, zipf theta 0.9, write ratio 0.9, SERIALIZABLE.
#   BAMBOO
#   RCC_LUI       RCC (LUI)       configs/fig8_ablation_rcc_lui.h      (RCC_ODH false, RCC_ERW false)
#   RCC_LUI_ODH   RCC (LUI+ODH)   configs/fig8_ablation_rcc_lui_odh.h  (RCC_ERW false)
#   RCC           RCC             configs/fig8_ablation_rcc.h
# BB_PF_PW traces the deadlock prevention time (Prevention in Fig. 8(b)).
# Usage: ./fig8_ablation.sh [--threads 1,16,32,64,96,128] [--paper] [--results-dir <path>] [--dry-run]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# ==========================================
# CONFIGURATION (paper settings)
# ==========================================
TITLE="DBx1000 Ablation Study (YCSB 1M rows, write ratio 0.9)"
YCSB_ROWS=1000000
# Format: "name:config:CC_ALG"
RUNS=(
    "BAMBOO:fig8_ablation_rcc.h:BAMBOO"
    "RCC_LUI:fig8_ablation_rcc_lui.h:RCC"
    "RCC_LUI_ODH:fig8_ablation_rcc_lui_odh.h:RCC"
    "RCC:fig8_ablation_rcc.h:RCC"
)
RESULTS_DIR="$BASE_DIR/results/Fig8"

parse_args "$@"
run_experiment
