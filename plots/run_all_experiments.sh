#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check required variables
if [[ -z "$rcc_data_dir" ]] || [[ -z "$rcc_log_dir" ]]; then
    echo "[ERROR] rcc_data_dir and rcc_log_dir must be set"
    echo "  export rcc_data_dir=/path/to/data"
    echo "  export rcc_log_dir=/path/to/log"
    exit 1
fi

# Get sudo password once and export for all child scripts
if [[ -z "$PASSWD" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
    export PASSWD
fi

EXPERIMENTS=(
    "fig4a_varying_transaction_length.sh"
    "fig4b_varying_hotspot_position.sh"
    "fig5a_varying_client_threads.sh"
    "fig5b_varying_warehouse_count.sh"
    "fig6_rcc_variants.sh"
)

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

TOTAL=0
SUCCESS=0
FAILED=()

for exp in "${EXPERIMENTS[@]}"; do
    TOTAL=$((TOTAL + 1))
    print_header "Running: $exp"

    if bash "$SCRIPT_DIR/$exp"; then
        SUCCESS=$((SUCCESS + 1))
        echo "[OK] $exp"
    else
        FAILED+=("$exp")
        echo "[FAIL] $exp"
    fi
done

print_header "SUMMARY"
echo "Total:   $TOTAL"
echo "Success: $SUCCESS"
echo "Failed:  ${#FAILED[@]}"
if (( ${#FAILED[@]} > 0 )); then
    echo "Failed experiments:"
    for f in "${FAILED[@]}"; do
        echo "  - $f"
    done
    exit 1
fi
