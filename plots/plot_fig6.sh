#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIG_DIR="$SCRIPT_DIR/fig6_varying_client_threads"
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
# CSV generator (TPS by thread count, 1 warehouse)
# ==========================================
# $1: results base dir, $2: vanilla subdir, $3: rcc subdir, $4: output csv, $5: extractor fn
generate_threads_csv() {
    local BASE="$1"
    local VANILLA_DIR="$2"
    local RCC_DIR="$3"
    local OUTPUT_CSV="$4"
    local EXTRACTOR="$5"

    local THREADS=(1 8 16 32 64 128)
    declare -A vanilla_tps
    declare -A rcc_tps

    for t in "${THREADS[@]}"; do
        vanilla_tps[$t]=$($EXTRACTOR "$BASE/$VANILLA_DIR/s1_t${t}/sysbench.log")
        rcc_tps[$t]=$($EXTRACTOR "$BASE/$RCC_DIR/s1_t${t}/sysbench.log")
    done

    echo "#threads,van,rcc" > "$OUTPUT_CSV"
    for t in "${THREADS[@]}"; do
        echo "$t,${vanilla_tps[$t]},${rcc_tps[$t]}" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
}

echo "=== Plotting Fig6: Varying Client Threads ==="

# ==========================================
# MySQL plot
# ==========================================
MYSQL_OUT_DIR="$FIG_DIR/mysql"
mkdir -p "$MYSQL_OUT_DIR"
generate_threads_csv "$RESULTS_DIR/mysql" "Vanilla" "RCC" \
    "$MYSQL_OUT_DIR/varying_conn_my.csv" extract_tps_mysql
cd "$MYSQL_OUT_DIR"
gnuplot "$SCRIPT_DIR/templates/mysql/varying_conn.gnu"
echo "[INFO] Generated: $MYSQL_OUT_DIR/varying_clients_threads_my.pdf"

# ==========================================
# PostgreSQL plot
# ==========================================
PG_OUT_DIR="$FIG_DIR/postgres"
mkdir -p "$PG_OUT_DIR"
generate_threads_csv "$RESULTS_DIR/postgres" "vanilla" "rcc" \
    "$PG_OUT_DIR/varying_conn_pg.csv" extract_tps_pg
cd "$PG_OUT_DIR"
gnuplot "$SCRIPT_DIR/templates/postgres/varying_conn_pg.gnu"
echo "[INFO] Generated: $PG_OUT_DIR/varying_clients_threads_pg.pdf"

echo "Done!"
