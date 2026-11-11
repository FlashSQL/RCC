#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIG_DIR="$SCRIPT_DIR/fig7_varying_warehouse_count"
RESULTS_DIR="$FIG_DIR/results"

# ==========================================
# TPS extractors
# ==========================================
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

# PostgreSQL (sysbench): "transactions:   N  (T per sec.)"
extract_tps_pg() {
    local LOG=$1
    [[ -f "$LOG" ]] || { echo "0"; return; }
    local tps
    tps=$(grep "transactions:" "$LOG" | grep -oP '\(\K[0-9.]+(?= per sec)')
    echo "${tps:-0}"
}

# ==========================================
# CSV generator (TPS by warehouse count)
# ==========================================
generate_wh_csv() {
    local BASE="$1"
    local VANILLA_DIR="$2"
    local RCC_DIR="$3"
    local OUTPUT_CSV="$4"
    local EXTRACTOR="$5"

    local WAREHOUSES=(1 2 4 8)
    local THREADS=(16 32 64 128)
    declare -A vanilla_tps
    declare -A rcc_tps

    for i in "${!WAREHOUSES[@]}"; do
        w="${WAREHOUSES[$i]}"
        t="${THREADS[$i]}"
        vanilla_tps[$w]=$($EXTRACTOR "$BASE/$VANILLA_DIR/s${w}_t${t}/sysbench.log")
        rcc_tps[$w]=$($EXTRACTOR "$BASE/$RCC_DIR/s${w}_t${t}/sysbench.log")
    done

    echo "#warehouses,van,rcc" > "$OUTPUT_CSV"
    for w in "${WAREHOUSES[@]}"; do
        echo "$w,${vanilla_tps[$w]},${rcc_tps[$w]}" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
}

echo "=== Plotting Fig7: Varying Warehouse Count ==="

# ==========================================
# MySQL plot
# ==========================================
if [[ -d "$RESULTS_DIR/mysql" ]]; then   # no MySQL results: do not draw an all-zero MySQL figure
    MYSQL_OUT_DIR="$FIG_DIR/mysql"
    mkdir -p "$MYSQL_OUT_DIR"
    generate_wh_csv "$RESULTS_DIR/mysql" "Vanilla" "RCC" \
        "$MYSQL_OUT_DIR/varying_wh_my.csv" extract_tps_mysql
    cd "$MYSQL_OUT_DIR"
    gnuplot "$SCRIPT_DIR/templates/mysql/varying_wh.gnu"
    echo "[INFO] Generated: $MYSQL_OUT_DIR/varying_wh_tpcc_my.pdf"
fi

# ==========================================
# PostgreSQL plot
# ==========================================
PG_OUT_DIR="$FIG_DIR/postgres"
mkdir -p "$PG_OUT_DIR"
# the modes of PostgreSQL/scripts/run_tpcc_warehouses.sh: RCC (RR) against vanilla at the same isolation level
generate_wh_csv "$RESULTS_DIR/postgres" "vanilla_rr" "rcc_rr" \
    "$PG_OUT_DIR/varying_wh_pg.csv" extract_tps_pg
cd "$PG_OUT_DIR"
gnuplot "$SCRIPT_DIR/templates/postgres/varying_wh_pg.gnu"
echo "[INFO] Generated: $PG_OUT_DIR/varying_wh_tpcc_pg.pdf"

echo "Done!"
