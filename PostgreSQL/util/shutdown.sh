#!/bin/bash

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Parse arguments
DATA_DIR=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --data-dir) DATA_DIR="$2"; shift 2 ;;
        *) echo "[ERROR] Unknown option: $1"; exit 1 ;;
    esac
done

if [[ -z "$DATA_DIR" ]]; then
    echo "Usage: $0 --data-dir <path>"
    exit 1
fi

$BASE_DIR/rcc_postgresql/bld/bin/pg_ctl stop -D $DATA_DIR -w -t 300
