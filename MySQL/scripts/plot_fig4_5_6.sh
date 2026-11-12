#!/usr/bin/env bash
# Collect Fig4/5/6 summaries and write plots under MySQL/results/plots/.
set -euo pipefail
exec python3 "$(dirname "$(realpath "$0")")/plot_figures.py" "$@"
