#!/bin/bash
set -Eeuo pipefail

BASE_DIR="$(pwd -P)"

##### YOU CAN CUSTOMIZE HERE #####
W=1 # Warehouse Count
C_LIST=(1 8 16 32 64 128) # Client Threads Count
R="50" # Warm-up Time
L="50" # Runtime
##################################

while [[ $# -gt 0 ]]; do
    case $1 in
        --vanilla)
            BUILD_MODE="vanilla"
            shift
            ;;
        --rcc)
            BUILD_MODE="rcc"
            shift
            ;;
        --bamboo)
            BUILD_MODE="bamboo"
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            ;;
    esac
done

MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
BLD_PATH="$MYSQL_HOME-build"

tpcc_dir="$BASE_DIR/tpcc-mysql"

for C in "${C_LIST[@]}"; do
  echo "=== Running TPCC: W=$W, C=$C (R=$R, L=$L) ==="
  LD_LIBRARY_PATH="$BLD_PATH/lib" \
    "$tpcc_dir/tpcc_start" -S /tmp/mysql.sock -d tpcc -u root \
    -w "$W" -c "$C" -r "$R" -l "$L"
done
