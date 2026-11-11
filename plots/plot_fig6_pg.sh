#!/bin/bash
# Fig6 (PostgreSQL): throughput vs. client threads, 1 warehouse
# Modes: vanilla_rc (VAN-RC), vanilla_rr (VAN-RR), rcc_rr (RCC-RR)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIG_DIR="$SCRIPT_DIR/fig6_varying_client_threads"
RESULTS_DIR="$FIG_DIR/results/postgres"
OUT_DIR="$FIG_DIR/postgres"
TEMPLATE="$SCRIPT_DIR/templates/postgres/varying_conn_iso_pg.gnu"

THREADS=(1 8 16 32 64 128 256)
MODES=(vanilla_rc vanilla_rr rcc_rr)

# sysbench: "transactions: N (T per sec.)"
extract_tps_pg() {
    local LOG=$1
    [[ -f "$LOG" ]] || { echo "NaN"; return; }
    local tps
    tps=$(grep "transactions:" "$LOG" | grep -oP '\(\K[0-9.]+(?= per sec)')
    echo "${tps:-NaN}"
}

echo "=== Plotting Fig6 (PostgreSQL): Varying Client Threads ==="
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/varying_conn_pg.csv"

echo "#threads,van_rc,van_rr,rcc_rr" > "$CSV"
for t in "${THREADS[@]}"; do
    row="$t"
    has_data=0
    for m in "${MODES[@]}"; do
        tps=$(extract_tps_pg "$RESULTS_DIR/$m/s1_t${t}/sysbench.log")
        row="$row,$tps"
        [[ "$tps" != "NaN" ]] && has_data=1
    done
    if (( has_data )); then
        echo "$row" >> "$CSV"
    fi
done

echo "[INFO] Generated: $CSV"
cat "$CSV"

cd "$OUT_DIR" || exit 1
gnuplot "$TEMPLATE"
gnuplot -e "TERM_SET=1; set terminal pngcairo size 1200,800 font 'Helvetica,20'; set output 'varying_clients_threads_pg.png'" "$TEMPLATE"

echo "[INFO] Generated: $OUT_DIR/varying_clients_threads_pg.pdf"
echo "[INFO] Generated: $OUT_DIR/varying_clients_threads_pg.png"
echo "Done!"
