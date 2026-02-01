#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

RESULT_DIR="$SCRIPT_DIR/fig4_varying_transaction_length/results"
POSTGRESQL_HOME="$SCRIPT_DIR/rcc_postgresql"
BLD_PATH="${POSTGRESQL_HOME}/bld"
PAGE_SIZE=16

BUILD_MODES=("vanilla" "rcc")

mkdir -p $RESULT_DIR

# Clean rcc_data_dir and rcc_log_dir at the start
echo "[INFO] Cleaning rcc_data_dir and rcc_log_dir..."
rm -rf $rcc_data_dir/* 2>/dev/null
rm -rf $rcc_log_dir/* 2>/dev/null

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

for BUILD_MODE in "${BUILD_MODES[@]}"; do
    print_header "Running fig4 with $BUILD_MODE build"

    mkdir -p $RESULT_DIR/$BUILD_MODE

    # Clean data_dir and log_dir before each experiment
    echo "[INFO] Cleaning data_dir and log_dir..."
    rm -rf $rcc_data_dir/* 2>/dev/null
    rm -rf $rcc_log_dir/* 2>/dev/null

    # Build PostgreSQL
    echo "[INFO] Building PostgreSQL ($BUILD_MODE mode)..."
    mkdir -p $POSTGRESQL_HOME/bld
    cd $POSTGRESQL_HOME/bld
    echo $PASSWD | sudo -S rm -rf $BLD_PATH/*
    make clean > /dev/null 2>&1

    if [[ "$BUILD_MODE" == "vanilla" ]]; then
        ../configure --prefix=$POSTGRESQL_HOME/bld --with-blocksize=$PAGE_SIZE CFLAGS="-O2" > /dev/null 2>&1
    else
        ../configure --prefix=$POSTGRESQL_HOME/bld --with-blocksize=$PAGE_SIZE CFLAGS="-O2 -DRCC" > /dev/null 2>&1
    fi

    make -j > /dev/null 2>&1
    make install > /dev/null 2>&1

    # Load data and run server
    bash $SCRIPT_DIR/scripts/load_synthetic_data.sh
    bash $SCRIPT_DIR/run_server.sh --skip-build

    # Run benchmark
    echo "[INFO] Running synthetic benchmark..."
    python3 $SCRIPT_DIR/synthetic_benchmark/varying_transaction_length.py | tee $RESULT_DIR/$BUILD_MODE/output.log

    # Shutdown
    bash $SCRIPT_DIR/util/shutdown.sh
    sleep 3
done

echo ""
echo "Results saved to: $RESULT_DIR"
echo "  - $RESULT_DIR/vanilla/"
echo "  - $RESULT_DIR/rcc/"

# Generate plot
echo ""
echo "[INFO] Generating plot..."
bash $SCRIPT_DIR/plots/plot_fig4.sh
