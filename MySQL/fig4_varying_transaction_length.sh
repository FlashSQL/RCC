#!/bin/bash

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

RESULT_DIR="$BASE_DIR/fig4_varying_transaction_length/results"
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

    print_header "Running fig4 with $BUILD_MODE build"
    mkdir -p $RESULT_DIR/$BUILD_MODE

    echo "[INFO] Cleaning data_dir and log_dir..."
    rm -rf $rcc_data_dir/* 2>/dev/null
    rm -rf $rcc_log_dir/* 2>/dev/null

    # Build MySQL
    echo "[INFO] Building MySQL ($BUILD_MODE mode)..."
    bash run_server.sh --$BUILD_MODE

    # Load data and run server
    $MYSQL -u root < $BASE_DIR/synthetic_benchmark/create_table_synthetic.sql
    python3 $BASE_DIR/synthetic_benchmark/varying_transaction_length.py | tee $RESULT_DIR/$BUILD_MODE/output.log

    # shutdown
    bash shutdown.sh --$BUILD_MODE
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
