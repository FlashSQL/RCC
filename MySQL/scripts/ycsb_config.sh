#!/bin/bash
# ============================================
# YCSB benchmark — shared configuration
# Sourced by load_ycsb_data.sh and run_ycsb_threads.sh.
# Aligned with MySQL/run_server.sh and MySQL/scripts/*.sh.
# ============================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
YCSB_DIR="$BASE_DIR/YCSB_benchmark"

# Vanilla build used for loading / SHOW STATUS queries (ShadowTX8's build tree)
VANILLA_BUILD="${vanilla_build_dir:-/home/hjin/mysql-8.4.5-build}"
VANILLA_MYSQLD="$VANILLA_BUILD/bin/mysqld"
VANILLA_MYSQL="$VANILLA_BUILD/bin/mysql"

# MySQL binaries per variant
MYSQLD_vanilla="/home/hjin/mysql-8.4.5-build/bin/mysqld"
MYSQLD_rcc="/home/hjin/ShadowTX8/mysql-8.4.5-build/bin/mysqld"
MYSQLD_rcc_w="/home/hjin/ShadowTX8_BB/mysql-8.4.5-build/bin/mysqld"

# MySQL config (ShadowTX8's my.cnf: datadir=/home/hjin/mysql_data, log=/home/hjin/mysql_log)
MY_CNF="${my_cnf:-/home/hjin/ShadowTX8/my.cnf}"

# Data / log directories — must match datadir / innodb_log_group_home_dir in $MY_CNF
DATA_DIR="${rcc_data_dir:-/home/hjin/mysql_data}"
LOG_DIR="${rcc_log_dir:-/home/hjin/mysql_log}"
if [[ -z "$DATA_DIR" ]] || [[ -z "$LOG_DIR" ]]; then
  echo "[ERROR] rcc_data_dir / rcc_log_dir must not be empty" >&2
  exit 1
fi
mkdir -p "$DATA_DIR" "$LOG_DIR"

# YCSB data backups
BACKUP_BASE="${ycsb_backup_dir:-$YCSB_DIR/backups}"
mkdir -p "$BACKUP_BASE"

# YCSB
BINDING=jdbc
MYSQL_CP="$YCSB_DIR/jdbc/target/dependency/mysql-connector-j-8.4.0.jar"

# NUMA (empty = no NUMA)
NUMA=""

# Warmup / measurement duration in seconds. Overrides maxexecutiontime in the
# workload file (command-line -p wins), so the file itself stays untouched.
# WARMUP_TIME="${warmup_time:-5}"
# RUN_TIME="${run_time:-5}"
WARMUP_TIME="5"
RUN_TIME="5"

# Live progress: YCSB -s prints "N sec: M operations; X current ops/sec" to
# stderr every status_interval seconds. show_status=0 turns it off.
SHOW_STATUS="${show_status:-1}"
STATUS_INTERVAL="${status_interval:-1}"
YCSB_STATUS_OPTS=""
[[ "$SHOW_STATUS" == "1" ]] && YCSB_STATUS_OPTS="-s -p status.interval=$STATUS_INTERVAL"

# ============================================
# Helper functions
# ============================================

mysql_cmd() {
  "$VANILLA_MYSQL" -uroot -p'test' -S /tmp/mysql.sock -N -B -e "$1" 2>/dev/null
}

# Hard deadline for one YCSB run. YCSB's own maxexecutiontime is cooperative:
# TerminatorThread sets a stop flag and then join()s the workers forever, and a
# worker only checks that flag between operations. So a worker blocked inside a
# JDBC call (stalled server, row-lock wait) hangs the run indefinitely — hence
# the endless "0 current ops/sec" after "Now Joining!".
#
# --foreground is required: without it timeout puts the command in its own
# process group, which takes it out of the terminal's foreground group, and
# Ctrl-C (SIGINT goes only to that group) no longer reaches the YCSB client.
# The tradeoff is that timeout then only kills its direct child — bin/ycsb runs
# java via subprocess, so pkill below finishes off the actual client.
# A killed run produces no [OVERALL] stats, so its result file is empty on
# purpose — that run is invalid, not slow.
# $1 = maxexecutiontime, rest = extra ycsb args. Caller redirects stdout.
YCSB_GRACE="${ycsb_grace:-60}"

run_ycsb() {
  local secs="$1" rc; shift
  timeout --foreground -k 10 "$((secs + YCSB_GRACE))" \
    $NUMA "$YCSB_DIR/bin/ycsb" run $BINDING -P "$YCSB_DIR/$WL" -cp "$MYSQL_CP" \
    -threads "$THREADS" $YCSB_STATUS_OPTS -p maxexecutiontime="$secs" "$@"
  rc=$?
  if (( rc == 124 || rc == 137 )); then
    echo "[WARN] YCSB did not exit within ${secs}s + ${YCSB_GRACE}s grace — killing client (workers stuck in the DB)" >&2
    pkill -9 -f 'site\.ycsb\.Client' 2>/dev/null
  fi
  return $rc
}

# Wait until the server actually accepts connections. A cold start with a large
# buffer pool takes well over 10s, so a fixed sleep silently races whatever runs
# next (the load's ALTER USER, or here the warmup + SHOW STATUS counters).
# $1 = root password ("" for a freshly initialized, password-less server)
wait_for_mysqld() {
  local pw="$1" i
  for ((i = 0; i < 180; i++)); do
    if [[ -z "$pw" ]]; then
      "$VANILLA_MYSQL" -uroot -S /tmp/mysql.sock -N -B -e "SELECT 1" &>/dev/null && return 0
    else
      "$VANILLA_MYSQL" -uroot -p"$pw" -S /tmp/mysql.sock -N -B -e "SELECT 1" &>/dev/null && return 0
    fi
    pgrep -x mysqld >/dev/null || { echo "[ERROR] mysqld died during startup" >&2; tail -20 "$LOG_DIR/mysql_error.log" >&2; return 1; }
    sleep 1
  done
  echo "[ERROR] mysqld did not accept connections within 180s" >&2
  return 1
}

run_experiment() {
  local LABEL="$1" SERVER="$2" THREADS="$3" WL="$4" RESULT_DIR="$5" BACKUP_DIR="$6"
  eval MYSQLD=\$MYSQLD_${SERVER}
  if [[ ! -x "$MYSQLD" ]]; then
    echo "[ERROR] mysqld for '$SERVER' not found at ${MYSQLD:-<unset>}" >&2
    return 1
  fi

  START_TIME=$SECONDS
  echo "=== [${LABEL}] $SERVER t$THREADS === $(date '+%H:%M:%S')"

  (pkill -9 -x mysqld 2>/dev/null || true)
  while pgrep -x mysqld >/dev/null; do sleep 1; done
  rm -rf "${DATA_DIR:?DATA_DIR is not set}"/* "${LOG_DIR:?LOG_DIR is not set}"/*
  cp -rp "$BACKUP_DIR"/mysql_data/* "$DATA_DIR"/
  cp -rp "$BACKUP_DIR"/mysql_log/* "$LOG_DIR"/

  $NUMA "$MYSQLD" --defaults-file="$MY_CNF" --disable-log-bin --max-prepared-stmt-count=200000 &>/dev/null &
  wait_for_mysqld test || return 1

  # Watchdog: the moment mysqld disappears (SIGSEGV etc.), print the crash
  # signature and kill the YCSB clients so the run aborts immediately instead
  # of hammering a dead server until maxexecutiontime. Killed at experiment end.
  (
    while sleep 1; do
      if ! pgrep -x mysqld >/dev/null; then
        echo "!!! [WATCHDOG $(date '+%H:%M:%S')] mysqld DIED during [${LABEL}] $SERVER t$THREADS — aborting" >&2
        grep -A3 "got signal" "$LOG_DIR/mysql_error.log" 2>/dev/null | head -8 >&2
        pkill -9 -f 'site\.ycsb\.Client' 2>/dev/null
        break
      fi
    done
  ) &
  local WATCHDOG_PID=$!

  # Warmup (with -s, per-second progress goes to the terminal via stderr).
  # warmup_time=0 skips it entirely: passing maxexecutiontime=0 to YCSB means
  # "no time limit" (the terminator thread only starts when the value is > 0),
  # so it would run the full operationcount instead of stopping immediately.
  if [[ "$WARMUP_TIME" -gt 0 ]]; then
    echo "--- warmup ${WARMUP_TIME}s ---"
    run_ycsb "$WARMUP_TIME" >/dev/null

    if ! pgrep -x mysqld >/dev/null; then
      kill "$WATCHDOG_PID" 2>/dev/null
      echo "[ERROR] mysqld died during warmup of [${LABEL}] $SERVER t$THREADS — skipping measurement" >&2
      return 1
    fi
  else
    echo "--- warmup skipped ---"
  fi

  # # Pre-measurement counters
  # LT_B=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_lock_time';" | awk '{print $2}')
  # CT_B=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_commit_time';" | awk '{print $2}')

  # Measurement (stdout -> result file; -s progress stays on the terminal)
  echo "--- measure ${RUN_TIME}s ---"
  run_ycsb "$RUN_TIME" > "$RESULT_DIR/${LABEL}_${SERVER}.txt"

  kill "$WATCHDOG_PID" 2>/dev/null
  if ! pgrep -x mysqld >/dev/null; then
    echo "[ERROR] mysqld is dead after [${LABEL}] $SERVER t$THREADS — result is invalid" >&2
    return 1
  fi

  # # Post-measurement counters
  # LT_A=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_lock_time';" | awk '{print $2}')
  # CT_A=$(mysql_cmd "SHOW GLOBAL STATUS LIKE 'Innodb_row_commit_time';" | awk '{print $2}')

  # LW=$((${LT_A:-0} - ${LT_B:-0}))
  # CW=$((${CT_A:-0} - ${CT_B:-0}))

  # Transaction-level results. With batchtxproportion=1.0 each YCSB "operation"
  # is one opspertransaction-sized transaction: CoreWorkload records a committed
  # one as [BATCHTX] and an aborted one as [WASTED]. So:
  #   commits = [BATCHTX] Operations, aborts = [WASTED] Operations
  #   abort rate = aborts / (commits + aborts)
  #   TPS        = commits / RunTime          <- committed only, unlike
  #                                              [OVERALL] Throughput, which
  #                                              counts aborted txns too.
  # local RES="$RESULT_DIR/${LABEL}_${SERVER}.txt"
  # read -r COMMITS ABORTS ABORT_RATE TPS < <(awk -F', *' '
  #   $1 == "[BATCHTX]" && $2 == "Operations" { ok   = $3 }
  #   $1 == "[WASTED]"  && $2 == "Operations" { fail = $3 }
  #   $1 == "[OVERALL]" && $2 == "RunTime(ms)" { ms  = $3 }
  #   END {
  #     total = ok + fail
  #     printf "%d %d %.4f %.2f\n", ok, fail,
  #            (total > 0 ? fail / total : 0),
  #            (ms    > 0 ? ok * 1000.0 / ms : 0)
  #   }' "$RES")

  # {
  #   echo ""
  #   echo "[LOCK_WAIT_TIME], Total(ms), $LW"
  #   echo "[COMMIT_WAIT_TIME], Total(ms), $CW"
  #   echo "[COMMITS], Count, ${COMMITS:-0}"
  #   echo "[ABORTS], Count, ${ABORTS:-0}"
  #   echo "[ABORT_RATE], Ratio, ${ABORT_RATE:-0}"
  #   echo "[TPS], Txn/sec, ${TPS:-0}"
  # } >> "$RES"

  ELAPSED=$((SECONDS - START_TIME))
  echo "[ELAPSED], Seconds, $ELAPSED" 
  echo "    Done in ${ELAPSED}s"
}

tps_abort() {
  local LABEL="$1" SERVER="$2" THREADS="$3" WL="$4" RESULT_DIR="$5" BACKUP_DIR="$6"
  
  START_TIME=$SECONDS
  echo "=== [${LABEL}] $SERVER t$THREADS === $(date '+%H:%M:%S')" >> "/home/hjin/RCC/temp.out"

  local RES="$RESULT_DIR/${LABEL}_${SERVER}.txt"
  read -r COMMITS ABORTS ABORT_RATE TPS < <(awk -F', *' '
    $1 == "[BATCHTX]" && $2 == "Operations" { ok   = $3 }
    $1 == "[WASTED]"  && $2 == "Operations" { fail = $3 }
    $1 == "[OVERALL]" && $2 == "RunTime(ms)" { ms  = $3 }
    END {
      total = ok + fail
      printf "%d %d %.4f %.2f\n", ok, fail,
             (total > 0 ? fail / total : 0),
             (ms    > 0 ? ok * 1000.0 / ms : 0)
    }' "$RES")

  {
    echo "${ABORT_RATE:-0}"
    echo "${TPS:-0}"
    echo ""
  } >> "/home/hjin/RCC/temp.out"

}
