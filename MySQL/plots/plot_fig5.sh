#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIG_DIR="$BASE_DIR/fig5_varying_hotspot_position"
RESULTS_DIR="$FIG_DIR/results"

# Parse output.log and generate CSV
generate_csv() {
    local VANILLA_LOG="$RESULTS_DIR/vanilla/output.log"
    local VANILLA_RC_LOG="$RESULTS_DIR/vanilla_rc/output.log"
    local RCC_LOG="$RESULTS_DIR/rcc/output.log"
    local OUTPUT_CSV="$FIG_DIR/tps_distance_pg.csv"

    if [[ ! -f "$VANILLA_LOG" ]] || [[ ! -f "$RCC_LOG" ]]; then
        echo "[ERROR] Result files not found"
        echo "  Expected: $VANILLA_LOG"
        echo "  Expected: $RCC_LOG"
        exit 1
    fi

    # Extract TPS values from [RUN] lines
    # Format: [RUN] th=128 reads=256 dist=0.00 | TPS=2658.0 ...
    declare -A vanilla_tps
    declare -A vanilla_rc_tps
    declare -A rcc_tps

    while read -r line; do
        if [[ "$line" =~ dist=([0-9.]+).*TPS=([0-9.]+) ]]; then
            dist="${BASH_REMATCH[1]}"
            tps="${BASH_REMATCH[2]}"
            vanilla_tps["$dist"]=$tps
        fi
    done < "$VANILLA_LOG"

    if [[ -f "$VANILLA_RC_LOG" ]]; then
        while read -r line; do
            if [[ "$line" =~ dist=([0-9.]+).*TPS=([0-9.]+) ]]; then
                dist="${BASH_REMATCH[1]}"
                tps="${BASH_REMATCH[2]}"
                vanilla_rc_tps["$dist"]=$tps
            fi
        done < "$VANILLA_RC_LOG"
    fi

    while read -r line; do
        if [[ "$line" =~ dist=([0-9.]+).*TPS=([0-9.]+) ]]; then
            dist="${BASH_REMATCH[1]}"
            tps="${BASH_REMATCH[2]}"
            rcc_tps["$dist"]=$tps
        fi
    done < "$RCC_LOG"

    # Generate TPS CSV
    echo "#distance,van,van(RC),rcc" > "$OUTPUT_CSV"

    for distance in 0.00 0.25 0.50 0.75 1.00; do
        van_tps="${vanilla_tps[$distance]:-0}"
        van_rc_tps="${vanilla_rc_tps[$distance]:-0}"
        rcc_tps_val="${rcc_tps[$distance]:-0}"
        echo "$distance,$van_tps,$van_rc_tps,$rcc_tps_val" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
}

# Run gnuplot
run_gnuplot() {
    cd "$FIG_DIR"
    gnuplot "$SCRIPT_DIR/templates/varying_hotspot_pos.gnu"
    echo "[INFO] Generated: $FIG_DIR/single_hot_distance_mini_pg.pdf"
}

echo "=== Plotting Fig5: Varying Hotspot Position ==="
generate_csv
run_gnuplot
echo "Done!"
