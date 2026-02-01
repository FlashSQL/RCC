#!/bin/bash

BASE_DIR=`pwd -P`

# 1. Load data for synthetic banchmark
bash $BASE_DIR/scripts/load_synthetic_data.sh

# 2. Run MySQL Server
bash run_server.sh

# 3. Run Synthetic Benchmark
python3 $BASE_DIR/synthetic_benchmark/varying_transaction_length.py