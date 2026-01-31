#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULT_DIR="$SCRIPT_DIR/fig7_varying_warehouse_count/results"

# Check required variables
if [[ -z "$rcc_data_dir" ]] || [[ -z "$rcc_log_dir" ]]; then
    echo "[ERROR] rcc_data_dir and rcc_log_dir must be set"
    echo "  export rcc_data_dir=/path/to/data"
    echo "  export rcc_log_dir=/path/to/log"
    exit 1
fi

# Get sudo password once
if [[ -z "$PASSWD" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
    export PASSWD
fi

mkdir -p $rcc_data_dir
mkdir -p $rcc_log_dir
mkdir -p $RESULT_DIR

# Run TPC-C Benchmark (varying warehouse count)
bash $SCRIPT_DIR/scripts/run_tpcc_warehouses.sh --results-dir $RESULT_DIR
