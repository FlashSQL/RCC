#!/bin/bash
set -Eeuo pipefail

BASE_DIR="$(pwd -P)"

##### YOU CAN CUSTOMIZE HERE #####
W_LIST=(1 2 4 8)          # Warehouse Count
C_LIST=(16 32 64 128)     # Client Threads Count
R="50"                    # Warm-up Time
L="50"                    # Runtime
##################################

mysql_dir="$BASE_DIR/mysql-8.4.5-build"
tpcc_dir="$BASE_DIR/tpcc-mysql"

# sanity check: lengths must match
if [[ ${#W_LIST[@]} -ne ${#C_LIST[@]} ]]; then
  echo "Error: W_LIST (len=${#W_LIST[@]}) and C_LIST (len=${#C_LIST[@]}) must have the same length."
  exit 1
fi

for ((i=0; i<${#W_LIST[@]}; i++)); do
  W="${W_LIST[$i]}"
  C="${C_LIST[$i]}"
  echo "=== Running TPCC: W=$W, C=$C (R=$R, L=$L) ==="

  LD_LIBRARY_PATH="$mysql_dir/lib" \
    "$tpcc_dir/tpcc_start" -S /tmp/mysql.sock -d tpcc -u root \
    -w "$W" -c "$C" -r "$R" -l "$L"
done
