#!/bin/bash
# Fig8b: YCSB varying-thread plot (MySQL only)
# Reads .txt files in fig8b_ycsb_varying_thread/results/ and emits:
#   - ycsb_varying_thread.csv  (#Threads,van,rcc_d,rcc_w,rcc) in TPS
#   - ycsb_abort_rate.csv       (same columns, abort rate in %)
# Then calls gnuplot to produce the two PDFs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIG_DIR="$SCRIPT_DIR/fig8b_ycsb_varying_thread"
RESULTS_DIR="$FIG_DIR/results"
OUT_DIR="$FIG_DIR"

# ==========================================
# Extractors
# ==========================================
# TPS = [OVERALL] Throughput(ops/sec), rounded
extract_tps() {
    local FILE=$1
    [[ -f "$FILE" ]] || { echo "0"; return; }
    awk -F', ' '
        /^\[OVERALL\], Throughput\(ops\/sec\)/ { printf "%d\n", $3 + 0.5; exit }
    ' "$FILE"
}

# Abort rate (%) = WASTED.Operations / (Throughput * RunTime/1000) * 100
extract_abort_rate() {
    local FILE=$1
    [[ -f "$FILE" ]] || { echo "0.00"; return; }
    awk -F', ' '
        /^\[OVERALL\], RunTime\(ms\)/         { runtime = $3 + 0 }
        /^\[OVERALL\], Throughput\(ops\/sec\)/ { tput = $3 + 0 }
        /^\[WASTED\], Operations/             { wasted = $3 + 0 }
        END {
            total = tput * runtime / 1000.0
            if (total > 0) printf "%.2f\n", (wasted / total) * 100.0
            else print "0.00"
        }
    ' "$FILE"
}

# ==========================================
# CSV generator
# CSV column order: #Threads,van,rcc_d,rcc_w,rcc
# ==========================================
generate_csv() {
    local OUTPUT_CSV="$1"
    local EXTRACTOR="$2"

    local THREADS=(1 8 16 32 64 128)
    local SERVERS=(vanilla rcc_d rcc_w rcc)

    echo "#Threads,van,rcc_d,rcc_w,rcc" > "$OUTPUT_CSV"
    for t in "${THREADS[@]}"; do
        local row="$t"
        for s in "${SERVERS[@]}"; do
            row="${row},$($EXTRACTOR "$RESULTS_DIR/t${t}_${s}.txt")"
        done
        echo "$row" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
    echo ""
}

echo "=== Plotting Fig8b: YCSB Varying Thread ==="

mkdir -p "$OUT_DIR"
generate_csv "$OUT_DIR/ycsb_varying_thread.csv" extract_tps
generate_csv "$OUT_DIR/ycsb_abort_rate.csv" extract_abort_rate

cd "$OUT_DIR"
gnuplot "$SCRIPT_DIR/templates/mysql/ycsb_varying_thread.gnu"
echo "[INFO] Generated: $OUT_DIR/ycsb_varying_thread.pdf"
echo "[INFO] Generated: $OUT_DIR/ycsb_varying_thread_abort.pdf"
echo "Done!"
