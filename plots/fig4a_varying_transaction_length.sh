#!/bin/bash
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MYSQL_DIR="$REPO_ROOT/MySQL"
POSTGRES_DIR="$REPO_ROOT/PostgreSQL"

# Check required variables
if [[ -z "$rcc_data_dir" ]] || [[ -z "$rcc_log_dir" ]]; then
    echo "[ERROR] rcc_data_dir and rcc_log_dir must be set"
    echo "  export rcc_data_dir=/path/to/data"
    echo "  export rcc_log_dir=/path/to/log"
    exit 1
fi

# ==========================================
# PARSE ARGUMENTS
# ==========================================
RUN_MYSQL=false
RUN_POSTGRES=false
SKIP_PLOT=false

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --mysql       Run MySQL experiments only"
    echo "  --postgres    Run PostgreSQL experiments only"
    echo "  --all         Run both MySQL and PostgreSQL (default)"
    echo "  --skip-plot   Run benchmarks without legacy plot generation"
    echo "  -h, --help    Show this help"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --mysql)    RUN_MYSQL=true; shift ;;
        --postgres) RUN_POSTGRES=true; shift ;;
        --all)      RUN_MYSQL=true; RUN_POSTGRES=true; shift ;;
        --skip-plot) SKIP_PLOT=true; shift ;;
        -h|--help)  usage ;;
        *)          echo "[ERROR] Unknown option: $1"; usage ;;
    esac
done

# Default: run both
if [[ "$RUN_MYSQL" == false && "$RUN_POSTGRES" == false ]]; then
    RUN_MYSQL=true
    RUN_POSTGRES=true
fi

# Get sudo password once
if ! sudo -n true 2>/dev/null && [[ -z "${PASSWD:-}" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
    export PASSWD
fi

RESULT_DIR="$SCRIPT_DIR/fig4_varying_transaction_length/results"
mkdir -p "$RESULT_DIR"

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

# ==========================================
# MYSQL EXPERIMENTS
# ==========================================
if [[ "$RUN_MYSQL" == true ]]; then
    print_header "MySQL fig4a experiments"

    MYSQL_BUILD_MODES=("Vanilla" "RCC")

    for BUILD_MODE in "${MYSQL_BUILD_MODES[@]}"; do
        BUILD_FLAG=$(echo "$BUILD_MODE" | tr '[:upper:]' '[:lower:]')

        MYSQL_HOME="${MYSQL_DIR}/${BUILD_MODE}/mysql-8.4.5"
        BLD_PATH="$MYSQL_HOME-build"
        MYSQL_BIN="${BLD_PATH}/bin/mysqld"
        MYSQL="${BLD_PATH}/bin/mysql"

        print_header "Running MySQL fig4a with $BUILD_MODE build"
        mkdir -p "$RESULT_DIR/mysql/$BUILD_MODE"

        echo "[INFO] Cleaning data_dir and log_dir..."
        rm -rf $rcc_data_dir/* 2>/dev/null
        rm -rf $rcc_log_dir/* 2>/dev/null

        # Build and start MySQL
        echo "[INFO] Building MySQL ($BUILD_MODE mode)..."
        cd "$MYSQL_DIR"
        bash run_server.sh --$BUILD_FLAG

        # Load data and run benchmark
        $MYSQL -u root < "$MYSQL_DIR/synthetic_benchmark/create_table_synthetic.sql"
        python3 "$MYSQL_DIR/synthetic_benchmark/varying_transaction_length.py" \
            | tee "$RESULT_DIR/mysql/$BUILD_MODE/output.log"

        # Shutdown
        cd "$MYSQL_DIR"
        bash shutdown.sh --$BUILD_FLAG
        sleep 3
    done
fi

# ==========================================
# POSTGRESQL EXPERIMENTS
# ==========================================
if [[ "$RUN_POSTGRES" == true ]]; then
    print_header "PostgreSQL fig4a experiments"

    POSTGRESQL_HOME="$POSTGRES_DIR/rcc_postgresql"
    BLD_PATH="${POSTGRESQL_HOME}/bld"
    PAGE_SIZE=16

    POSTGRES_BUILD_MODES=("vanilla" "vanilla_rc" "rcc")

    # Clean rcc_data_dir and rcc_log_dir at the start
    echo "[INFO] Cleaning rcc_data_dir and rcc_log_dir..."
    rm -rf $rcc_data_dir/* 2>/dev/null
    rm -rf $rcc_log_dir/* 2>/dev/null

    for BUILD_MODE in "${POSTGRES_BUILD_MODES[@]}"; do
        print_header "Running PostgreSQL fig4a with $BUILD_MODE build"
        mkdir -p "$RESULT_DIR/postgres/$BUILD_MODE"

        # Clean data_dir and log_dir before each experiment
        echo "[INFO] Cleaning data_dir and log_dir..."
        rm -rf $rcc_data_dir/* 2>/dev/null
        rm -rf $rcc_log_dir/* 2>/dev/null

        # Build PostgreSQL
        echo "[INFO] Building PostgreSQL ($BUILD_MODE mode)..."
        mkdir -p "$POSTGRESQL_HOME/bld"
        cd "$POSTGRESQL_HOME/bld"
        echo $PASSWD | sudo -S rm -rf $BLD_PATH/*
        CONFIG_LOG="$RESULT_DIR/postgres/$BUILD_MODE/configure.log"
        BUILD_LOG="$RESULT_DIR/postgres/$BUILD_MODE/build.log"
        CFLAGS_MODE="-O2"
        [[ "$BUILD_MODE" != rcc ]] || CFLAGS_MODE="-O2 -DRCC"
        if ! ../configure --prefix="$BLD_PATH" --with-blocksize="$PAGE_SIZE" CFLAGS="$CFLAGS_MODE" > "$CONFIG_LOG" 2>&1; then
            tail -n 40 "$CONFIG_LOG"
            echo "[ERROR] Configure failed: $CONFIG_LOG" >&2
            exit 1
        fi
        if ! make -j"${BUILD_JOBS:-16}" > "$BUILD_LOG" 2>&1 ||
           ! make install >> "$BUILD_LOG" 2>&1; then
            tail -n 60 "$BUILD_LOG"
            echo "[ERROR] Build/install failed: $BUILD_LOG" >&2
            exit 1
        fi
        if [[ ! -x "$BLD_PATH/bin/initdb" ]]; then
            echo "[ERROR] Missing $BLD_PATH/bin/initdb" >&2
            exit 1
        fi

        # Load data and run server
        bash "$POSTGRES_DIR/scripts/load_synthetic_data.sh" || exit 1
        bash "$POSTGRES_DIR/run_server.sh" --skip-build

        # Run benchmark
        echo "[INFO] Running synthetic benchmark..."
        ISOLATION=RR
        [[ "$BUILD_MODE" != "vanilla_rc" ]] || ISOLATION=RC
        python3 "$POSTGRES_DIR/synthetic_benchmark/varying_transaction_length.py" --isolation "$ISOLATION" \
            | tee "$RESULT_DIR/postgres/$BUILD_MODE/output.log" || exit 1

        # Shutdown
        bash "$POSTGRES_DIR/util/shutdown.sh" --data-dir $rcc_data_dir
        sleep 3
    done
fi

echo ""
echo "Results saved to: $RESULT_DIR"
[[ "$RUN_MYSQL" == true ]] && echo "  - $RESULT_DIR/mysql/"
[[ "$RUN_POSTGRES" == true ]] && echo "  - $RESULT_DIR/postgres/"

# Generate plot
echo ""
if [[ "$SKIP_PLOT" == false ]]; then
    echo "[INFO] Generating plot..."
    bash "$SCRIPT_DIR/plot_fig4.sh"
fi
