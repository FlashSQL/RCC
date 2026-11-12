#!/usr/bin/env python3
"""Collect MySQL Fig. 4/5/6 summaries and plot throughput, time, and abort rate."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import re
import statistics
import sys

BASE = Path(__file__).resolve().parent.parent
FIGURES = ("fig4", "fig5", "fig6")
ENGINES = ("Vanilla", "Bamboo", "Rebirth", "RCC", "RCC-C")
COLORS = dict(zip(ENGINES, ("#777777", "#699158", "#637cb3", "#ff7f0e", "#de556f")))
MARKERS = dict(zip(ENGINES, ("o", "v", "+", "s", "D")))
LABELS = dict(zip(ENGINES, ("VAN", "BB", "RR", "RCC", "RCC-C")))
KEY = ("figure", "engine", "rows", "table_rows", "threads")
INTEGER_FIELDS = ("rows", "table_rows", "threads", "repeat", "commits", "aborts", "failures")
FLOAT_FIELDS = ("measured_seconds", "tps", "abort_rate", "successful_attempt_ms", "aborted_attempt_ms")
WAIT_FIELDS = ("row_lock_wait_ms", "commit_wait_ms")
RAW_FIELDS = ("figure", "workload", "engine", *INTEGER_FIELDS, *FLOAT_FIELDS, *WAIT_FIELDS,
              "consistency_check", "source_csv", "source_line")
METRICS = ("tps", "abort_pct", "successful_ms", "wasted_ms", "lock_wait_ms", "commit_wait_ms")
AGG_FIELDS = (*KEY, "samples", "tps", "tps_std", "abort_pct", "abort_pct_std",
              "successful_ms", "wasted_ms", "lock_wait_ms", "commit_wait_ms", "execution_ms",
              "time_split", "consistency_checks", "sources")


def warn(message):
    print(f"[WARN] {message}", file=sys.stderr)


def case_key(row):
    return tuple(row[name] for name in KEY)


def source_order(path):
    # Runner timestamps, rather than copy/transfer mtimes, determine recency.
    match = re.fullmatch(r"(\d{8}_\d{6})(?:_(\d+))?", path.parent.name)
    if match:
        return (match[1], int(match[2] or 0), str(path))
    from datetime import datetime
    return (datetime.fromtimestamp(path.stat().st_mtime).strftime("%Y%m%d_%H%M%S"),
            0, str(path))


def find_summaries(inputs):
    paths = set()
    for value in inputs:
        path = value.expanduser().resolve()
        if path.is_file():
            paths.add(path)
        elif path.is_dir():
            paths.update(p.resolve() for p in path.rglob("summary.csv"))
        else:
            raise ValueError(f"Result path does not exist: {path}")
    return sorted(paths, key=source_order)


def read_summary(path, figure):
    records = []
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        required = set(KEY) | set(INTEGER_FIELDS) | set(FLOAT_FIELDS) | {"workload"}
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(sorted(missing))}")
        for line, raw in enumerate(reader, 2):
            try:
                if None in raw or any(value is None for value in raw.values()):
                    raise ValueError("incomplete CSV row (the run may still be writing)")
                row = dict(raw)
                row["figure"] = row["figure"].lower()
                if row["figure"] != figure:
                    continue
                if row["engine"] not in ENGINES:
                    raise ValueError(f"unknown engine: {row['engine']}")
                expected_workload = "tpcc" if figure == "fig5" else "sysbench"
                if row["workload"] != expected_workload:
                    raise ValueError(f"expected workload {expected_workload}")
                for name in INTEGER_FIELDS:
                    row[name] = int(row[name])
                    if row[name] < 0:
                        raise ValueError(f"negative {name}")
                for name in (*FLOAT_FIELDS, *WAIT_FIELDS):
                    value = row.get(name, "")
                    row[name] = None if name in WAIT_FIELDS and value == "" else float(value)
                    if row[name] is not None and (not math.isfinite(row[name]) or row[name] < 0):
                        raise ValueError(f"invalid {name}: {value}")
                if min(row[k] for k in ("commits", "threads", "repeat", "measured_seconds")) <= 0:
                    raise ValueError("commits, threads, repeat and measured_seconds must be positive")
                if row["failures"] or not 0 <= row["abort_rate"] <= 1:
                    raise ValueError("failed transactions or invalid abort fraction")
                if figure != "fig5" and not 0 < row["rows"] <= row["table_rows"]:
                    raise ValueError("invalid access range / table size")
                row["consistency_check"] = row.get("consistency_check") or "unknown"
                if row["consistency_check"] not in ("passed", "skipped", "not_applicable", "unknown"):
                    raise ValueError(f"invalid consistency status: {row['consistency_check']}")
                row.update(source_csv=str(path), source_line=line)
                records.append(row)
            except (TypeError, ValueError) as error:
                raise ValueError(f"{path}:{line}: {error}") from error
    return records


def collect(paths, figure, all_runs=False):
    records = [row for path in paths for row in read_summary(path, figure)]
    seen = set()
    for row in records:
        key = (row["source_csv"], *case_key(row), row["repeat"])
        if key in seen:
            raise ValueError(f"Duplicate case/repeat in {row['source_csv']}: {key[2:]}")
        seen.add(key)
    if not all_runs:
        latest = {}
        for row in records:
            latest[(*case_key(row), row["repeat"])] = row
        records = list(latest.values())
    return sorted(records, key=lambda r: (*case_key(r), r["repeat"], r["source_csv"]))


def normalized_times(row):
    commits = row["commits"]
    success = row["successful_attempt_ms"] / commits
    lock = None if row["row_lock_wait_ms"] is None else row["row_lock_wait_ms"] / commits
    commit = None if row["commit_wait_ms"] is None else row["commit_wait_ms"] / commits
    # Wait counters overlap successful time. Never add them to that total,
    # assume missing waits are zero, or clamp an inconsistent residual to zero.
    split = lock is not None and commit is not None and lock + commit <= success
    return dict(successful_ms=success, wasted_ms=row["aborted_attempt_ms"] / commits,
                lock_wait_ms=lock, commit_wait_ms=commit,
                execution_ms=success - lock - commit if split else None)


def aggregate(records):
    groups = defaultdict(list)
    for row in records:
        groups[case_key(row)].append(row)
    output = []
    for key, rows in sorted(groups.items()):
        normalized = [dict(tps=r["tps"], abort_pct=100 * r["abort_rate"],
                           **normalized_times(r)) for r in rows]
        result = dict(zip(KEY, key), samples=len(rows))
        for metric in (*METRICS, "execution_ms"):
            values = [r[metric] for r in normalized]
            result[metric] = statistics.mean(values) if all(v is not None for v in values) else None
            if metric in ("tps", "abort_pct"):
                result[metric + "_std"] = statistics.stdev(values) if len(values) > 1 else 0.0
        result["time_split"] = "split" if result["execution_ms"] is not None else "unsplit"
        result["consistency_checks"] = ";".join(sorted({r["consistency_check"] for r in rows}))
        result["sources"] = ";".join(sorted({r["source_csv"] for r in rows}))
        output.append(result)
    return output


def panels(rows, figure):
    # Extra --rows (Fig4) or --threads (Fig6) sweeps must not be averaged together.
    fixed = ("threads", "table_rows") if figure == "fig6" else ("rows", "table_rows")
    groups = defaultdict(list)
    for row in rows:
        groups[tuple(row[k] for k in fixed)].append(row)
    return [(dict(zip(fixed, key)), values) for key, values in sorted(groups.items())]


def write_csv(path, fields, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def size_label(number):
    if number == 100:
        return "0.1K"
    for scale, suffix in ((1000000, "M"), (1000, "K")):
        if number >= scale and number % scale == 0:
            return f"{number // scale}{suffix}"
    return str(number)


def style_axis(ax, xlabel, ylabel, title):
    ax.set(xlabel=xlabel, ylabel=ylabel, title=title)
    ax.set_ylim(bottom=0)
    ax.grid(axis="y", color="#dddddd", linewidth=0.7)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def plot_panel(plt, rows, figure, fixed, output, formats, suffix):
    from matplotlib.patches import Patch
    from matplotlib.ticker import FuncFormatter

    axis = "rows" if figure == "fig6" else "threads"
    xs = sorted({r[axis] for r in rows}, reverse=figure == "fig6")
    engines = [e for e in ENGINES if any(r["engine"] == e for r in rows)]
    by_point = {(r["engine"], r[axis]): r for r in rows}
    tick_labels = [size_label(x) if axis == "rows" else str(x) for x in xs]
    xlabel = "Access range (items)" if figure == "fig6" else "Number of threads"
    workload = "TPC-C" if figure == "fig5" else "YCSB"
    detail = (f"{fixed['threads']} threads, {size_label(fixed['table_rows'])}-row table"
              if figure == "fig6" else
              ("1 warehouse" if figure == "fig5" else f"{size_label(fixed['rows'])} items"))

    def save(fig, name):
        for fmt in formats:
            path = output / f"{name}{suffix}.{fmt}"
            fig.savefig(path, dpi=200, bbox_inches="tight")
            print(f"[plot] {path}")
        plt.close(fig)

    line_plots = [("tps", {"fig4": "fig4a_ycsb", "fig5": "fig5a_tpcc", "fig6": "fig6a_tps"}[figure],
                   "Throughput (TPS)", "a")]
    if figure == "fig6":
        line_plots.append(("abort_pct", "fig6b_abort", "Abort rate (%)", "b"))
    for metric, stem, ylabel, panel in line_plots:
        fig, ax = plt.subplots(figsize=(7, 4.3), constrained_layout=True)
        for engine in engines:
            points = [by_point.get((engine, x)) for x in xs]
            ys = [r[metric] if r else math.nan for r in points]
            errors = [r[metric + "_std"] if r else math.nan for r in points]
            ax.errorbar(range(len(xs)), ys, yerr=errors if any(r and r["samples"] > 1 for r in points) else None,
                        label=LABELS[engine], color=COLORS[engine], marker=MARKERS[engine],
                        linewidth=1.7, markersize=6, capsize=3)
        ax.set_xticks(range(len(xs)), tick_labels)
        style_axis(ax, xlabel, ylabel, f"Fig. {figure[-1]}({panel})  {workload} · {detail}")
        ax.legend(ncol=len(engines), loc="lower center", bbox_to_anchor=(0.5, 1.1), frameon=False)
        if metric == "abort_pct":
            ax.set_ylim(0, 100)
        else:
            ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y / 1000:g}K" if abs(y) >= 1000 else f"{y:g}"))
        save(fig, stem)

    if figure == "fig6":
        return
    fig, ax = plt.subplots(figsize=(9, 5.4))
    fig.subplots_adjust(left=0.10, right=0.98, bottom=0.17, top=0.71)
    width = 0.8 / len(engines)
    hatches = {"wasted_ms": "xxx", "commit_wait_ms": "---", "lock_wait_ms": "///",
               "execution_ms": "", "successful_ms": "..."}
    unsplit = any(r["time_split"] == "unsplit" for r in rows)
    for i, engine in enumerate(engines):
        for j, x in enumerate(xs):
            row = by_point.get((engine, x))
            if row is None:
                continue
            position = j - 0.4 + width * (i + 0.5)
            metrics = ["wasted_ms", "commit_wait_ms", "lock_wait_ms", "execution_ms"] if row["time_split"] == "split" else ["wasted_ms", "successful_ms"]
            bottom = 0
            for metric in metrics:
                height = row[metric]
                ax.bar(position, height, width=width * 0.9, bottom=bottom, color=COLORS[engine],
                       edgecolor="#333333", linewidth=0.45, hatch=hatches[metric])
                bottom += height
    ax.set_xticks(range(len(xs)), tick_labels)
    style_axis(ax, xlabel, "Amortized runtime (ms / commit)", f"Fig. {figure[-1]}(b)  {workload} · {detail}")
    fig.legend(handles=[Patch(facecolor=COLORS[e], label=LABELS[e]) for e in engines],
               loc="upper center", bbox_to_anchor=(0.54, 0.99), ncol=len(engines), frameon=False)
    components = [("Execution", "execution_ms"), ("Lock wait", "lock_wait_ms"),
                  ("Commit wait", "commit_wait_ms"), ("Wasted", "wasted_ms")]
    if unsplit:
        components.append(("Successful (unsplit)", "successful_ms"))
    fig.legend(handles=[Patch(facecolor="white", edgecolor="#333333", hatch=hatches[k], label=label)
                        for label, k in components], loc="upper center", bbox_to_anchor=(0.54, 0.93),
               ncol=3, frameon=False)
    note = "Times are normalized by committed transactions; bars show means across repetitions."
    if unsplit:
        note += "\nDotted fill: successful time kept whole because wait counters are missing or exceed it."
    fig.text(0.10, 0.025, note, fontsize=8, color="#444444")
    save(fig, "fig4b_ycsb_time" if figure == "fig4" else "fig5b_tpcc_time")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, default=BASE / "results",
                        help="Parent containing Fig4/, Fig5/ and Fig6/ (default: MySQL/results)")
    parser.add_argument("--output-dir", type=Path, help="Default: RESULTS_DIR/plots")
    parser.add_argument("--figures", nargs="+", choices=FIGURES, default=list(FIGURES))
    for figure in FIGURES:
        parser.add_argument(f"--{figure}-dir", type=Path, nargs="+",
                            help=f"Override {figure} inputs: run directories, figure directories, or CSV files")
    parser.add_argument("--all-runs", action="store_true",
                        help="Average all run directories; default keeps latest result per case/repeat")
    parser.add_argument("--formats", nargs="+", choices=("pdf", "png", "svg"), default=["pdf", "png"])
    parser.add_argument("--collect-only", action="store_true", help="Write CSVs without requiring matplotlib")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    args.results_dir = args.results_dir.expanduser().resolve()
    output = (args.output_dir or args.results_dir / "plots").expanduser().resolve()
    collected = {}
    manifest = {"selection": "all-runs" if args.all_runs else "latest-per-case-repeat", "figures": {}}
    # Validate all inputs before writing any output.
    for figure in dict.fromkeys(args.figures):
        inputs = getattr(args, figure + "_dir")
        if inputs is None:
            default = args.results_dir / figure.capitalize()
            if not default.is_dir():
                warn(f"{figure}: missing result directory {default}; skipping")
                continue
            inputs = [default]
        paths = find_summaries(inputs)
        rows = collect(paths, figure, args.all_runs)
        if not rows:
            warn(f"{figure}: no completed summary rows; skipping")
            continue
        collected[figure] = rows
        counts = Counter(r["source_csv"] for r in rows)
        manifest["figures"][figure] = {"input_csvs": [str(p) for p in paths],
                                       "selected_rows_by_source": dict(counts)}
        print(f"[collect] {figure}: {len(rows)} measurements from {len(counts)} selected run(s)")
        for path, count in sorted(counts.items()):
            print(f"  {count:3d} row(s): {path}")
        if len(counts) > 1 and not args.all_runs:
            warn(f"{figure}: combining run directories; older results fill points absent from newer summaries")
        skipped = sum(r["consistency_check"] == "skipped" for r in rows)
        if skipped:
            warn(f"{figure}: {skipped} selected measurement(s) skipped consistency checks; see collected.csv")
    if not collected:
        raise ValueError("No Fig4/5/6 results found; run an experiment or supply --results-dir / --figN-dir")
    plt = None
    if not args.collect_only:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError as error:
            raise ValueError("Plotting requires matplotlib: python3 -m pip install matplotlib; "
                             "or use --collect-only") from error
        plt.rcParams.update({"font.size": 11, "axes.titlesize": 12, "pdf.fonttype": 42})
    output.mkdir(parents=True, exist_ok=True)
    for figure, records in collected.items():
        destination = output / figure.capitalize()
        destination.mkdir(exist_ok=True)
        rows = aggregate(records)
        write_csv(destination / "collected.csv", RAW_FIELDS, records)
        write_csv(destination / "aggregated.csv", AGG_FIELDS, rows)
        groups = panels(rows, figure)
        if plt is not None:
            for fixed, group in groups:
                suffix = "" if len(groups) == 1 else "_" + "_".join(f"{k}{v}" for k, v in fixed.items())
                plot_panel(plt, group, figure, fixed, destination, args.formats, suffix)
    (output / "sources.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"[done] CSVs and source manifest: {output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, csv.Error) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        sys.exit(1)
