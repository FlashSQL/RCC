#!/bin/bash

BASE_DIR=`pwd -P`

# 1. Load data for synthetic banchmark
bash $BASE_DIR/scripts/load_tpcc_data.sh

# 2. Run MySQL Server
bash run_server.sh

# 3. Run Synthetic Benchmark
bash $BASE_DIR/scripts/run_tpcc_threads.sh