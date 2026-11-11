#!/bin/bash
# Fig. 7(b): DBx1000 TPC-C throughput vs. client threads
# Usage: bash plots/plot_fig7b.sh [results dir (default: DBx1000/results/Fig7b)]
# Writes tps_128.csv and fig7b_tpcc.pdf into the results directory.

PLOTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${1:-$PLOTS_DIR/../DBx1000/results/Fig7b}"
[[ -f $DIR/summary.txt ]] || { echo "[ERROR] No results: $DIR (run the experiment first)"; exit 1; }

# summary.txt: name threads rep throughput abort_rate
# One row per thread count: the mean throughput over the repetitions of each run (NaN if missing)
{
    echo "thr,van,bb,rcc,silo"
    awk -v names="WOUND_WAIT BAMBOO RCC SILO" '
        NR > 1 { sum[$1, $2] += $4; cnt[$1, $2]++; thr[$2] = 1 }
        END {
            n = split(names, name, " ")
            for (t in thr) {
                row = t
                for (k = 1; k <= n; k++) row = row "," (cnt[name[k], t] ? sprintf("%.0f", sum[name[k], t] / cnt[name[k], t]) : "NaN")
                print row
            }
        }' "$DIR/summary.txt" | sort -t, -k1,1n
} > "$DIR/tps_128.csv"

cd "$DIR" && gnuplot "$PLOTS_DIR/templates/fig7b_tpcc.gnu" && echo "[INFO] $DIR/fig7b_tpcc.pdf"
