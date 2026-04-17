#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIG_DIR="$SCRIPT_DIR/fig4_varying_transaction_length"
RESULTS_DIR="$FIG_DIR/results"

# ==========================================
# Shared CSV generator (ratio = RCC TPS / Vanilla TPS)
# ==========================================
# $1: vanilla log, $2: rcc log, $3: output csv path
generate_ratio_csv() {
    local VANILLA_LOG="$1"
    local RCC_LOG="$2"
    local OUTPUT_CSV="$3"

    if [[ ! -f "$VANILLA_LOG" ]] || [[ ! -f "$RCC_LOG" ]]; then
        echo "[WARN] Skipping $OUTPUT_CSV: result files not found"
        echo "  Expected: $VANILLA_LOG"
        echo "  Expected: $RCC_LOG"
        return 1
    fi

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

    echo "#threads,64,128,256" > "$OUTPUT_CSV"
    for threads in 1 8 16 32 64 128; do
        ratio_64=$(echo "scale=1; ${rcc_tps[${threads}_64]:-0} / ${vanilla_tps[${threads}_64]:-1}" | bc 2>/dev/null || echo "0")
        ratio_128=$(echo "scale=1; ${rcc_tps[${threads}_128]:-0} / ${vanilla_tps[${threads}_128]:-1}" | bc 2>/dev/null || echo "0")
        ratio_256=$(echo "scale=1; ${rcc_tps[${threads}_256]:-0} / ${vanilla_tps[${threads}_256]:-1}" | bc 2>/dev/null || echo "0")
        echo "$threads,$ratio_64,$ratio_128,$ratio_256" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
    return 0
}

echo "=== Plotting Fig4: Varying Transaction Length ==="

# ==========================================
# MySQL plot
# ==========================================
MYSQL_OUT_DIR="$FIG_DIR/mysql"
mkdir -p "$MYSQL_OUT_DIR"
if generate_ratio_csv \
        "$RESULTS_DIR/mysql/Vanilla/output.log" \
        "$RESULTS_DIR/mysql/RCC/output.log" \
        "$MYSQL_OUT_DIR/ratio_by_threads_my.csv"; then
    cd "$MYSQL_OUT_DIR"
    gnuplot "$SCRIPT_DIR/templates/mysql/varying_tx_length.gnu"
    echo "[INFO] Generated: $MYSQL_OUT_DIR/single_hot_mini_my.pdf"
fi

# ==========================================
# PostgreSQL plot
# ==========================================
PG_OUT_DIR="$FIG_DIR/postgres"
mkdir -p "$PG_OUT_DIR"
if generate_ratio_csv \
        "$RESULTS_DIR/postgres/vanilla/output.log" \
        "$RESULTS_DIR/postgres/rcc/output.log" \
        "$PG_OUT_DIR/ratio_by_threads_pg.csv"; then
    cd "$PG_OUT_DIR"
    gnuplot "$SCRIPT_DIR/templates/postgres/varying_tx_length.gnu"
    echo "[INFO] Generated: $PG_OUT_DIR/single_hot_mini_pg.pdf"
fi

echo "Done!"
