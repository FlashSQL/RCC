#!/bin/bash
# ============================================
# YCSB benchmark — shared configuration
# Sourced by load_ycsb_data.sh and run_ycsb_threads.sh.
# Aligned with MySQL/run_server.sh and MySQL/scripts/*.sh.
# ============================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
YCSB_DIR="$BASE_DIR/YCSB_benchmark"

# MySQL binaries per variant (built via MySQL/run_server.sh --<variant>)
MYSQLD_vanilla="$BASE_DIR/Vanilla/mysql-8.4.5-build/bin/mysqld"
MYSQLD_rcc="$BASE_DIR/RCC/mysql-8.4.5-build/bin/mysqld"
MYSQLD_rcc_w="$BASE_DIR/RCC-W/mysql-8.4.5-build/bin/mysqld"
MYSQLD_rcc_d="$BASE_DIR/RCC-D/mysql-8.4.5-build/bin/mysqld"
MYSQLD_rcc_c="$BASE_DIR/RCC-C/mysql-8.4.5-build/bin/mysqld"

# Vanilla binaries for loading / SHOW STATUS queries
VANILLA_MYSQLD="$MYSQLD_vanilla"
VANILLA_MYSQL="$BASE_DIR/Vanilla/mysql-8.4.5-build/bin/mysql"

# MySQL config
MY_CNF="$BASE_DIR/my.cnf"

# Data / log directories (follow the same env-var convention as run_server.sh)
if [[ -z "${rcc_data_dir:-}" ]] || [[ -z "${rcc_log_dir:-}" ]]; then
  echo "[ERROR] rcc_data_dir and rcc_log_dir must be set" >&2
  echo "  export rcc_data_dir=/path/to/data" >&2
  echo "  export rcc_log_dir=/path/to/log" >&2
  return 1 2>/dev/null || exit 1
fi
DATA_DIR="$rcc_data_dir"
LOG_DIR="$rcc_log_dir"

# YCSB data backups
BACKUP_BASE="${ycsb_backup_dir:-$YCSB_DIR/backups}"
mkdir -p "$BACKUP_BASE"

# YCSB
BINDING=jdbc
MYSQL_CP="$YCSB_DIR/jdbc/target/dependency/mysql-connector-j-8.4.0.jar"

# NUMA (empty = no NUMA)
NUMA=""

# ============================================
# Helper functions
# ============================================

mysql_cmd() {
  "$VANILLA_MYSQL" -uroot -p'test' -S /tmp/mysql.sock -N -B -e "$1" 2>/dev/null
}

run_experiment() {
  local LABEL="$1" SERVER="$2" THREADS="$3" WL="$4" RESULT_DIR="$5" BACKUP_DIR="$6"
  eval MYSQLD=\$MYSQLD_${SERVER}

  START_TIME=$SECONDS
  echo "=== [${LABEL}] $SERVER t$THREADS === $(date '+%H:%M:%S')"

  (pkill -9 -f mysqld 2>/dev/null || true); sleep 5
  rm -rf "$DATA_DIR"/* "$LOG_DIR"/*
  cp -rp "$BACKUP_DIR"/mysql_data/* "$DATA_DIR"/
  cp -rp "$BACKUP_DIR"/mysql_log/* "$LOG_DIR"/

  $NUMA "$MYSQLD" --defaults-file="$MY_CNF" --disable-log-bin --max-prepared-stmt-count=200000 &>/dev/null &
  sleep 10

  # Warmup
  $NUMA "$YCSB_DIR/bin/ycsb" run $BINDING -P "$YCSB_DIR/$WL" -cp "$MYSQL_CP" -threads $THREADS &>/dev/null

  # Pre-measurement counters
  LT_B=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_lock_time';" | awk '{print $2}')
  CT_B=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_commit_time';" | awk '{print $2}')

  # Measurement
  $NUMA "$YCSB_DIR/bin/ycsb" run $BINDING -P "$YCSB_DIR/$WL" -cp "$MYSQL_CP" -threads $THREADS > "$RESULT_DIR/${LABEL}_${SERVER}.txt" 2>&1

  # Post-measurement counters
  LT_A=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_lock_time';" | awk '{print $2}')
  CT_A=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_commit_time';" | awk '{print $2}')

  LW=$((${LT_A:-0} - ${LT_B:-0}))
  CW=$((${CT_A:-0} - ${CT_B:-0}))
  ABORTS=$(grep "^\[READ\].*Return=ERROR" "$RESULT_DIR/${LABEL}_${SERVER}.txt" | awk -F', ' '{print $3}')

  echo "" >> "$RESULT_DIR/${LABEL}_${SERVER}.txt"
  echo "[LOCK_WAIT_TIME], Total(ms), $LW" >> "$RESULT_DIR/${LABEL}_${SERVER}.txt"
  echo "[COMMIT_WAIT_TIME], Total(ms), $CW" >> "$RESULT_DIR/${LABEL}_${SERVER}.txt"
  echo "[ABORTS], Count, ${ABORTS:-0}" >> "$RESULT_DIR/${LABEL}_${SERVER}.txt"

  ELAPSED=$((SECONDS - START_TIME))
  echo "[ELAPSED], Seconds, $ELAPSED" >> "$RESULT_DIR/${LABEL}_${SERVER}.txt"
  echo "    Done in ${ELAPSED}s"
}
