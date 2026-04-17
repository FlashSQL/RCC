#!/bin/bash
# Fig8b: YCSB varying client threads (MySQL only, fc10, z0.9, 10M rows)

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

RESULT_DIR="$SCRIPT_DIR/fig8b_ycsb_varying_thread/results"
mkdir -p "$rcc_data_dir"
mkdir -p "$rcc_log_dir"
mkdir -p "$RESULT_DIR"

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

BUILD_MODES=("Vanilla" "RCC" "RCC-W" "RCC-D" "RCC-C")

# ==========================================
# 1. Build all MySQL variants (once)
# ==========================================
for BUILD_MODE in "${BUILD_MODES[@]}"; do
    MYSQLD="$MYSQL_DIR/$BUILD_MODE/mysql-8.4.5-build/bin/mysqld"
    if [[ -x "$MYSQLD" ]]; then
        echo "[INFO] $BUILD_MODE already built, skipping"
        continue
    fi
    print_header "Building MySQL ($BUILD_MODE)"
    BUILD_FLAG=$(echo "$BUILD_MODE" | tr '[:upper:]' '[:lower:]')
    cd "$MYSQL_DIR"
    bash run_server.sh --$BUILD_FLAG
    # Stop server started by run_server.sh; the benchmark manages its own lifecycle.
    bash shutdown.sh --$BUILD_FLAG 2>/dev/null
    sleep 3
done

# ==========================================
# 2. Build YCSB JDBC binding
# ==========================================
print_header "Building YCSB JDBC binding"
bash "$MYSQL_DIR/YCSB_benchmark/build.sh"

# ==========================================
# 3. Load YCSB 10M backup (uses Vanilla to populate)
# ==========================================
print_header "Loading YCSB 10M backup"
bash "$MYSQL_DIR/scripts/load_ycsb_data.sh"

# ==========================================
# 4. Run varying-thread experiment
# ==========================================
print_header "Running YCSB varying-thread experiment"
bash "$MYSQL_DIR/scripts/run_ycsb_threads.sh"

# Copy results from YCSB_benchmark into this fig's results dir
cp -rp "$MYSQL_DIR/YCSB_benchmark/results_varying_thread/"* "$RESULT_DIR/" 2>/dev/null

echo ""
echo "Results saved to: $RESULT_DIR"

# ==========================================
# 5. Plot
# ==========================================
if [[ -f "$SCRIPT_DIR/plot_fig8b.sh" ]]; then
    echo ""
    echo "[INFO] Generating plot..."
    bash "$SCRIPT_DIR/plot_fig8b.sh"
fi
