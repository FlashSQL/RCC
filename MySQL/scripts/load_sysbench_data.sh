#!/usr/bin/env bash
# Prepare the shared 10M-row Vanilla snapshot for Fig. 4/6; reuse an existing matching snapshot.
set -euo pipefail
exec python3 "$(dirname "$(realpath "$0")")/experiments.py" load-sysbench "$@"
