#!/bin/bash
set -eo pipefail

# ==========================================
# USER CONFIGURATION - Set paths below
# ==========================================
DATA_DIR="/mnt/test_data"    # PostgreSQL data directory
LOG_DIR="/mnt/test_log"      # PostgreSQL log directory

# ==========================================
# DO NOT MODIFY BELOW
# ==========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLOTS_DIR="$SCRIPT_DIR/../plots"
export rcc_data_dir="${rcc_data_dir:-$DATA_DIR}"
export rcc_log_dir="${rcc_log_dir:-$LOG_DIR}"
ONLY=all
DRY_RUN=false

usage() {
    cat <<'EOF'
Usage: bash run_all_experiments.sh [--only length|hotspot|threads|warehouses] [--dry-run]

Runs the four current PostgreSQL experiments (default: all).
  length      Synthetic: 64/128/256 reads, 1/8/16/32/64/128 clients
  hotspot     Synthetic: five update positions, 128 clients, 256 reads
  threads     TPC-C: one warehouse, 1 through 256 clients
  warehouses  TPC-C: 1/2/4/8 warehouses, 16/32/64/128 clients

Uses each experiment's existing build modes and parameters; does not add SR
or enable rcc_bg_detect. Rebuilds from the local source tree.
Results: ../plots/fig{4,5,6,7}_*/results/postgres/
Automatic plotting is skipped: legacy plotters do not cover the current modes.
Data/log directories are cleared by the experiments. Use dedicated test data.
--dry-run only prints commands; it does not build, stop servers, or delete data.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --only)
            [[ $# -ge 2 ]] || { echo "Missing --only value" >&2; exit 2; }
            ONLY="$2"; shift 2 ;;
        --dry-run) DRY_RUN=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$ONLY" in
    all|length|hotspot|threads|warehouses) ;;
    *) echo "Unknown experiment: $ONLY" >&2; exit 2 ;;
esac

printf 'Data: %s\nLog:  %s\n' "$rcc_data_dir" "$rcc_log_dir"
if [[ "$DRY_RUN" == false ]]; then
    # These experiments erase both directories. Never reuse a MySQL datadir
    # or redo-log directory.
    for dir in "$rcc_data_dir" "$rcc_log_dir"; do
        if [[ -e "$dir/ibdata1" || -d "$dir/mysql" || -d "$dir/#innodb_redo" ]]; then
            echo "ERROR: MySQL data found in $dir; refusing to erase it." >&2
            echo "Set rcc_data_dir and rcc_log_dir to dedicated PostgreSQL experiment directories." >&2
            exit 1
        fi
    done
    mkdir -p "$rcc_data_dir" "$rcc_log_dir"
    if ! sudo -n true 2>/dev/null && [[ -z "${PASSWD:-}" ]]; then
        read -rsp 'Enter sudo password: ' PASSWD
        echo
    fi
    export PASSWD
fi

run_experiment() {
    local name="$1"
    shift
    if [[ "$ONLY" != all && "$ONLY" != "$name" ]]; then
        return
    fi
    printf '\n[%s] ' "$name"
    printf '%q ' "$@"
    printf '\n'
    if [[ "$DRY_RUN" == false ]]; then
        "$@"
    fi
}

run_experiment length bash "$PLOTS_DIR/fig4a_varying_transaction_length.sh" --postgres --skip-plot
run_experiment hotspot bash "$PLOTS_DIR/fig4b_varying_hotspot_position.sh" --postgres --skip-plot
# run_experiment threads bash "$SCRIPT_DIR/scripts/run_tpcc_threads.sh" \
#     --results-dir "$PLOTS_DIR/fig6_varying_client_threads/results/postgres"
run_experiment warehouses bash "$SCRIPT_DIR/scripts/run_tpcc_warehouses.sh" \
    --results-dir "$PLOTS_DIR/fig7_varying_warehouse_count/results/postgres"

if [[ "$DRY_RUN" == false ]]; then
    echo "Experiment scripts finished. Review sysbench.log and consistency.txt for each TPC-C run."
fi
