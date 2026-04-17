#!/bin/bash

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

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --mysql       Run MySQL experiments only"
    echo "  --postgres    Run PostgreSQL experiments only"
    echo "  --all         Run both MySQL and PostgreSQL (default)"
    echo "  -h, --help    Show this help"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --mysql)    RUN_MYSQL=true; shift ;;
        --postgres) RUN_POSTGRES=true; shift ;;
        --all)      RUN_MYSQL=true; RUN_POSTGRES=true; shift ;;
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
if [[ -z "$PASSWD" ]]; then
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
    print_header "MySQL fig4 experiments"

    MYSQL_BUILD_MODES=("Vanilla" "RCC")

    for BUILD_MODE in "${MYSQL_BUILD_MODES[@]}"; do
        BUILD_FLAG=$(echo "$BUILD_MODE" | tr '[:upper:]' '[:lower:]')

        MYSQL_HOME="${MYSQL_DIR}/${BUILD_MODE}/mysql-8.4.5"
        BLD_PATH="$MYSQL_HOME-build"
        MYSQL_BIN="${BLD_PATH}/bin/mysqld"
        MYSQL="${BLD_PATH}/bin/mysql"

        print_header "Running MySQL fig4 with $BUILD_MODE build"
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
    print_header "PostgreSQL fig4 experiments"

    POSTGRESQL_HOME="$POSTGRES_DIR/rcc_postgresql"
    BLD_PATH="${POSTGRESQL_HOME}/bld"
    PAGE_SIZE=16

    POSTGRES_BUILD_MODES=("vanilla" "rcc")

    # Clean rcc_data_dir and rcc_log_dir at the start
    echo "[INFO] Cleaning rcc_data_dir and rcc_log_dir..."
    rm -rf $rcc_data_dir/* 2>/dev/null
    rm -rf $rcc_log_dir/* 2>/dev/null

    for BUILD_MODE in "${POSTGRES_BUILD_MODES[@]}"; do
        print_header "Running PostgreSQL fig4 with $BUILD_MODE build"
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
        make clean > /dev/null 2>&1

        if [[ "$BUILD_MODE" == "vanilla" ]]; then
            ../configure --prefix=$POSTGRESQL_HOME/bld --with-blocksize=$PAGE_SIZE CFLAGS="-O2" > /dev/null 2>&1
        else
            ../configure --prefix=$POSTGRESQL_HOME/bld --with-blocksize=$PAGE_SIZE CFLAGS="-O2 -DRCC" > /dev/null 2>&1
        fi

        make -j > /dev/null 2>&1
        make install > /dev/null 2>&1

        # Load data and run server
        bash "$POSTGRES_DIR/scripts/load_synthetic_data.sh"
        bash "$POSTGRES_DIR/run_server.sh" --skip-build

        # Run benchmark
        echo "[INFO] Running synthetic benchmark..."
        python3 "$POSTGRES_DIR/synthetic_benchmark/varying_transaction_length.py" \
            | tee "$RESULT_DIR/postgres/$BUILD_MODE/output.log"

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
echo "[INFO] Generating plot..."
bash "$SCRIPT_DIR/plot_fig4.sh"
