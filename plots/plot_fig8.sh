#!/bin/bash
# Fig. 8: DBx1000 RCC ablation, (a) throughput and (b) runtime breakdown
# Usage: bash plots/plot_fig8.sh [results dir (default: DBx1000/results/Fig8)]
# Writes tps_128.csv, time_split.dat, fig8a_tps.pdf and fig8b_time.pdf into the results directory.

PLOTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${1:-$PLOTS_DIR/../DBx1000/results/Fig8}"
[[ -f $DIR/summary.txt ]] || { echo "[ERROR] No results: $DIR (run the experiment first)"; exit 1; }
RUNS="BAMBOO RCC_LUI RCC_LUI_ODH RCC"

# (a) summary.txt: name threads rep throughput abort_rate
# One row per thread count: the mean throughput over the repetitions of each run, in thousands of
# transactions per second as the template expects (NaN if missing)
{
    echo "thr,BB,RCC(LUI),RCC(LUI+ODH),RCC"
    awk -v names="$RUNS" '
        NR > 1 { sum[$1, $2] += $4; cnt[$1, $2]++; thr[$2] = 1 }
        END {
            n = split(names, name, " ")
            for (t in thr) {
                row = t
                for (k = 1; k <= n; k++) row = row "," (cnt[name[k], t] ? sprintf("%.4f", sum[name[k], t] / cnt[name[k], t] / 1000) : "NaN")
                print row
            }
        }' "$DIR/summary.txt" | sort -t, -k1,1n
} > "$DIR/tps_128.csv"

# (b) Amortized runtime per committed transaction (ms), mean over the repetitions, split into
# Execution, Lock Wait, Commit Wait, Prevention and Wasted. It is computed from the [summary] line of
# <run>_<threads>_r<rep>/run.out; the prevention share needs BB_PF_PW, which the Fig. 8 configs turn on.
{
    printf '# threads'
    for n in $RUNS; do printf ' %s_exec %s_lock %s_commit %s_prevention %s_wasted' $n $n $n $n $n; done
    echo
    for t in $(awk 'NR > 1 { print $2 }' "$DIR/summary.txt" | sort -nu); do
        printf '%s' "$t"
        for n in $RUNS; do
            for f in "$DIR/${n}_${t}_r"*/run.out; do grep '^\[summary\]' "$f" 2>/dev/null | tail -1; done | awk '
                {
                    sub(/^\[summary\] */, ""); m = split($0, kv, ", "); delete r
                    for (i = 1; i <= m; i++) { split(kv[i], p, "="); gsub(/ /, "", p[1]); r[p[1]] = p[2] + 0 }
                    if (r["txn_cnt"] == 0) next
                    s = 1e6 / r["txn_cnt"]          # seconds summed over the workers -> us per committed transaction
                    lock = (r["time_wait"] + r["time_get_latch"] + r["time_get_cs"] + r["time_retire_latch"] + r["time_retire_cs"]) * s
                    cw = r["time_commit"] * s; w = r["time_abort"] * s
                    e = r["run_time"] * s - lock - cw - w; prev = 0
                    if (r["bb_pw_cnt"] > 0) {       # move the prevention part out of the lock and commit wait
                        lp = r["time_pw_lock"] > 0 ? r["time_pwl_prev"] / r["time_pw_lock"] : 0
                        cp = r["time_pw_cw"] > 0 ? r["time_pwc_prev"] / r["time_pw_cw"] : 0
                        prev = lock * lp + cw * cp; lock *= 1 - lp; cw *= 1 - cp
                    }
                    E += e; L += lock; C += cw; P += prev; W += w; k++
                }
                END {
                    if (k) printf "\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f", E / k / 1e3, L / k / 1e3, C / k / 1e3, P / k / 1e3, W / k / 1e3
                    else printf "\tNaN\tNaN\tNaN\tNaN\tNaN"
                }'
        done
        echo
    done
} > "$DIR/time_split.dat"

cd "$DIR" || exit 1
gnuplot "$PLOTS_DIR/templates/fig8a_tps.gnu" && echo "[INFO] $DIR/fig8a_tps.pdf"
gnuplot "$PLOTS_DIR/templates/fig8b_time.gnu" && echo "[INFO] $DIR/fig8b_time.pdf"
