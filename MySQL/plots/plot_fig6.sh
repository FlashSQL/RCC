#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIG_DIR="$BASE_DIR/fig6_varying_client_threads"
RESULTS_DIR="$FIG_DIR/results"

# Parse sysbench logs and generate CSV
generate_csv() {
    local OUTPUT_CSV="$FIG_DIR/varying_conn_pg.csv"

    # Thread counts to check
    THREADS=(1 8 16 32 64 128)

    declare -A vanilla_tps
    declare -A rcc_tps

    # Extract TPS from sysbench logs
    for t in "${THREADS[@]}"; do
        local van_log="$RESULTS_DIR/vanilla/s1_t${t}/sysbench.log"
        local rcc_log="$RESULTS_DIR/rcc/s1_t${t}/sysbench.log"

        if [[ -f "$van_log" ]]; then
            # Parse: transactions:                        58303  (1164.30 per sec.)
            tps=$(grep "transactions:" "$van_log" | grep -oP '\(\K[0-9.]+(?= per sec)')
            vanilla_tps[$t]="${tps:-0}"
        else
            vanilla_tps[$t]="0"
        fi

        if [[ -f "$rcc_log" ]]; then
            tps=$(grep "transactions:" "$rcc_log" | grep -oP '\(\K[0-9.]+(?= per sec)')
            rcc_tps[$t]="${tps:-0}"
        else
            rcc_tps[$t]="0"
        fi
    done

    # Generate CSV
    echo "#threads,van,rcc" > "$OUTPUT_CSV"
    for t in "${THREADS[@]}"; do
        echo "$t,${vanilla_tps[$t]},${rcc_tps[$t]}" >> "$OUTPUT_CSV"
    done

    echo "[INFO] Generated: $OUTPUT_CSV"
    cat "$OUTPUT_CSV"
}

# Run gnuplot
run_gnuplot() {
    cd "$FIG_DIR"
    gnuplot "$SCRIPT_DIR/templates/varying_conn_pg.gnu"
    echo "[INFO] Generated: $FIG_DIR/varying_clients_threads_pg.pdf"
}

echo "=== Plotting Fig6: Varying Client Threads ==="
generate_csv
run_gnuplot
echo "Done!"
