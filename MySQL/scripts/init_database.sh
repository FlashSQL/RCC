#!/bin/bash
# Initialize MySQL data directory for a given build variant.
# Called by MySQL/run_server.sh when DATA_DIR is empty.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_MODE=$1

if [[ -z "$BUILD_MODE" ]]; then
    echo "[ERROR] BUILD_MODE required" >&2
    echo "  Usage: $0 <Vanilla|RCC|RCC-C|Bamboo|Rebirth>" >&2
    exit 1
fi

DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"

if [[ -z "$DATA_DIR" ]] || [[ -z "$LOG_DIR" ]]; then
    echo "[ERROR] rcc_data_dir and rcc_log_dir must be set" >&2
    exit 1
fi

source "$BASE_DIR/scripts/build_config.sh"
mysql_build_config "$BUILD_MODE" || exit 1
MYSQLD="${INSTALL_PATH}/bin/mysqld"

if [[ ! -x "$MYSQLD" ]]; then
    echo "[ERROR] mysqld not found at $MYSQLD" >&2
    echo "  Build it first: bash run_server.sh --$(echo $BUILD_MODE | tr '[:upper:]' '[:lower:]')" >&2
    exit 1
fi

mkdir -p "$DATA_DIR" "$LOG_DIR"

$MYSQLD --defaults-file="$BASE_DIR/my.cnf" \
        --initialize-insecure \
        --user=mysql \
        --datadir="$DATA_DIR" \
        --basedir="$INSTALL_PATH"
