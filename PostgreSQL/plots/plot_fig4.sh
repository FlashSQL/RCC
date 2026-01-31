#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIG_DIR="$BASE_DIR/fig4_varying_transaction_length"
RESULTS_DIR="$FIG_DIR/results"

# Parse output.log and generate CSV
generate_csv() {
    local VANILLA_LOG="$RESULTS_DIR/vanilla/output.log"
    local RCC_LOG="$RESULTS_DIR/rcc/output.log"
    local OUTPUT_CSV="$FIG_DIR/ratio_by_threads_pg.csv"

    if [[ ! -f "$VANILLA_LOG" ]] || [[ ! -f "$RCC_LOG" ]]; then
        echo "[ERROR] Result files not found"
        echo "  Expected: $VANILLA_LOG"
        echo "  Expected: $RCC_LOG"
        exit 1
    fi

    # Extract TPS values from CSV section
    # Format: threads,reads_per_tx,tps,...
    declare -A vanilla_tps
    declare -A rcc_tps

    while IFS=',' read -r threads reads tps rest; do
        [[ "$threads" =~ ^[0-9]+$ ]] || continue
        vanilla_tps["${threads}_${reads}"]=$tps
    done < <(sed -n '/^threads,reads_per_tx/,$ p' "$VANILLA_LOG" | tail -n +2)

    while IFS=',' read -r threads reads tps rest; do
        [[ "$threads" =~ ^[0-9]+$ ]] || continue
        rcc_tps["${threads}_${reads}"]=$tps
    done < <(sed -n '/^threads,reads_per_tx/,$ p' "$RCC_LOG" | tail -n +2)

    # Generate ratio CSV (RCC TPS / Vanilla TPS)
    echo "#threads,64,128,256" > "$OUTPUT_CSV"

    for threads in 1 8 16 32 64 128; do
        ratio_64=$(echo "scale=1; ${rcc_tps[${threads}_64]:-0} / ${vanilla_tps[${threads}_64]:-1}" | bc 2>/dev/null || echo "0")
        ratio_128=$(echo "scale=1; ${rcc_tps[${threads}_128]:-0} / ${vanilla_tps[${threads}_128]:-1}" | bc 2>/dev/null || echo "0")
        ratio_256=$(echo "scale=1; ${rcc_tps[${threads}_256]:-0} / ${vanilla_tps[${threads}_256]:-1}" | bc 2>/dev/null || echo "0")
        echo "$threads,$ratio_64,$ratio_128,$ratio_256" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
}

# Run gnuplot
run_gnuplot() {
    cd "$FIG_DIR"
    gnuplot "$SCRIPT_DIR/templates/varying_tx_length.gnu"
    echo "[INFO] Generated: $FIG_DIR/single_hot_mini_pg.pdf"
}

echo "=== Plotting Fig4: Varying Transaction Length ==="
generate_csv
run_gnuplot
echo "Done!"
