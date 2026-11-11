#!/usr/bin/env bash
# Prepare a clean Vanilla snapshot: one warehouse, 100K items.
set -euo pipefail
exec python3 "$(dirname "$(realpath "$0")")/experiments.py" load-tpcc "$@"
