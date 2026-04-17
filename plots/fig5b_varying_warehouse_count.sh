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

RESULT_DIR="$SCRIPT_DIR/fig7_varying_warehouse_count/results"
mkdir -p "$rcc_data_dir"
mkdir -p "$rcc_log_dir"
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
    print_header "MySQL fig5b experiments"

    mkdir -p "$RESULT_DIR/mysql"
    bash "$MYSQL_DIR/scripts/run_tpcc_warehouses.sh" --results-dir "$RESULT_DIR/mysql"
fi

# ==========================================
# POSTGRESQL EXPERIMENTS
# ==========================================
if [[ "$RUN_POSTGRES" == true ]]; then
    print_header "PostgreSQL fig5b experiments"

    mkdir -p "$RESULT_DIR/postgres"
    bash "$POSTGRES_DIR/scripts/run_tpcc_warehouses.sh" --results-dir "$RESULT_DIR/postgres"
fi

echo ""
echo "Results saved to: $RESULT_DIR"
[[ "$RUN_MYSQL" == true ]] && echo "  - $RESULT_DIR/mysql/"
[[ "$RUN_POSTGRES" == true ]] && echo "  - $RESULT_DIR/postgres/"

echo ""
echo "[INFO] Generating plot..."
bash "$SCRIPT_DIR/plot_fig7.sh"
