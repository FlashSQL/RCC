#!/bin/bash

# TPC-C Benchmark - Varying Warehouse Count
# Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && ./run_tpcc_warehouses.sh [--results-dir <path>]

# ==========================================
# PATH CONFIGURATION
# ==========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
POSTGRESQL_HOME="$BASE_DIR/rcc_postgresql"
SYSBENCH_HOME="$BASE_DIR/sysbench-tpcc"
# sysbench (built with --with-pgsql) for tpcc.lua's `#!/usr/bin/env sysbench`
export PATH="/home/vldb/sysbench/src:$PATH"
BLD_PATH="${POSTGRESQL_HOME}/bld"
PSQL_BIN="${BLD_PATH}/bin/psql"
PG_BIN="${BLD_PATH}/bin/postgres"
INITDB_BIN="${BLD_PATH}/bin/initdb"

# ==========================================
# CONFIGURATION
# ==========================================
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"
RESULTS_DIR=""

DEFAULT_WARMUP=50
PAGE_SIZE=16
# Configurations to sweep, as <build>_<isolation>:
#   vanilla_rc / vanilla_rr : stock PostgreSQL (-O2) at Read Committed / Repeatable Read
#   rcc_rr                  : RCC (-O2 -DRCC) at Repeatable Read
BUILD_MODES=("vanilla_rc" "vanilla_rr" "rcc_rr")

# NUMA policy, chosen per experiment by set_numa_policy (2 sockets x 48 logical CPUs):
#   up to NUMA_PIN_MAX_THREADS client threads : CPUs and memory pinned to node 0
#   above that                                : all CPUs, memory interleaved over both nodes
# Applied to the server and to sysbench alike.  Empty if numactl is missing.
NUMA_PIN_MAX_THREADS=128
NUMACTL=""
set_numa_policy() {
    local TERMINALS=$1
    command -v numactl >/dev/null 2>&1 || { NUMACTL=""; return; }
    if (( TERMINALS <= NUMA_PIN_MAX_THREADS )); then
        NUMACTL="numactl --cpunodebind=0 --membind=0"
    else
        NUMACTL="numactl --interleave=all"
    fi
}

# Format: "name:warehouses,terminals,duration"
EXPERIMENTS=(
    "s1_t16:1,16,50"
    "s2_t32:2,32,50"
    "s4_t64:4,64,50"
    "s8_t128:8,128,50"
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
    echo "  rcc_data_dir          PostgreSQL data directory"
    echo "  rcc_log_dir           PostgreSQL log directory"
    echo ""
    echo "Options:"
    echo "  --results-dir <path>  Results directory"
    echo "  -h, --help            Show this help"
    exit 0
}

# ==========================================
# POSTGRESQL FUNCTIONS
# ==========================================
stop_postgresql() {
    print_info "Stopping PostgreSQL..."
    pkill -f "postgres -D" 2>/dev/null
    sleep 3
}

start_postgresql() {
    local WAREHOUSES=$1
    local RESULT_DIR=$2
    local BUILD_MODE=$3

    print_header "PostgreSQL Server Startup ($BUILD_MODE, Warehouses: $WAREHOUSES)"

    stop_postgresql

    # Clean data_dir and log_dir before each experiment
    print_info "Cleaning data_dir and log_dir..."
    rm -rf $DATA_DIR/* 2>/dev/null
    rm -rf $LOG_DIR/* 2>/dev/null

    print_info "Building PostgreSQL ($BUILD_MODE mode)..."
    mkdir -p $POSTGRESQL_HOME/bld
    cd $POSTGRESQL_HOME/bld
    echo $PASSWD | sudo -S rm -rf $BLD_PATH/*
    make clean > /dev/null 2>&1

    BUILD_LOG="$RESULT_DIR/build.log"
    CONFIG_LOG="$RESULT_DIR/config.log"

    if [[ "$BUILD_MODE" == vanilla* ]]; then
        ../configure --prefix=$POSTGRESQL_HOME/bld --with-blocksize=$PAGE_SIZE CFLAGS="-O2" > "$CONFIG_LOG" 2>&1
    else
        ../configure --prefix=$POSTGRESQL_HOME/bld --with-blocksize=$PAGE_SIZE CFLAGS="-O2 -DRCC" > "$CONFIG_LOG" 2>&1
    fi

    make clean >> "$BUILD_LOG" 2>&1
    echo $PASSWD | sudo -S make -j install >> "$BUILD_LOG" 2>&1

    if [ $? -ne 0 ]; then
        print_error "Build failed"
        return 1
    fi

    print_info "Initializing database..."
    cd $POSTGRESQL_HOME
    $INITDB_BIN -D $DATA_DIR -U postgres > /dev/null 2>&1

    cp $BASE_DIR/postgresql.conf $DATA_DIR/postgresql.conf
    sed -i "s|log_directory = '.*'|log_directory = '$LOG_DIR'|" $DATA_DIR/postgresql.conf

    print_info "Starting PostgreSQL..."
    $NUMACTL $PG_BIN -D $DATA_DIR &
    sleep 5

    $PSQL_BIN -U postgres -c "CREATE DATABASE tpcc OWNER postgres;" 2>/dev/null
    $PSQL_BIN -U postgres -c "GRANT ALL PRIVILEGES ON DATABASE tpcc TO postgres;" 2>/dev/null

    print_info "Loading TPC-C data (warehouses=$WAREHOUSES)..."
    cd $SYSBENCH_HOME
    $NUMACTL ./tpcc.lua --pgsql-user=postgres --pgsql-db=tpcc \
        --threads=$WAREHOUSES --tables=1 --scale=$WAREHOUSES \
        --use_fk=0 --trx_level=RR --partition=yes --db-driver=pgsql prepare > /dev/null 2>&1

    $PSQL_BIN -U postgres -d tpcc -c "VACUUM ANALYZE;" 2>/dev/null
    $PSQL_BIN -U postgres -d tpcc -c "SELECT pg_stat_reset();" 2>/dev/null

    print_info "PostgreSQL ready!"
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

    mkdir -p $RESULT_DIR

    print_header "Experiment: $NAME ($BUILD_MODE)"
    echo "  Warehouses: $WAREHOUSES, Terminals: $TERMINALS, Duration: ${DURATION}s"

    set_numa_policy $TERMINALS
    print_info "NUMA policy: ${NUMACTL:-none}"

    start_postgresql $WAREHOUSES $RESULT_DIR $BUILD_MODE
    if [ $? -ne 0 ]; then
        return 1
    fi

    # isolation level follows the configuration name (see BUILD_MODES)
    local TRX
    case "$BUILD_MODE" in
        *_rc) TRX="RC" ;;
        *)    TRX="RR" ;;
    esac

    cd $SYSBENCH_HOME

    # Warmup as a SEPARATE run (discarded), so server-side statistics cover
    # only the measured run.
    print_info "Warming up... (${DEFAULT_WARMUP}s, trx_level=$TRX, not measured)"
    $NUMACTL ./tpcc.lua --pgsql-user=postgres --pgsql-db=tpcc \
        --time=$DEFAULT_WARMUP \
        --threads=$TERMINALS --report-interval=1 --retry_on_fail=yes --backoff_on=0 \
        --tables=1 --scale=$WAREHOUSES --use_fk=0 --trx_level=$TRX \
        --partition=yes --db-driver=pgsql run > $RESULT_DIR/warmup.log 2>&1
    $PSQL_BIN -U postgres -d tpcc -c "SELECT pg_stat_reset();" > /dev/null 2>&1

    print_info "Running Sysbench TPC-C... (trx_level=$TRX, ${DURATION}s measured)"
    $NUMACTL ./tpcc.lua --pgsql-user=postgres --pgsql-db=tpcc \
        --time=$DURATION \
        --threads=$TERMINALS --report-interval=1 --retry_on_fail=yes --backoff_on=0 \
        --tables=1 --scale=$WAREHOUSES --use_fk=0 --trx_level=$TRX \
        --partition=yes --db-driver=pgsql run > $RESULT_DIR/sysbench.log 2>&1

    # TPC-C Clause 3.3 consistency check on the final data, before the server stops.
    print_info "Running consistency check..."
    $PSQL_BIN -U postgres -d tpcc -f "$SCRIPT_DIR/consistency_check.sql" \
        > $RESULT_DIR/consistency.txt 2>&1

    # Save summary
    echo "=== Experiment Summary ===" > $RESULT_DIR/summary.txt
    echo "Build Mode: $BUILD_MODE" >> $RESULT_DIR/summary.txt
    echo "Isolation: $TRX" >> $RESULT_DIR/summary.txt
    echo "NUMA: ${NUMACTL:-none}" >> $RESULT_DIR/summary.txt
    echo "Warehouses: $WAREHOUSES" >> $RESULT_DIR/summary.txt
    echo "Terminals: $TERMINALS" >> $RESULT_DIR/summary.txt
    echo "Duration: ${DURATION}s" >> $RESULT_DIR/summary.txt

    stop_postgresql
    rm -f $LOG_DIR/*.log 2>/dev/null
    rm -rf $DATA_DIR/* 2>/dev/null
    rm -f $RESULT_DIR/build.log $RESULT_DIR/config.log 2>/dev/null

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
fi

print_header "TPC-C Benchmark - Varying Warehouse Count"
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
