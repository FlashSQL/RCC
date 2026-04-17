#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MYSQL_DIR="$REPO_ROOT/MySQL"

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

RESULT_DIR="$SCRIPT_DIR/fig6_rcc_variants/results"
MY_CNF="$MYSQL_DIR/my.cnf"
mkdir -p "$rcc_data_dir"
mkdir -p "$rcc_log_dir"
mkdir -p "$RESULT_DIR"

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

set_isolation() {
    local LEVEL=$1
    print_header "Setting isolation level to $LEVEL"
    sed -i -E "s|^transaction_isolation=.*$|transaction_isolation=\"$LEVEL\"|" "$MY_CNF"
    grep "^transaction_isolation" "$MY_CNF"
}

# ==========================================
# REPEATABLE READ
# ==========================================
set_isolation "REPEATABLE-READ"
print_header "MySQL RCC-variants @ REPEATABLE-READ"
mkdir -p "$RESULT_DIR/rr"
bash "$MYSQL_DIR/scripts/run_tpcc_threads.sh" --results-dir "$RESULT_DIR/rr"

# ==========================================
# SERIALIZABLE
# ==========================================
set_isolation "SERIALIZABLE"
print_header "MySQL RCC-variants @ SERIALIZABLE"
mkdir -p "$RESULT_DIR/sr"
bash "$MYSQL_DIR/scripts/run_tpcc_threads.sh" --results-dir "$RESULT_DIR/sr"

# Restore default isolation level
set_isolation "REPEATABLE-READ"

echo ""
echo "Results saved to: $RESULT_DIR"
echo "  - $RESULT_DIR/rr/"
echo "  - $RESULT_DIR/sr/"

echo ""
echo "[INFO] Generating plot..."
bash "$SCRIPT_DIR/plot_fig6_rcc_variants.sh"
