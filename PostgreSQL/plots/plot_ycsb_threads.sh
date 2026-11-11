#!/bin/bash
# PostgreSQL YCSB throughput vs. client threads
# Usage: bash plots/plot_ycsb_threads.sh [results dir (default: PostgreSQL/results/ycsb_threads)]
# Writes tps_128.csv and ycsb_threads.pdf into the results directory.

PLOTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${1:-$PLOTS_DIR/../results/ycsb_threads}"
[[ -d $DIR ]] || { echo "[ERROR] No results: $DIR (run the experiment first)"; exit 1; }

# One row per thread count: the throughput (TPS) in <config>/t<threads>/sysbench.log (NaN if missing)
{
    echo "#thr,van_rc,van_rr,rcc_rr"
    for t in $(ls -d "$DIR"/*/t* 2>/dev/null | sed "s|.*/t||" | grep -E '^[0-9]+$' | sort -nu); do
        printf '%s' "$t"
        for c in vanilla_rc vanilla_rr rcc_rr; do
            tps=$(grep -m1 'transactions:' "$DIR/$c/t$t/sysbench.log" 2>/dev/null | sed -E 's/.*\(([0-9.]+) per sec.*/\1/')
            printf ',%s' "${tps:-NaN}"
        done
        echo
    done
} > "$DIR/tps_128.csv"

cd "$DIR" && gnuplot "$PLOTS_DIR/ycsb_threads.gnu" && echo "[INFO] $DIR/ycsb_threads.pdf"
