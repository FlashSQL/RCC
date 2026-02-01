#!/bin/bash

BASE_DIR=`pwd -P`
RESULT_DIR="$BASE_DIR/fig8_serializable/results"

# Check required variables
if [[ -z "$rcc_data_dir" ]] || [[ -z "$rcc_log_dir" ]]; then
    echo "[ERROR] rcc_data_dir and rcc_log_dir must be set"
    echo "  export rcc_data_dir=/path/to/data"
    echo "  export rcc_log_dir=/path/to/log"
    exit 1
fi

# Get sudo password once
if [[ -z "$PASSWD" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
    export PASSWD
fi

mkdir -p $rcc_data_dir

mkdir -p $RESULT_DIR

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

# Vanilla, OCC
BUILD_MODE="vanilla"
MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
BLD_PATH="$MYSQL_HOME-build"
MYSQL_BIN="${BLD_PATH}/bin/mysqld"
MYSQL="${BLD_PATH}/bin/mysql"

mkdir -p $RESULT_DIR/$BUILD_MODE

echo "[INFO] Cleaning data_dir and log_dir..."
rm -rf $rcc_data_dir/* 2>/dev/null
rm -rf $rcc_log_dir/* 2>/dev/null

# innodb_buffer_pool_size -> 64GB
sed -i -E 's|^[[:space:]]*innodb_buffer_pool_size[[:space:]]*=.*$|innodb_buffer_pool_size=64GB|' "$BASE_DIR/my.cnf"
sed -i -E 's|^[[:space:]]*innodb_buffer_pool_instances[[:space:]]*=.*$|innodb_buffer_pool_instances=64|' "$BASE_DIR/my.cnf"
sed -i -E 's|^[[:space:]]*innodb_page_cleaners[[:space:]]*=.*$|innodb_page_cleaners=64|' "$BASE_DIR/my.cnf"

# Build MySQL
bash run_server.sh --$BUILD_MODE

# Load data
$MYSQL -u root < $BASE_DIR/occ/create_table_occ.sql

# Vanilla
python3 $BASE_DIR/occ/2pl.py

# OCC
python3 $BASE_DIR/occ/occ.py


# RCC
BUILD_MODE="rcc"
BUILD_MODE="vanilla"
MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
BLD_PATH="$MYSQL_HOME-build"
MYSQL_BIN="${BLD_PATH}/bin/mysqld"
MYSQL="${BLD_PATH}/bin/mysql"

mkdir -p $RESULT_DIR/$BUILD_MODE

echo "[INFO] Cleaning data_dir and log_dir..."
rm -rf $rcc_data_dir/* 2>/dev/null
rm -rf $rcc_log_dir/* 2>/dev/null

bash run_server.sh --$BUILD_MODE
# Load data
$MYSQL -u root < $BASE_DIR/occ/create_table_occ.sql

python3 $BASE_DIR/occ/2pl.py