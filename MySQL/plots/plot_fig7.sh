#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIG_DIR="$BASE_DIR/fig7_varying_warehouse_count"
RESULTS_DIR="$FIG_DIR/results"

# Parse sysbench logs and generate CSV
generate_csv() {
    local OUTPUT_CSV="$FIG_DIR/varying_wh_pg.csv"

    # Warehouse counts and thread counts
    WAREHOUSES=(1 2 4 8)
    THREADS=(16 32 64 128)

    declare -A vanilla_tps
    declare -A rcc_tps

    # Extract TPS from sysbench logs
    for i in "${!WAREHOUSES[@]}"; do
        w="${WAREHOUSES[$i]}"
        t="${THREADS[$i]}"

        local van_log="$RESULTS_DIR/vanilla/s${w}_t${t}/sysbench.log"
        local rcc_log="$RESULTS_DIR/rcc/s${w}_t${t}/sysbench.log"

        if [[ -f "$van_log" ]]; then
            tps=$(grep "transactions:" "$van_log" | grep -oP '\(\K[0-9.]+(?= per sec)')
            vanilla_tps[$w]="${tps:-0}"
        else
            vanilla_tps[$w]="0"
        fi

        if [[ -f "$rcc_log" ]]; then
            tps=$(grep "transactions:" "$rcc_log" | grep -oP '\(\K[0-9.]+(?= per sec)')
            rcc_tps[$w]="${tps:-0}"
        else
            rcc_tps[$w]="0"
        fi
    done

    # Generate CSV
    echo "#warehouses,van,stx" > "$OUTPUT_CSV"
    for w in "${WAREHOUSES[@]}"; do
        echo "$w,${vanilla_tps[$w]},${rcc_tps[$w]}" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
}

# Run gnuplot
run_gnuplot() {
    cd "$FIG_DIR"
    gnuplot "$SCRIPT_DIR/templates/varying_wh_pg.gnu"
    echo "[INFO] Generated: $FIG_DIR/varying_wh_tpcc_pg.pdf"
}

echo "=== Plotting Fig7: Varying Warehouse Count ==="
generate_csv
run_gnuplot
echo "Done!"
