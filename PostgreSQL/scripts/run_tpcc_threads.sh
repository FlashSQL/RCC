#!/bin/bash

# TPC-C Benchmark - Varying Client Threads
# Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && ./run_tpcc_threads.sh [--results-dir <path>]

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
#   sr                      : RCC_SR (-O2 -DRCC -DRCC_SR) at Serializable
BUILD_MODES=("vanilla_rc" "vanilla_rr" "rcc_rr")
# BUILD_MODES=("sr")

# Format: "name:warehouses,terminals,duration"
EXPERIMENTS=(
    "s1_t1:1,1,50"
    "s1_t8:1,8,50"
    "s1_t16:1,16,50"
    "s1_t32:1,32,50"
    "s1_t64:1,64,50"
    "s1_t128:1,128,50"
    "s1_t256:1,256,50"
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
    elif [[ "$BUILD_MODE" == "sr" ]]; then
        ../configure --prefix=$POSTGRESQL_HOME/bld --with-blocksize=$PAGE_SIZE CFLAGS="-O2 -DRCC -DRCC_SR" > "$CONFIG_LOG" 2>&1
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
        *_rr) TRX="RR" ;;
        *)    TRX="SER" ;;
    esac

    cd $SYSBENCH_HOME

    # Warmup as a SEPARATE run (discarded).  sysbench's --warmup-time only excludes
    # warmup from its own tps, but our in-server pgstat breakdown counters
    # (committed/aborted_total_time, txwait, etc.) accumulate from server start and
    # would otherwise include the warmup transactions.  Running warmup separately
    # gives a clean boundary at which to reset those counters.
    print_info "Warming up... (${DEFAULT_WARMUP}s, trx_level=$TRX, not measured)"
    $NUMACTL ./tpcc.lua --pgsql-user=postgres --pgsql-db=tpcc \
        --time=$DEFAULT_WARMUP \
        --threads=$TERMINALS --report-interval=1 --retry_on_fail=yes --backoff_on=0 \
        --tables=1 --scale=$WAREHOUSES --use_fk=0 --trx_level=$TRX \
        --partition=yes --db-driver=pgsql run > $RESULT_DIR/warmup.log 2>&1

    # Reset PG-side stats so the breakdown counters cover ONLY the measured window.
    $PSQL_BIN -U postgres -d tpcc -c "SELECT pg_stat_reset();" > /dev/null 2>&1

    # Truncate server logs so postgresql.log covers ONLY the measured window
    # (abort-classification counts would otherwise include warmup).  Truncation
    # is safe while the logger holds the file open (append mode).
    for f in "$LOG_DIR"/*; do : > "$f"; done 2>/dev/null

    print_info "Running Sysbench TPC-C... (trx_level=$TRX, ${DURATION}s measured)"
    $NUMACTL ./tpcc.lua --pgsql-user=postgres --pgsql-db=tpcc \
        --time=$DURATION \
        --threads=$TERMINALS --report-interval=1 --retry_on_fail=yes --backoff_on=0 \
        --tables=1 --scale=$WAREHOUSES --use_fk=0 --trx_level=$TRX \
        --partition=yes --db-driver=pgsql run > $RESULT_DIR/sysbench.log 2>&1

    # Save summary
    echo "=== Experiment Summary ===" > $RESULT_DIR/summary.txt
    echo "Build Mode: $BUILD_MODE" >> $RESULT_DIR/summary.txt
    echo "Warehouses: $WAREHOUSES" >> $RESULT_DIR/summary.txt
    echo "Terminals: $TERMINALS" >> $RESULT_DIR/summary.txt
    echo "Duration: ${DURATION}s" >> $RESULT_DIR/summary.txt

    # Breakdown: capture per-db wait/total-time counters BEFORE stopping (pg_stat is in-memory)
    $PSQL_BIN -U postgres -d tpcc -At -F'=' > $RESULT_DIR/breakdown.txt 2>/dev/null <<'SQL'
SELECT 'committed_total_ms', pg_stat_get_db_committed_total_time(d.oid) FROM pg_database d WHERE datname='tpcc'
UNION ALL SELECT 'aborted_total_ms', pg_stat_get_db_aborted_total_time(d.oid) FROM pg_database d WHERE datname='tpcc'
UNION ALL SELECT 'lock_wait_ms',   pg_stat_get_db_tuplock_wait_time(d.oid)      FROM pg_database d WHERE datname='tpcc'
UNION ALL SELECT 'spin_wait_ms',   pg_stat_get_db_tuple_update_wait_time(d.oid) FROM pg_database d WHERE datname='tpcc'
UNION ALL SELECT 'commit_wait_ms', pg_stat_get_db_txwait_time(d.oid)            FROM pg_database d WHERE datname='tpcc'
UNION ALL SELECT 'xact_commit',   xact_commit::float8   FROM pg_stat_database WHERE datname='tpcc'
UNION ALL SELECT 'xact_rollback', xact_rollback::float8 FROM pg_stat_database WHERE datname='tpcc';
SQL

    # HOT vs non-HOT update counts (scenario-2 trigger frequency) + relid->name
    # map for decoding [RCC_fanout] log lines.  pg_stat is in-memory: must run
    # before the server stops.
    $PSQL_BIN -U postgres -d tpcc -c "SELECT relname, n_tup_upd, n_tup_hot_upd, n_tup_upd - n_tup_hot_upd AS non_hot FROM pg_stat_user_tables ORDER BY n_tup_upd DESC;" \
        > $RESULT_DIR/hot_stats.txt 2>&1
    $PSQL_BIN -U postgres -d tpcc -c "SELECT c.oid, c.relname, c.relkind FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'public' ORDER BY c.oid;" \
        > $RESULT_DIR/relmap.txt 2>&1

    # TPC-C Clause 3.3 consistency check on the final data, before the server stops.
    print_info "Running consistency check..."
    $PSQL_BIN -U postgres -d tpcc -f "$SCRIPT_DIR/consistency_check.sql" \
        > $RESULT_DIR/consistency.txt 2>&1

    stop_postgresql
    # Save the PostgreSQL server log (ERRORs, serialization failures, not-found, etc.)
    # into this run's result dir before it gets wiped.
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
if [[ -n "$RESULTS_DIR" ]]; then
    mkdir -p "$RESULTS_DIR"
    RESULTS_DIR="$(cd "$RESULTS_DIR" && pwd)"
fi

# Get sudo password (from env or prompt)
if [[ -z "$PASSWD" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
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

# Latency breakdown (Execution / Lock wait / Commit wait / Wasted) per experiment
for BUILD_MODE in "${BUILD_MODES[@]}"; do
    print_header "LATENCY BREAKDOWN ($BUILD_MODE)"
    python3 "$SCRIPT_DIR/breakdown_report.py" "$RESULTS_DIR/$BUILD_MODE" \
        | tee "$RESULTS_DIR/$BUILD_MODE/breakdown_report.txt"
done
