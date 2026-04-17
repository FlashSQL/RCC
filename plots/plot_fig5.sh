#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIG_DIR="$SCRIPT_DIR/fig5_varying_hotspot_position"
RESULTS_DIR="$FIG_DIR/results"

# ==========================================
# Shared CSV generator (TPS by hotspot distance)
# ==========================================
# $1: vanilla log, $2: vanilla_rc log (optional), $3: rcc log, $4: output csv
generate_tps_csv() {
    local VANILLA_LOG="$1"
    local VANILLA_RC_LOG="$2"
    local RCC_LOG="$3"
    local OUTPUT_CSV="$4"

    if [[ ! -f "$VANILLA_LOG" ]] || [[ ! -f "$RCC_LOG" ]]; then
        echo "[WARN] Skipping $OUTPUT_CSV: result files not found"
        echo "  Expected: $VANILLA_LOG"
        echo "  Expected: $RCC_LOG"
        return 1
    fi

    declare -A vanilla_tps
    declare -A vanilla_rc_tps
    declare -A rcc_tps

    while read -r line; do
        if [[ "$line" =~ dist=([0-9.]+).*TPS=([0-9.]+) ]]; then
            vanilla_tps["${BASH_REMATCH[1]}"]="${BASH_REMATCH[2]}"
        fi
    done < "$VANILLA_LOG"

    if [[ -f "$VANILLA_RC_LOG" ]]; then
        while read -r line; do
            if [[ "$line" =~ dist=([0-9.]+).*TPS=([0-9.]+) ]]; then
                vanilla_rc_tps["${BASH_REMATCH[1]}"]="${BASH_REMATCH[2]}"
            fi
        done < "$VANILLA_RC_LOG"
    fi

    while read -r line; do
        if [[ "$line" =~ dist=([0-9.]+).*TPS=([0-9.]+) ]]; then
            rcc_tps["${BASH_REMATCH[1]}"]="${BASH_REMATCH[2]}"
        fi
    done < "$RCC_LOG"

    echo "#distance,van,van(RC),rcc" > "$OUTPUT_CSV"
    for distance in 0.00 0.25 0.50 0.75 1.00; do
        echo "$distance,${vanilla_tps[$distance]:-0},${vanilla_rc_tps[$distance]:-0},${rcc_tps[$distance]:-0}" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
    return 0
}

echo "=== Plotting Fig5: Varying Hotspot Position ==="

# ==========================================
# MySQL plot
# ==========================================
MYSQL_OUT_DIR="$FIG_DIR/mysql"
mkdir -p "$MYSQL_OUT_DIR"
if generate_tps_csv \
        "$RESULTS_DIR/mysql/Vanilla/output.log" \
        "$RESULTS_DIR/mysql/Vanilla_RC/output.log" \
        "$RESULTS_DIR/mysql/RCC/output.log" \
        "$MYSQL_OUT_DIR/tps_distance_my.csv"; then
    cd "$MYSQL_OUT_DIR"
    gnuplot "$SCRIPT_DIR/templates/mysql/varying_hotspot_pos.gnu"
    echo "[INFO] Generated: $MYSQL_OUT_DIR/single_hot_distance_mini_my.pdf"
fi

# ==========================================
# PostgreSQL plot
# ==========================================
PG_OUT_DIR="$FIG_DIR/postgres"
mkdir -p "$PG_OUT_DIR"
if generate_tps_csv \
        "$RESULTS_DIR/postgres/vanilla/output.log" \
        "$RESULTS_DIR/postgres/vanilla_rc/output.log" \
        "$RESULTS_DIR/postgres/rcc/output.log" \
        "$PG_OUT_DIR/tps_distance_pg.csv"; then
    cd "$PG_OUT_DIR"
    gnuplot "$SCRIPT_DIR/templates/postgres/varying_hotspot_pos.gnu"
    echo "[INFO] Generated: $PG_OUT_DIR/single_hot_distance_mini_pg.pdf"
fi

echo "Done!"
