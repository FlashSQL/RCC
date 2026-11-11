#!/bin/bash

# PostgreSQL Server Start Script
# Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && ./run_server.sh

# ==========================================
# PATH CONFIGURATION
# ==========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POSTGRESQL_HOME="$SCRIPT_DIR/rcc_postgresql"
BLD_PATH="${POSTGRESQL_HOME}/bld"
PSQL_BIN="${BLD_PATH}/bin/psql"
PG_BIN="${BLD_PATH}/bin/postgres"
INITDB_BIN="${BLD_PATH}/bin/initdb"

# ==========================================
# DEFAULT CONFIGURATION
# ==========================================
PAGE_SIZE=16
BUILD_MODE="rr"

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
    echo "Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && $0 [options]"
    echo ""
    echo "Required environment variables:"
    echo "  rcc_data_dir          PostgreSQL data directory"
    echo "  rcc_log_dir           PostgreSQL log directory"
    echo ""
    echo "Options:"
    echo "  --vanilla             Build vanilla PostgreSQL (without RCC)"
    echo "  --rr                  Build RCC for Repeatable Read experiments (-DRCC) [default]"
    echo "  --skip-build          Skip build step (use existing binaries)"
    echo "  --numa <mode>         Server NUMA policy: pin (CPUs+memory on node 0) [default],"
    echo "                        interleave (all CPUs, memory interleaved; use for 256 threads), none"
    echo "  -h, --help            Show this help"
    exit 0
}

stop_postgresql() {
    print_info "Stopping PostgreSQL..."
    pkill -f "postgres -D" 2>/dev/null
    sleep 3
}

# ==========================================
# PARSE ARGUMENTS
# ==========================================
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"
SKIP_BUILD=false
NUMA_MODE="pin"

while [[ $# -gt 0 ]]; do
    case $1 in
        --vanilla)
            BUILD_MODE="vanilla"
            shift
            ;;
        --rr)
            BUILD_MODE="rr"
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --numa)
            NUMA_MODE="$2"
            if [[ "$NUMA_MODE" != "pin" && "$NUMA_MODE" != "interleave" && "$NUMA_MODE" != "none" ]]; then
                print_error "Unknown --numa mode: $NUMA_MODE"
                usage
            fi
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            ;;
    esac
done

# Validate required env vars
if [[ -z "$DATA_DIR" ]]; then
    print_error "rcc_data_dir is required"
    usage
fi

if [[ -z "$LOG_DIR" ]]; then
    print_error "rcc_log_dir is required"
    usage
fi

# Get sudo password (from env or prompt)
if ! sudo -n true 2>/dev/null && [[ -z "${PASSWD:-}" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
fi

# ==========================================
# MAIN EXECUTION
# ==========================================
print_header "PostgreSQL Build and Start"

# Stop existing
stop_postgresql

# Build PostgreSQL
if [[ "$SKIP_BUILD" == false ]]; then
    print_info "Building PostgreSQL ($BUILD_MODE mode)..."
    mkdir -p $BLD_PATH
    cd $POSTGRESQL_HOME/bld
    echo $PASSWD | sudo -S rm -rf $BLD_PATH/*
    make clean > /dev/null 2>&1

    BUILD_LOG="$SCRIPT_DIR/build.log"
    CONFIG_LOG="$SCRIPT_DIR/config.log"

    if [[ "$BUILD_MODE" == "vanilla" ]]; then
        ../configure --prefix=$BLD_PATH --with-blocksize=$PAGE_SIZE CFLAGS="-O2" > "$CONFIG_LOG" 2>&1
    else
        ../configure --prefix=$BLD_PATH --with-blocksize=$PAGE_SIZE CFLAGS="-O2 -DRCC" > "$CONFIG_LOG" 2>&1
    fi
    make clean >> "$BUILD_LOG" 2>&1
    echo $PASSWD | sudo -S make -j install >> "$BUILD_LOG" 2>&1
    print_info "Build log: $BUILD_LOG"
    print_info "Config log: $CONFIG_LOG"

    if [ $? -ne 0 ]; then
        print_error "Build failed. Check $BUILD_LOG"
        exit 1
    fi
else
    print_info "Skipping build (using existing binaries)..."
fi

# Initialize database (only if DATA_DIR is empty)
if [ -z "$(ls -A $DATA_DIR 2>/dev/null)" ]; then
    print_info "Initializing database..."
    cd $POSTGRESQL_HOME
    $INITDB_BIN -D $DATA_DIR -U postgres > /dev/null 2>&1
    # Apply the project's tuned configuration (logging_collector, etc.)
    cp "$SCRIPT_DIR/postgresql.conf" "$DATA_DIR/postgresql.conf"
    # Point log_directory at the requested LOG_DIR
    sed -i "s|^log_directory = .*|log_directory = '$LOG_DIR'|" $DATA_DIR/postgresql.conf
else
    print_info "Using existing data directory..."
fi

# Start PostgreSQL
print_info "Starting PostgreSQL..."
NUMACTL=""
if command -v numactl >/dev/null 2>&1; then
    case "$NUMA_MODE" in
        pin)        NUMACTL="numactl --cpunodebind=0 --membind=0" ;;
        interleave) NUMACTL="numactl --interleave=all" ;;
        none)       NUMACTL="" ;;
    esac
fi
$NUMACTL $PG_BIN -D $DATA_DIR &
sleep 5

# Cleanup build logs
rm -f $SCRIPT_DIR/build.log $SCRIPT_DIR/config.log 2>/dev/null

print_header "PostgreSQL Ready"
echo "  Build Mode:  $BUILD_MODE"
echo "  NUMA:        ${NUMACTL:-none}"
echo "  Data Dir:    $DATA_DIR"
echo "  Log Dir:     $LOG_DIR"
echo "  Page Size:   ${PAGE_SIZE}KB"
echo ""
echo "To stop: pkill -f 'postgres -D'"
echo "To connect: $PSQL_BIN -U postgres"
