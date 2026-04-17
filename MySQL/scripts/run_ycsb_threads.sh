#!/bin/bash
# YCSB varying-thread experiment (1~128 threads, fc10, z0.9, 10M rows).
# Requires each variant built via MySQL/run_server.sh and the 10M backup
# produced by load_ycsb_data.sh.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/ycsb_config.sh"

SERVERS=("vanilla" "rcc" "rcc_w" "rcc_d" "rcc_c")
THREAD_LIST=(1 8 16 32 64 128)
WL="workloads/varying_thread/workload_base"

RESULT_DIR="$YCSB_DIR/results_varying_thread"
mkdir -p "$RESULT_DIR"

if [[ ! -d "$BACKUP_BASE/10M" ]]; then
  echo "[ERROR] Backup not found at $BACKUP_BASE/10M" >&2
  echo "  Run: bash $SCRIPT_DIR/load_ycsb_data.sh" >&2
  exit 1
fi

for THREADS in "${THREAD_LIST[@]}"; do
  for SERVER in "${SERVERS[@]}"; do
    run_experiment "t${THREADS}" "$SERVER" "$THREADS" "$WL" "$RESULT_DIR" "$BACKUP_BASE/10M"
  done
done

(pkill -9 -f mysqld 2>/dev/null || true)
echo "=== Varying Thread Done ==="
