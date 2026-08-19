#!/bin/bash
# YCSB varying-thread experiment (1~128 threads, fc10, z0.9, 10M rows).
# Requires each variant built via MySQL/run_server.sh and the 10M backup
# produced by load_ycsb_data.sh.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/ycsb_config.sh"

# SERVERS=("vanilla" "rcc" "rcc_w")
SERVERS=("rcc_w")
THREAD_LIST=(64 128)
WL="workloads/varying_thread/workload_base"

RESULT_DIR="$YCSB_DIR/results_varying_thread"
mkdir -p "$RESULT_DIR"

# Ctrl-C leaves mysqld, the YCSB client and the watchdog subshell behind
# otherwise, and the next run then trips over the still-running mysqld.
cleanup() {
  trap - INT TERM
  echo "" >&2
  echo "[INT] interrupted — killing YCSB client, mysqld and watchdog" >&2
  pkill -9 -f 'site\.ycsb\.Client' 2>/dev/null
  pkill -9 -x mysqld 2>/dev/null
  kill 0 2>/dev/null   # the whole process group: watchdog subshell included
  exit 130
}
trap cleanup INT TERM

if [[ ! -d "$BACKUP_BASE/10M" ]]; then
  echo "[ERROR] Backup not found at $BACKUP_BASE/10M" >&2
  echo "  Run: bash $SCRIPT_DIR/load_ycsb_data.sh" >&2
  exit 1
fi

for SERVER in "${SERVERS[@]}"; do
  for THREADS in "${THREAD_LIST[@]}"; do
    if ! run_experiment "t${THREADS}" "$SERVER" "$THREADS" "$WL" "$RESULT_DIR" "$BACKUP_BASE/10M"; then
      echo "!!! [ABORT] $SERVER t$THREADS failed (server crash?) — stopping all experiments" >&2
      (pkill -9 -f 'site\.ycsb\.Client' 2>/dev/null || true)
      (pkill -9 -x mysqld 2>/dev/null || true)
      exit 1
    fi
    tps_abort "t${THREADS}" "$SERVER" "$THREADS" "$WL" "$RESULT_DIR" "$BACKUP_BASE/10M"
  done
done

(pkill -9 -x mysqld 2>/dev/null || true)
echo "=== Varying Thread Done ==="
