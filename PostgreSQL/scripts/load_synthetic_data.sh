#!/bin/bash

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

echo "RUN PostgreSQL"
$BASE_DIR/rcc_postgresql/bld/bin/postgres -D $DATA_DIR > /dev/null 2>&1 &
sleep 10

echo "CREATE DATABASE"
$BASE_DIR/rcc_postgresql/bld/bin/psql -U postgres -c "CREATE DATABASE stx;" 2>/dev/null

echo "CREATE TABLE AND LOAD DATA"
$BASE_DIR/rcc_postgresql/bld/bin/psql -U postgres -d stx -f $BASE_DIR/synthetic_benchmark/create_table_synthetic.sql

echo "Data loading complete. Stopping server..."
$BASE_DIR/rcc_postgresql/bld/bin/pg_ctl stop -D $DATA_DIR -m fast
echo "Server stopped. Ready for benchmark."
