#!/bin/bash

BASE_DIR=`pwd -P`
RESULT_DIR="$BASE_DIR/fig6_varying_client_threads/results"

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
BUILD_MODES=("vanilla" "rcc")

mkdir -p $RESULT_DIR

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

for BUILD_MODE in "${BUILD_MODES[@]}"; do

    MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
    BLD_PATH="$MYSQL_HOME-build"
    MYSQL_BIN="${BLD_PATH}/bin/mysqld"
    MYSQL="${BLD_PATH}/bin/mysql"

    print_header "Running fig5 with $BUILD_MODE build"
    mkdir -p $RESULT_DIR/$BUILD_MODE

    echo "[INFO] Cleaning data_dir and log_dir..."
    rm -rf $rcc_data_dir/* 2>/dev/null
    rm -rf $rcc_log_dir/* 2>/dev/null

    # Build MySQL
    echo "[INFO] Building MySQL ($BUILD_MODE mode)..."
    bash run_server.sh --$BUILD_MODE

    $MYSQL -u root -e "CREATE DATABASE tpcc;"
    $MYSQL -u root tpcc < $BASE_DIR/tpcc-mysql/create_table.sql
    $MYSQL -u root tpcc < $BASE_DIR/tpcc-mysql/add_fkey_idx.sql
    LD_LIBRARY_PATH=$BLD_PATH/lib $BASE_DIR/tpcc-mysql/tpcc_load -h127.0.0.1 -dtpcc -uroot -w1

    bash $BASE_DIR/scripts/run_tpcc_threads.sh --$BULID_MODE

done

# Generate plot
echo ""
echo "[INFO] Generating plot..."
bash $SCRIPT_DIR/plots/plot_fig6.sh
