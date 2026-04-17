#!/bin/bash

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_MODE=$1

# Use environment variables
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"

if [[ -z "$DATA_DIR" ]] || [[ -z "$LOG_DIR" ]]; then
    echo "[ERROR] rcc_data_dir and rcc_log_dir must be set"
    echo "  export rcc_data_dir=/path/to/data"
    echo "  export rcc_log_dir=/path/to/log"
    exit 1
fi

if [[ -z "$BUILD_MODE" ]]; then
    echo "[ERROR] BUILD_MODE is required"
    echo "  Usage: $0 <Vanilla|RCC|RCC-W|RCC-D|RCC-C>"
    exit 1
fi

MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
BLD_PATH="$MYSQL_HOME-build"
MYSQLD="${BLD_PATH}/bin/mysqld"
MYSQL="${BLD_PATH}/bin/mysql"
MYSQLADMIN="${BLD_PATH}/bin/mysqladmin"

mkdir -p $DATA_DIR
mkdir -p $LOG_DIR
rm -rf $DATA_DIR/*
rm -rf $LOG_DIR/*

# Update my.cnf paths
if grep -Eq '^[[:space:]]*pid-file[[:space:]]*=' "$BASE_DIR/my.cnf"; then
    sed -i -E "s|^[[:space:]]*pid-file[[:space:]]*=.*$|pid-file=${DATA_DIR}/mysql.pid|" "$BASE_DIR/my.cnf"
fi
if grep -Eq '^[[:space:]]*datadir[[:space:]]*=' "$BASE_DIR/my.cnf"; then
    sed -i -E "s|^[[:space:]]*datadir[[:space:]]*=.*$|datadir=${DATA_DIR}|" "$BASE_DIR/my.cnf"
fi
if grep -Eq '^[[:space:]]*log-error[[:space:]]*=' "$BASE_DIR/my.cnf"; then
    sed -i -E "s|^[[:space:]]*log-error[[:space:]]*=.*$|log-error=${LOG_DIR}/mysql_error.log|" "$BASE_DIR/my.cnf"
fi
if grep -Eq '^[[:space:]]*innodb_log_group_home_dir[[:space:]]*=' "$BASE_DIR/my.cnf"; then
    sed -i -E "s|^[[:space:]]*innodb_log_group_home_dir[[:space:]]*=.*$|innodb_log_group_home_dir=${LOG_DIR}|" "$BASE_DIR/my.cnf"
fi

echo "INIT MySQL"
$MYSQLD --defaults-file=$BASE_DIR/my.cnf --initialize-insecure --user=mysql --datadir=$DATA_DIR --basedir=$MYSQL_HOME
sleep 10

echo "RUN MySQL"
$MYSQLD --defaults-file=$BASE_DIR/my.cnf --datadir=$DATA_DIR --disable-log-bin > /dev/null 2>&1 &
sleep 20

echo "SET PASSWORD on MySQL"
$MYSQL -uroot mysql -e "DROP USER IF EXISTS 'root'@'%';"
$MYSQL -uroot mysql -e "CREATE USER 'root'@'%' IDENTIFIED BY '';"
$MYSQL -uroot mysql -e "GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;"
$MYSQL -uroot mysql -e "FLUSH PRIVILEGES;"

echo "CREATE TABLE AND LOAD DATA"
$MYSQL -uroot < $BASE_DIR/synthetic_benchmark/create_table_synthetic.sql

echo "Data loading complete. Stopping server..."
$MYSQLADMIN -uroot shutdown
sleep 5
echo "Server stopped. Ready for benchmark."
