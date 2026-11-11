#!/bin/bash
set -e

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$BASE_DIR/scripts/build_config.sh"
BUILD_MODE=RCC

while [[ $# -gt 0 ]]; do
    case "$1" in
        --vanilla) BUILD_MODE=Vanilla ;;
        --rcc) BUILD_MODE=RCC ;;
        --rcc-c) BUILD_MODE=RCC-C ;;
        --bamboo) BUILD_MODE=Bamboo ;;
        --rebirth) BUILD_MODE=Rebirth ;;
        -h|--help)
            echo "Usage: $0 [--vanilla|--rcc|--rcc-c|--bamboo|--rebirth]"
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1" >&2
            exit 1
            ;;
    esac
    shift
done

mysql_build_config "$BUILD_MODE"
exec "$INSTALL_PATH/bin/mysqladmin" -uroot shutdown
