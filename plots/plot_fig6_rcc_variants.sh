#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIG_DIR="$SCRIPT_DIR/fig6_rcc_variants"
RESULTS_DIR="$FIG_DIR/results"
OUT_DIR="$FIG_DIR"

# MySQL (tpcc-mysql): average of (trx + sum of 4 pipe counters) per line
extract_tps_mysql() {
    local LOG=$1
    [[ -f "$LOG" ]] || { echo "0"; return; }
    grep 'trx:' "$LOG" | awk '
    {
      match($0, /trx: *([0-9]+)/, m1)
      total = m1[1] + 0
      remain = $0
      for (i = 1; i <= 4; i++) {
        if (match(remain, /([0-9]+)\|[0-9]+\.[0-9]+/, m2)) {
          total += m2[1] + 0
          remain = substr(remain, RSTART + RLENGTH)
        }
      }
      sum += total
      count++
    }
    END {
      if (count > 0) printf "%.2f\n", sum / count
      else print "0"
    }'
}

# CSV column order: threads, van, ww, rcc, col, det
# i.e. threads, Vanilla, RCC-W, RCC, RCC-C, RCC-D
generate_csv() {
    local ISO=$1            # rr or sr
    local OUTPUT_CSV="$OUT_DIR/varying_conn_${ISO}.csv"

    local THREADS=(1 8 16 32 64 128)
    local VARIANTS=("Vanilla" "RCC-W" "RCC" "RCC-C" "RCC-D")

    declare -A tps

    for v in "${VARIANTS[@]}"; do
        for t in "${THREADS[@]}"; do
            tps["${v}_${t}"]=$(extract_tps_mysql "$RESULTS_DIR/$ISO/$v/s1_t${t}/sysbench.log")
        done
    done

    echo "#threads,van,ww,rcc,col,det" > "$OUTPUT_CSV"
    for t in "${THREADS[@]}"; do
        echo "$t,${tps[Vanilla_$t]},${tps[RCC-W_$t]},${tps[RCC_$t]},${tps[RCC-C_$t]},${tps[RCC-D_$t]}" \
            >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
}

echo "=== Plotting Fig6: RCC Variants (RR + SR) ==="

mkdir -p "$OUT_DIR"
generate_csv "rr"
generate_csv "sr"

cd "$OUT_DIR"
gnuplot "$SCRIPT_DIR/templates/mysql/rcc_variants.gnu"
echo "[INFO] Generated: $OUT_DIR/rcc_variants_rr.pdf"
echo "[INFO] Generated: $OUT_DIR/rcc_variants_sr.pdf"
echo "Done!"
