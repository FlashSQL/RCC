#!/bin/bash

# ==========================================
# USER CONFIGURATION - Set paths below
# ==========================================
DATA_DIR="/mnt/test_data"    # PostgreSQL data directory
LOG_DIR="/mnt/test_log"      # PostgreSQL log directory

# ==========================================
# DO NOT MODIFY BELOW
# ==========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

print_header() {
    echo ""
    echo "############################################"
    echo "# $1"
    echo "############################################"
}

# Validate configuration
if [[ "$DATA_DIR" == "/path/to/data_dir" ]] || [[ "$LOG_DIR" == "/path/to/log_dir" ]]; then
    echo "[ERROR] Please configure DATA_DIR and LOG_DIR at the top of this script"
    exit 1
fi

mkdir -p $DATA_DIR
mkdir -p $LOG_DIR

echo "=========================================="
echo " PostgreSQL RCC Experiments"
echo "=========================================="
echo "Data Dir: $DATA_DIR"
echo "Log Dir:  $LOG_DIR"
echo "Start:    $(date)"
echo ""

# Fig4: Varying Transaction Length (Synthetic)
print_header "Fig4: Varying Transaction Length"
bash $SCRIPT_DIR/fig4_varying_transaction_length.sh --data-dir $DATA_DIR --log-dir $LOG_DIR

# Fig5: Varying Hotspot Position (Synthetic)
print_header "Fig5: Varying Hotspot Position"
bash $SCRIPT_DIR/fig5_varying_hotspot_position.sh --data-dir $DATA_DIR --log-dir $LOG_DIR

# Fig6: Varying Client Threads (TPC-C)
print_header "Fig6: Varying Client Threads (TPC-C)"
bash $SCRIPT_DIR/fig6_varying_client_threads.sh --data-dir $DATA_DIR --log-dir $LOG_DIR

# Fig7: Varying Warehouse Count (TPC-C)
print_header "Fig7: Varying Warehouse Count (TPC-C)"
bash $SCRIPT_DIR/fig7_varying_warehouse_count.sh --data-dir $DATA_DIR --log-dir $LOG_DIR

echo ""
echo "=========================================="
echo " All Experiments Complete"
echo "=========================================="
echo "End: $(date)"
echo ""
echo "Results:"
echo "  - $SCRIPT_DIR/fig4_varying_transaction_length/results/"
echo "  - $SCRIPT_DIR/fig5_varying_hotspot_position/results/"
echo "  - $SCRIPT_DIR/fig6_varying_client_threads/results/"
echo "  - $SCRIPT_DIR/fig7_varying_warehouse_count/results/"
