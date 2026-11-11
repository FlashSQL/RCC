#!/bin/bash
set -eo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Use environment variables
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"

if [[ -z "$DATA_DIR" ]] || [[ -z "$LOG_DIR" ]]; then
    echo "[ERROR] rcc_data_dir and rcc_log_dir must be set"
    echo "  export rcc_data_dir=/path/to/data"
    echo "  export rcc_log_dir=/path/to/log"
    exit 1
fi

mkdir -p $DATA_DIR
mkdir -p $LOG_DIR
rm -rf $DATA_DIR/*
rm -rf $LOG_DIR/*

echo "INIT PostgreSQL"
$BASE_DIR/rcc_postgresql/bld/bin/initdb -D $DATA_DIR -U postgres
sleep 5

# Copy configuration and update log directory
cp $BASE_DIR/postgresql.conf $DATA_DIR/postgresql.conf
sed -i "s|log_directory = '.*'|log_directory = '$LOG_DIR'|" $DATA_DIR/postgresql.conf

# Configure pg_hba.conf for local trust authentication
cat > $DATA_DIR/pg_hba.conf << 'EOF'
# TYPE  DATABASE        USER            ADDRESS                 METHOD
local   all             all                                     trust
host    all             all             127.0.0.1/32            trust
host    all             all             ::1/128                 trust
EOF

echo "RUN PostgreSQL (load settings: fewer checkpoints, more index-build memory; the benchmark server uses postgresql.conf as is)"
"$BASE_DIR/rcc_postgresql/bld/bin/pg_ctl" start -D "$DATA_DIR" -l "$LOG_DIR/startup.log" -w -t 300 \
    -o "-c max_wal_size=64GB -c checkpoint_timeout=1h -c maintenance_work_mem=1GB"

echo "CREATE DATABASE"
$BASE_DIR/rcc_postgresql/bld/bin/psql -U postgres -c "CREATE DATABASE synt;" 2>/dev/null

echo "CREATE TABLE"
$BASE_DIR/rcc_postgresql/bld/bin/psql -U postgres -v ON_ERROR_STOP=1 -d synt -f $BASE_DIR/synthetic_benchmark/create_table_synthetic.sql

# Rows of the 128 read partitions, in parallel: one statement per partition (100,000 rows, then its id index)
LOAD_JOBS="${LOAD_JOBS:-32}"
echo "LOAD DATA: 128 partitions x 100,000 rows, $LOAD_JOBS sessions"
load_start=$SECONDS
seq 0 127 | xargs -P "$LOAD_JOBS" -I{} "$BASE_DIR/rcc_postgresql/bld/bin/psql" -U postgres -v ON_ERROR_STOP=1 -d synt -q -c \
    "INSERT INTO bench_hotread (p, val, payload) SELECT {}, 0, repeat('x', 2048)::bytea FROM seq1000 a CROSS JOIN seq100 b; CREATE INDEX idx_bench_hotread_p{}_id ON bench_hotread_p{}(id);"
$BASE_DIR/rcc_postgresql/bld/bin/psql -U postgres -v ON_ERROR_STOP=1 -d synt -c "ANALYZE bench_hotread;"
rows=$($BASE_DIR/rcc_postgresql/bld/bin/psql -U postgres -d synt -tAc "SELECT count(*) FROM bench_hotread")
echo "LOADED $rows rows in $((SECONDS - load_start)) s"
[[ "$rows" == "12800001" ]] || { echo "[ERROR] expected 12800001 rows (128 x 100000 + hotspot), got $rows"; exit 1; }

echo "Data loading complete. Stopping server..."
$BASE_DIR/rcc_postgresql/bld/bin/pg_ctl stop -D $DATA_DIR -w -t 300
echo "Server stopped. Ready for benchmark."
