#!/bin/bash

# TPC-C Benchmark - Varying Client Threads
# Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && ./run_tpcc_threads.sh [--results-dir <path>]

# ==========================================
# PATH CONFIGURATION
# ==========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TPCC_DIR="$BASE_DIR/tpcc-mysql"

# ==========================================
# CONFIGURATION
# ==========================================
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"
RESULTS_DIR=""

DEFAULT_WARMUP=50
BUILD_MODES=("Vanilla" "RCC" "RCC-W" "RCC-D" "RCC-C")

# Format: "name:warehouses,terminals,duration"
EXPERIMENTS=(
    "s1_t1:1,1,50"
    "s1_t8:1,8,50"
    "s1_t16:1,16,50"
    "s1_t32:1,32,50"
    "s1_t64:1,64,50"
    "s1_t128:1,128,50"
)

# ==========================================
# HELPER FUNCTIONS
# ==========================================
print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

print_info() {
    echo "[INFO] $1"
}

print_error() {
    echo "[ERROR] $1"
}

usage() {
    echo "Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && $0 [--results-dir <path>]"
    echo ""
    echo "Required environment variables:"
    echo "  rcc_data_dir          MySQL data directory"
    echo "  rcc_log_dir           MySQL log directory"
    echo ""
    echo "Options:"
    echo "  --results-dir <path>  Results directory"
    echo "  -h, --help            Show this help"
    exit 0
}

# ==========================================
# MYSQL FUNCTIONS
# ==========================================
stop_mysql() {
    local BUILD_MODE=$1
    print_info "Stopping MySQL..."
    local MYSQLADMIN="$BASE_DIR/$BUILD_MODE/mysql-8.4.5-build/bin/mysqladmin"
    $MYSQLADMIN -uroot shutdown 2>/dev/null
    pkill -9 -ef mysqld 2>/dev/null
    sleep 3
}

start_mysql() {
    local BUILD_MODE=$1
    local WAREHOUSES=$2
    local BUILD_FLAG=$(echo "$BUILD_MODE" | tr '[:upper:]' '[:lower:]')

    local MYSQL_HOME="$BASE_DIR/$BUILD_MODE/mysql-8.4.5"
    local BLD_PATH="$MYSQL_HOME-build"
    local MYSQL="$BLD_PATH/bin/mysql"

    print_header "MySQL Server Startup ($BUILD_MODE, Warehouses: $WAREHOUSES)"

    stop_mysql "$BUILD_MODE"

    print_info "Cleaning data_dir and log_dir..."
    rm -rf $DATA_DIR/* 2>/dev/null
    rm -rf $LOG_DIR/* 2>/dev/null

    print_info "Building and starting MySQL ($BUILD_MODE mode)..."
    cd "$BASE_DIR"
    bash run_server.sh --$BUILD_FLAG

    print_info "Building TPC-C binaries..."
    cd "$TPCC_DIR/src" && make

    print_info "Loading TPC-C data (warehouses=$WAREHOUSES)..."
    $MYSQL -uroot -e "CREATE DATABASE tpcc;"
    $MYSQL -uroot tpcc < "$TPCC_DIR/create_table.sql"
    $MYSQL -uroot tpcc < "$TPCC_DIR/add_fkey_idx.sql"
    LD_LIBRARY_PATH="$BLD_PATH/lib" "$TPCC_DIR/tpcc_load" \
        -h127.0.0.1 -dtpcc -uroot -w$WAREHOUSES

    print_info "MySQL ready!"
    return 0
}

# ==========================================
# BENCHMARK FUNCTION
# ==========================================
run_experiment() {
    local NAME=$1
    local WAREHOUSES=$2
    local TERMINALS=$3
    local DURATION=$4
    local RESULT_DIR=$5
    local BUILD_MODE=$6

    local BLD_PATH="$BASE_DIR/$BUILD_MODE/mysql-8.4.5-build"

    mkdir -p "$RESULT_DIR"

    print_header "Experiment: $NAME ($BUILD_MODE)"
    echo "  Warehouses: $WAREHOUSES, Terminals: $TERMINALS, Duration: ${DURATION}s"

    start_mysql "$BUILD_MODE" "$WAREHOUSES"
    if [ $? -ne 0 ]; then
        return 1
    fi

    print_info "Running TPC-C..."
    LD_LIBRARY_PATH="$BLD_PATH/lib" \
        "$TPCC_DIR/tpcc_start" -S /tmp/mysql.sock -d tpcc -u root \
        -w "$WAREHOUSES" -c "$TERMINALS" -r "$DEFAULT_WARMUP" -l "$DURATION" \
        > "$RESULT_DIR/sysbench.log" 2>&1

    echo "=== Experiment Summary ===" > "$RESULT_DIR/summary.txt"
    echo "Build Mode: $BUILD_MODE" >> "$RESULT_DIR/summary.txt"
    echo "Warehouses: $WAREHOUSES" >> "$RESULT_DIR/summary.txt"
    echo "Terminals: $TERMINALS" >> "$RESULT_DIR/summary.txt"
    echo "Duration: ${DURATION}s" >> "$RESULT_DIR/summary.txt"

    stop_mysql "$BUILD_MODE"
    rm -rf $DATA_DIR/* 2>/dev/null
    rm -rf $LOG_DIR/* 2>/dev/null

    return 0
}

# ==========================================
# MAIN
# ==========================================
while [[ $# -gt 0 ]]; do
    case $1 in
        --results-dir) RESULTS_DIR="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) print_error "Unknown option: $1"; usage ;;
    esac
done

# Validate required env vars
if [[ -z "$DATA_DIR" ]] || [[ -z "$LOG_DIR" ]]; then
    print_error "rcc_data_dir and rcc_log_dir are required"
    usage
fi

# Get sudo password (from env or prompt)
if [[ -z "$PASSWD" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
    export PASSWD
fi

print_header "TPC-C Benchmark - Varying Client Threads"
echo "Results: $RESULTS_DIR"

TOTAL=0
SUCCESS=0

for BUILD_MODE in "${BUILD_MODES[@]}"; do
    for exp in "${EXPERIMENTS[@]}"; do
        NAME=$(echo $exp | cut -d':' -f1)
        PARAMS=$(echo $exp | cut -d':' -f2)
        WAREHOUSES=$(echo $PARAMS | cut -d',' -f1)
        TERMINALS=$(echo $PARAMS | cut -d',' -f2)
        DURATION=$(echo $PARAMS | cut -d',' -f3)

        TOTAL=$((TOTAL + 1))
        if run_experiment "$NAME" $WAREHOUSES $TERMINALS $DURATION "$RESULTS_DIR/$BUILD_MODE/$NAME" $BUILD_MODE; then
            SUCCESS=$((SUCCESS + 1))
        fi
        sleep 10
    done
done

print_header "SUMMARY"
echo "Total: $TOTAL, Success: $SUCCESS, Failed: $((TOTAL - SUCCESS))"
