#!/bin/bash

# PostgreSQL YCSB - Varying Client Threads (paper Fig. 9(a))
# YCSB 10M rows (HASH(id), 128 partitions), zipf theta 0.9, 8 reads + 8 updates per transaction in ascending key order.
# RCC at Repeatable Read vs. vanilla PostgreSQL at Read Committed / Repeatable Read.
# Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && ./fig9a_ycsb_threads.sh [--results-dir <path>]

# ==========================================
# PATH CONFIGURATION
# ==========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
POSTGRESQL_HOME="$BASE_DIR/rcc_postgresql"
SYSBENCH_HOME="$BASE_DIR/sysbench-ycsb"
# sysbench built with --with-pgsql
export PATH="${SYSBENCH_BIN_DIR:-$HOME/sysbench/src}:$PATH"
BLD_PATH="${POSTGRESQL_HOME}/bld"
PSQL_BIN="${BLD_PATH}/bin/psql"
PG_BIN="${BLD_PATH}/bin/postgres"
INITDB_BIN="${BLD_PATH}/bin/initdb"
# Use the original shared-node placement for both server and clients.
NUMA_PIN_MAX_THREADS=128
NUMACTL=""
set_numa_policy() {
    local THREADS=$1
    command -v numactl >/dev/null 2>&1 || { NUMACTL=""; return; }
    if (( THREADS <= NUMA_PIN_MAX_THREADS )); then
        NUMACTL="numactl --cpunodebind=0 --membind=0"
    else
        NUMACTL="numactl --interleave=all"
    fi
}

# ==========================================
# CONFIGURATION
# ==========================================
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"
RESULTS_DIR="$BASE_DIR/results/Fig9a"

DEFAULT_WARMUP=50
PAGE_SIZE=16
TABLE_SIZE=10000000
PARTITIONS=128
ZIPF_THETA=0.9
READS=8
UPDATES=8
# Configurations to sweep, as <build>_<isolation>:
#   vanilla_rc / vanilla_rr : stock PostgreSQL (-O2) at Read Committed / Repeatable Read
#   rcc_rr                  : RCC (-O2 -DRCC) at Repeatable Read
BUILD_MODES=("rcc_rr" "vanilla_rc" "vanilla_rr")

# Format: "name:threads,duration"
EXPERIMENTS=(
    "t1:1,50"
    "t16:16,50"
    "t32:32,50"
    "t64:64,50"
    "t96:96,50"
    "t128:128,50"
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
    echo "  --results-dir <path>  Results directory (default: results/Fig9a)"
    echo "  -h, --help            Show this help"
    exit 0
}

isolation_of() {
    case "$1" in
        *_rc) echo "read committed" ;;
        *)    echo "repeatable read" ;;
    esac
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
    local RESULT_DIR=$1
    local BUILD_MODE=$2

    print_header "PostgreSQL Server Startup ($BUILD_MODE)"

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
    # Isolation level of every transaction; RCC: the asynchronous deadlock detector runs in the background
    # (vanilla builds do not know rcc_bg_detect)
    local PG_OPTS=(-c "default_transaction_isolation=$(isolation_of $BUILD_MODE)")
    if [[ "$BUILD_MODE" == rcc* ]]; then PG_OPTS+=(-c rcc_bg_detect=on); fi
    $NUMACTL $PG_BIN -D $DATA_DIR "${PG_OPTS[@]}" &
    sleep 5

    $PSQL_BIN -U postgres -c "CREATE DATABASE sbtest OWNER postgres;" 2>/dev/null

    print_info "Loading YCSB data ($TABLE_SIZE rows, $PARTITIONS partitions)..."
    cd $SYSBENCH_HOME
    $NUMACTL sysbench ycsb_like.lua --db-driver=pgsql --pgsql-user=postgres --pgsql-db=sbtest \
        --tables=1 --table_size=$TABLE_SIZE --hash_partitions=$PARTITIONS prepare > $RESULT_DIR/load.log 2>&1

    $PSQL_BIN -U postgres -d sbtest -c "VACUUM ANALYZE;" 2>/dev/null
    $PSQL_BIN -U postgres -d sbtest -c "SELECT pg_stat_reset();" 2>/dev/null

    print_info "PostgreSQL ready!"
    return 0
}

# ==========================================
# BENCHMARK FUNCTION
# ==========================================
run_ycsb() {   # <threads> <seconds> <log>
    # aborted transactions retry at once (no backoff), as in the TPC-C script (--backoff_on=0)
    SB_SORT=1 SB_BACKOFF_US=0 $NUMACTL sysbench ycsb_like.lua --db-driver=pgsql --pgsql-user=postgres --pgsql-db=sbtest \
        --tables=1 --table_size=$TABLE_SIZE --point_selects=$READS --non_index_updates=$UPDATES \
        --rand-type=zipfian --rand-zipfian-exp=$ZIPF_THETA \
        --threads=$1 --time=$2 --report-interval=1 run > $3 2>&1
}

run_experiment() {
    local NAME=$1
    local THREADS=$2
    local DURATION=$3
    local RESULT_DIR=$4
    local BUILD_MODE=$5

    mkdir -p $RESULT_DIR

    print_header "Experiment: $NAME ($BUILD_MODE)"
    echo "  Threads: $THREADS, Duration: ${DURATION}s, Isolation: $(isolation_of $BUILD_MODE)"

    set_numa_policy $THREADS
    print_info "NUMA policy: ${NUMACTL:-none}"

    start_postgresql $RESULT_DIR $BUILD_MODE
    if [ $? -ne 0 ]; then
        print_error "Failed to start PostgreSQL"
        return 1
    fi

    cd $SYSBENCH_HOME

    # Warmup as a separate run (discarded): the measured run starts on a warmed-up server.
    print_info "Warming up... (${DEFAULT_WARMUP}s, not measured)"
    run_ycsb $THREADS $DEFAULT_WARMUP $RESULT_DIR/warmup.log

    $PSQL_BIN -U postgres -d sbtest -c "SELECT pg_stat_reset();" > /dev/null 2>&1
    for f in "$LOG_DIR"/*; do : > "$f"; done 2>/dev/null

    print_info "Running Sysbench YCSB... (${DURATION}s measured)"
    run_ycsb $THREADS $DURATION $RESULT_DIR/sysbench.log

    # Save summary
    echo "=== Experiment Summary ===" > $RESULT_DIR/summary.txt
    echo "Build Mode: $BUILD_MODE" >> $RESULT_DIR/summary.txt
    echo "Isolation: $(isolation_of $BUILD_MODE)" >> $RESULT_DIR/summary.txt
    echo "Threads: $THREADS" >> $RESULT_DIR/summary.txt
    echo "Duration: ${DURATION}s (warmup ${DEFAULT_WARMUP}s)" >> $RESULT_DIR/summary.txt
    echo "Table: $TABLE_SIZE rows, HASH(id) $PARTITIONS partitions, zipf $ZIPF_THETA, ${READS}R+${UPDATES}W sorted" >> $RESULT_DIR/summary.txt
    echo "Backoff: off" >> $RESULT_DIR/summary.txt
    echo "rcc_bg_detect: $($PSQL_BIN -U postgres -At -c 'SHOW rcc_bg_detect' 2>/dev/null || echo n/a)" >> $RESULT_DIR/summary.txt

    stop_postgresql
    # Save the PostgreSQL server log into this run's result dir before it gets wiped.
    cp $LOG_DIR/*.log $RESULT_DIR/ 2>/dev/null
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

# Make results dir absolute: build runs from bld/, so relative log paths would break
mkdir -p "$RESULTS_DIR"
RESULTS_DIR="$(cd "$RESULTS_DIR" && pwd)"

# Get sudo password (from env or prompt)
if ! sudo -n true 2>/dev/null && [[ -z "${PASSWD:-}" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
fi

print_header "YCSB Benchmark - Varying Client Threads"
echo "Results: $RESULTS_DIR"

TOTAL=0
SUCCESS=0

for BUILD_MODE in "${BUILD_MODES[@]}"; do
    for exp in "${EXPERIMENTS[@]}"; do
        NAME=$(echo $exp | cut -d':' -f1)
        PARAMS=$(echo $exp | cut -d':' -f2)
        THREADS=$(echo $PARAMS | cut -d',' -f1)
        DURATION=$(echo $PARAMS | cut -d',' -f2)

        TOTAL=$((TOTAL + 1))
        if run_experiment "$NAME" $THREADS $DURATION "$RESULTS_DIR/$BUILD_MODE/$NAME" $BUILD_MODE; then
            SUCCESS=$((SUCCESS + 1))
        fi
        sleep 10
    done
done

print_header "SUMMARY"
echo "Total: $TOTAL, Success: $SUCCESS, Failed: $((TOTAL - SUCCESS))"
