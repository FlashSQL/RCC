#!/bin/bash
set -Eeuo pipefail

BASE_DIR="$(pwd -P)"

##### YOU CAN CUSTOMIZE HERE #####
W=1 # Warehouse Count
C_LIST=(1 8 16 32 64 128) # Client Threads Count
R="50" # Warm-up Time
L="50" # Runtime
##################################

mysql_dir="$BASE_DIR/mysql-8.4.5-build"
tpcc_dir="$BASE_DIR/tpcc-mysql"

for C in "${C_LIST[@]}"; do
  echo "=== Running TPCC: W=$W, C=$C (R=$R, L=$L) ==="
  LD_LIBRARY_PATH="$mysql_dir/lib" \
    "$tpcc_dir/tpcc_start" -S /tmp/mysql.sock -d tpcc -u root \
    -w "$W" -c "$C" -r "$R" -l "$L"
done
