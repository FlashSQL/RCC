#!/bin/bash
# Load YCSB 10M backup (fc=10, fl=100, ~1KB rows) for varying_thread experiment.
# Requires Vanilla MySQL built via: cd MySQL && bash run_server.sh --vanilla

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/ycsb_config.sh"

if [[ ! -x "$VANILLA_MYSQLD" ]]; then
  echo "[ERROR] Vanilla mysqld not found at $VANILLA_MYSQLD" >&2
  echo "  Build it first: cd $BASE_DIR && bash run_server.sh --vanilla" >&2
  exit 1
fi

load_backup() {
  local NAME="$1" FC="$2" FL="$3" WL="$4"

  if [ -d "$BACKUP_BASE/$NAME" ]; then
    COUNT=$(ls "$BACKUP_BASE/$NAME/mysql_data/ycsb/" 2>/dev/null | wc -l)
    if [ "$COUNT" -gt 0 ]; then
      echo "SKIP $NAME (already exists)"
      return
    fi
  fi

  echo "=== Loading $NAME (fc=$FC, fl=$FL) ==="

  # Kill existing MySQL and clean dirs
  (pkill -9 -f mysqld 2>/dev/null || true)
  sleep 3
  rm -rf "$DATA_DIR"/* "$LOG_DIR"/*

  # Initialize fresh MySQL
  "$VANILLA_MYSQLD" --defaults-file="$MY_CNF" --initialize-insecure --disable-log-bin --innodb-redo-log-capacity=2G 2>&1 | tail -3
  "$VANILLA_MYSQLD" --defaults-file="$MY_CNF" --disable-log-bin --innodb-redo-log-capacity=2G &
  sleep 8

  # Set root password (YCSB workload expects password 'test')
  "$VANILLA_MYSQL" -uroot -S /tmp/mysql.sock -N -B -e "ALTER USER 'root'@'localhost' IDENTIFIED BY 'test';" 2>/dev/null

  # Create database and table
  mysql_cmd "CREATE DATABASE IF NOT EXISTS ycsb"
  COLS=""
  for i in $(seq 0 $((FC-1))); do
    COLS="$COLS, field${i} TEXT"
  done
  mysql_cmd "CREATE TABLE ycsb.usertable (YCSB_KEY VARCHAR(100) PRIMARY KEY $COLS)"

  # Load data
  "$YCSB_DIR/bin/ycsb" load jdbc -P "$YCSB_DIR/$WL" -cp "$MYSQL_CP" -threads 16 -p maxexecutiontime=0 2>&1 | tail -3

  COUNT=$(mysql_cmd "SELECT COUNT(*) FROM ycsb.usertable")
  echo "  Loaded: $COUNT records"

  # Graceful shutdown
  mysql_cmd "SHUTDOWN"
  sleep 5
  while pgrep -f mysqld > /dev/null 2>&1; do sleep 1; done
  sleep 2

  # Save backup
  mkdir -p "$BACKUP_BASE/$NAME/mysql_data" "$BACKUP_BASE/$NAME/mysql_log"
  cp -rp "$DATA_DIR"/* "$BACKUP_BASE/$NAME/mysql_data/"
  cp -rp "$LOG_DIR"/* "$BACKUP_BASE/$NAME/mysql_log/"
  rm -f "$BACKUP_BASE/$NAME/mysql_log/mysql_error.log"
  echo "  Done: $NAME"
  echo ""
}

echo "=========================================="
echo "  Loading 10M backup"
echo "=========================================="
echo ""

load_backup "10M" 10 100 "workloads/varying_thread/workload_base"
echo ""

(pkill -9 -f mysqld 2>/dev/null || true)

echo "=========================================="
echo "  Backup ready!"
echo "=========================================="
echo ""
echo "Backups:"
ls -d "$BACKUP_BASE"/10M* 2>/dev/null | while read d; do
  SIZE=$(du -sh "$d/mysql_data/ycsb/usertable.ibd" 2>/dev/null | awk '{print $1}')
  echo "  $(basename $d): $SIZE"
done
