#!/usr/bin/env bash
# Run the current MySQL paper figures; options are forwarded to each figure.
set -euo pipefail
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for experiment in fig4_sysbench_threads fig5_tpcc_threads fig6_sysbench_rows; do
    bash "$BASE_DIR/scripts/$experiment.sh" "$@"
done
