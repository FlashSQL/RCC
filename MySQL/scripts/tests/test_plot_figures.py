"""Check data selection and scientific units without running a database."""
import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("plot_figures", Path(__file__).parents[1] / "plot_figures.py")
plots = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(plots)


def measurement(**overrides):
    row = dict(figure="fig4", workload="sysbench", engine="RCC", rows=10000000,
               table_rows=10000000, threads=64, repeat=1, commits=100, aborts=25,
               failures=0, measured_seconds=5, tps=20, abort_rate=0.2,
               successful_attempt_ms=1000, aborted_attempt_ms=500,
               row_lock_wait_ms=200, commit_wait_ms=100, consistency_check="not_applicable")
    row.update(overrides)
    return row


def summary(root, stamp, rows):
    path = root / stamp / "summary.csv"
    path.parent.mkdir(parents=True)
    fields = [k for k in plots.RAW_FIELDS if k not in ("source_csv", "source_line")]
    plots.write_csv(path, fields, rows)
    return path


class Collection(unittest.TestCase):
    def test_partial_rerun_replaces_only_matching_case_and_repeat(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = summary(root, "20261111_120000_100", [
                measurement(tps=10), measurement(repeat=2, tps=30),
                measurement(engine="Bamboo", tps=5)])
            new = summary(root, "20261112_120000_200", [measurement(tps=40)])
            paths = plots.find_summaries([root, new, old])
            rows = plots.collect(paths, "fig4")
            self.assertEqual(len(rows), 3)
            rcc = next(r for r in plots.aggregate(rows) if r["engine"] == "RCC")
            self.assertEqual(rcc["samples"], 2)
            self.assertEqual(rcc["tps"], 35)
            selected = next(r for r in rows if r["engine"] == "RCC" and r["repeat"] == 1)
            self.assertEqual(selected["source_csv"], str(new))
            self.assertEqual(len(plots.collect(paths, "fig4", all_runs=True)), 4)

    def test_legacy_csv_without_consistency_or_wait_columns(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.csv"
            row = measurement(engine="Vanilla")
            for key in ("consistency_check", "commit_wait_ms"):
                row.pop(key)
            plots.write_csv(path, list(row), [row])
            result = plots.read_summary(path, "fig4")[0]
            self.assertEqual(result["consistency_check"], "unknown")
            self.assertIsNone(result["commit_wait_ms"])

    def test_invalid_rows_are_rejected_with_source_line(self):
        for change in (dict(tps="nan"), dict(commits=0), dict(abort_rate=20),
                       dict(failures=1), dict(row_lock_wait_ms=-1)):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                path = summary(Path(directory), "run", [measurement(**change)])
                with self.assertRaisesRegex(ValueError, r"summary.csv:2:"):
                    plots.read_summary(path, "fig4")

    def test_duplicate_case_in_one_summary_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = summary(Path(directory), "run", [measurement(), measurement()])
            with self.assertRaisesRegex(ValueError, "Duplicate case"):
                plots.collect([path], "fig4")


class Aggregation(unittest.TestCase):
    def aggregate(self, *rows):
        return plots.aggregate([dict(r, source_csv="fixture.csv") for r in rows])

    def test_time_normalized_before_averaging_and_abort_is_percent(self):
        row = self.aggregate(measurement(), measurement(commits=200, repeat=2, tps=40))[0]
        self.assertEqual(row["abort_pct"], 20)
        self.assertEqual(row["successful_ms"], 7.5)
        self.assertEqual(row["wasted_ms"], 3.75)
        self.assertEqual(row["lock_wait_ms"], 1.5)
        self.assertEqual(row["commit_wait_ms"], 0.75)
        self.assertEqual(row["execution_ms"], 5.25)
        self.assertEqual(row["time_split"], "split")
        self.assertAlmostEqual(row["tps_std"], 14.1421356237)

    def test_missing_or_overlapping_waits_preserve_successful_total(self):
        for change in (dict(commit_wait_ms=None), dict(row_lock_wait_ms=None),
                       dict(row_lock_wait_ms=1200)):
            with self.subTest(change=change):
                row = self.aggregate(measurement(**change))[0]
                self.assertEqual(row["time_split"], "unsplit")
                self.assertIsNone(row["execution_ms"])
                self.assertEqual(row["successful_ms"] + row["wasted_ms"], 15)

    def test_different_table_sizes_and_thread_sweeps_stay_separate(self):
        rows = self.aggregate(measurement(figure="fig6"),
                              measurement(figure="fig6", rows=100),
                              measurement(figure="fig6", threads=128),
                              measurement(figure="fig6", rows=100, table_rows=100))
        groups = plots.panels(rows, "fig6")
        self.assertEqual(len(groups), 3)
        self.assertEqual(sorted(len(group) for fixed, group in groups), [1, 1, 2])

    def test_collect_only_writes_selected_sources_and_means(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = summary(root / "Fig5", "20261112_120000_1", [
                measurement(figure="fig5", workload="tpcc", rows=0, table_rows=0,
                            consistency_check="skipped")])
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                plots.main(["--results-dir", str(root), "--figures", "fig5", "--collect-only"])
            with (root / "plots/Fig5/aggregated.csv").open() as stream:
                result = list(csv.DictReader(stream))
            self.assertEqual(result[0]["abort_pct"], "20.0")
            self.assertEqual(result[0]["consistency_checks"], "skipped")
            manifest = json.loads((root / "plots/sources.json").read_text())
            self.assertEqual(manifest["figures"]["fig5"]["selected_rows_by_source"], {str(path): 1})


if __name__ == "__main__":
    unittest.main()
