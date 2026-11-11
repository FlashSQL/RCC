#!/usr/bin/env bash
# Build the imported TPC-C loader/client against the chosen MySQL installation.
set -euo pipefail
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MYSQL_CONFIG="$BASE_DIR/Vanilla/mysql-8.4.5-install/bin/mysql_config"
JOBS="${BUILD_JOBS:-$(nproc)}"
DRY_RUN=false
while (( $# )); do
    case "$1" in
        --mysql-config) MYSQL_CONFIG="${2:?--mysql-config requires a path}"; shift 2 ;;
        --jobs) JOBS="${2:?--jobs requires a number}"; shift 2 ;;
        --dry-run) DRY_RUN=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--mysql-config /path/to/mysql_config] [--jobs N] [--dry-run]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be positive" >&2; exit 1; }
MYSQL_CONFIG="$(realpath -m "$MYSQL_CONFIG")"
cmd=(make -B -C "$BASE_DIR/tpcc-mysql/src" -j"$JOBS" "MYSQL_CONFIG=$MYSQL_CONFIG" all)
if "$DRY_RUN"; then
    printf '%q ' "${cmd[@]}"; printf '\n'
    exit 0
fi
[[ -x "$MYSQL_CONFIG" ]] || { echo "Build Vanilla first or pass --mysql-config" >&2; exit 1; }
exec "${cmd[@]}"
