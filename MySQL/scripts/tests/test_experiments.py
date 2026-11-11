"""Offline checks: plans, measurement parsing, snapshot publication and server ownership."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

SPEC = importlib.util.spec_from_file_location("experiments", Path(__file__).parents[1] / "experiments.py")
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


def tpcc_output(seconds=30, extra=False, threads=64):
    return f"""THREADS_COMPLETED: {threads}
<Raw Results>
  [0] sc:9999 lt:9999 rt:9999 fl:0
 in {seconds} sec.
TOTAL_COMMIT: 1500
TOTAL_WASTE: 250
<Raw Results2(sum ver.)>
  [0] sc:70  lt:10  rt:20  random_rt:0  fl:0
  [1] sc:15  lt:5  rt:5  random_rt:0  fl:0
  [2] sc:{int(extra)}  lt:0  rt:0  random_rt:0  fl:0
  [3] sc:0  lt:0  rt:0  random_rt:0  fl:0
  [4] sc:0  lt:0  rt:0  random_rt:0  fl:0
<Constraint Check>
"""


class Plans(unittest.TestCase):
    def test_requested_matrix(self):
        for figure, count in (("fig4", 24), ("fig5", 30), ("fig6", 15)):
            args = bench.parse_args([figure])
            cases = bench.plan(args)
            self.assertEqual(len(cases), count)
            self.assertEqual((args.warmup, args.runtime), (30, 30))
            if figure == "fig5":
                self.assertEqual({c["workload"] for c in cases}, {"tpcc"})
            else:
                self.assertEqual({c["table_rows"] for c in cases}, {10000000})
            if figure == "fig6":
                self.assertEqual({c["threads"] for c in cases}, {64})
                self.assertEqual([c["rows"] for c in cases[:5]], [10000000, 100000, 10000, 1000, 100])
            else:
                self.assertEqual([c["threads"] for c in cases[:6]], [1, 16, 32, 64, 96, 128])

    def test_one_sysbench_snapshot_for_both_figures(self):
        args = bench.parse_args(["load-sysbench"])
        self.assertEqual(bench.plan(args), [dict(workload="sysbench", table_rows=10000000)])

    def test_invalid_settings(self):
        for flags in (("--rows", "15"), ("--rows", "11M"), ("--engines", "RCC-D"),
                      ("--runtime", "0"), ("--threads", "0"), ("--repeat", "4")):
            with self.assertRaises(RuntimeError):
                bench.parse_args(["fig6", *flags])

    def test_dry_run_has_no_side_effects(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory) / "not-created"
            with patch.object(bench, "check_tools", side_effect=AssertionError("checked runtime tools")):
                with contextlib.redirect_stdout(io.StringIO()):
                    bench.main(["fig6", "--work-dir", str(work), "--dry-run"])
            self.assertFalse(work.exists())

    def test_both_commands_exclude_warmup_from_measurement(self):
        args = bench.parse_args(["fig4"])
        server = type("Server", (), {"socket": Path("/tmp/rcc-test/mysql.sock")})()
        command = bench.sysbench_command(args, server, 100, 64, 30)
        for option in ("--table-size=100", "--threads=64", "--time=30", "--rand-zipfian-exp=0.9",
                       "--point-selects=8", "--non-index-updates=8"):
            self.assertIn(option, command)
        self.assertEqual(bench.client_env(args)["SB_SORT"], "1")
        tpcc = bench.tpcc_command(server, 64, 30)
        self.assertEqual(tpcc[tpcc.index("-r") + 1], "0")
        self.assertEqual(tpcc[tpcc.index("-l") + 1], "30")
        self.assertEqual(tpcc[tpcc.index("-w") + 1], "1")
        self.assertFalse(any(option in tpcc for option in ("-0", "-1", "-2", "-3", "-4")))


class Metrics(unittest.TestCase):
    def test_sysbench_11_elapsed_time_format(self):
        text = "[TIME] commits=1979 aborts=0 exec_us=1961195 wasted_us=0\n" \
               "Throughput:\n    time elapsed:                        2.0051s\n"
        result = bench.parse_metrics("sysbench", text, 2, 1)
        self.assertEqual(result["commits"], 1979)
        self.assertAlmostEqual(result["measured_seconds"], 2.0051)
        self.assertAlmostEqual(result["tps"], 1979 / 2.0051)

    def test_tpcc_consistency_rejects_violations_and_missing_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            valid = "OK   \t0\tconstraint\n" * 7
            server = type("Server", (), {"output": Path(directory)})()
            server.mysql = Mock(return_value=valid)
            bench.check_tpcc_consistency(server)
            sql, database = server.mysql.call_args.args
            self.assertEqual(database, "tpcc")
            self.assertTrue(sql.startswith("START TRANSACTION;"))
            self.assertTrue(sql.endswith("COMMIT;"))
            for invalid in (valid.replace("OK   \t0", "ERROR\t1", 1),
                            "OK\t0\tconstraint\n" * 6):
                server.mysql = lambda *args: invalid
                with self.assertRaisesRegex(RuntimeError, "consistency check failed"):
                    bench.check_tpcc_consistency(server)

    def test_sysbench_interleaved_thread_summaries(self):
        text = "total time: 30.25s\n[TIME] commits=10 aborts=3 exec_us=3000 wasted_us=1000[TIME] commits=20 aborts=7 exec_us=5000 wasted_us=2000\n"
        result = bench.parse_metrics("sysbench", text, 30, 2)
        self.assertEqual(result["commits"], 30)
        self.assertEqual(result["abort_rate"], 0.25)
        self.assertEqual(result["successful_attempt_ms"], 8)
        self.assertAlmostEqual(result["tps"], 30 / 30.25)
        with self.assertRaises(RuntimeError):
            bench.parse_metrics("sysbench", text, 30, 3)

    def test_tpcc_uses_per_thread_totals(self):
        result = bench.parse_metrics("tpcc", tpcc_output(), 30, 64)
        self.assertEqual(result["commits"], 100)
        self.assertEqual(result["aborts"], 25)
        self.assertEqual(result["abort_rate"], 0.2)
        self.assertEqual(result["successful_attempt_ms"], 1500)

    def test_incomplete_or_wrong_tpcc_runs_fail(self):
        for text in (tpcc_output(20), tpcc_output(extra=True), tpcc_output(threads=63),
                     tpcc_output().replace("THREADS_COMPLETED: 64", ""),
                     tpcc_output().replace("fl:0", "fl:1"), ""):
            with self.assertRaises(RuntimeError):
                bench.parse_metrics("tpcc", text, 30, 64)


class FakeServer:
    instances = []
    fail_clean_stop = False

    def __init__(self, args, prefix, data, redo, output):
        self.args, self.data, self.redo, self.output = args, data, redo, output
        self.socket = Path("/tmp/fake-rcc.sock")
        self.socket_dir = output / "socket"
        self.socket_dir.mkdir()
        self.process = None
        self.sql = []
        self.instances.append(self)

    def initialize(self):
        self.data.mkdir(); self.redo.mkdir()
        (self.data / "ibdata1").write_text("clean snapshot")

    def start(self):
        self.process = object()

    def mysql(self, sql, database=None, timeout=600):
        self.sql.append(sql)
        if "SELECT COUNT(*) FROM sbtest.sbtest1" in sql:
            return "10000000\n"
        if sql.startswith("SHOW GLOBAL STATUS"):
            count = sum(s.startswith("SHOW GLOBAL STATUS") for s in self.sql)
            return f"Innodb_row_lock_time\t{count * 10}\nInnodb_row_commit_time\t{count * 3}\n"
        return ""

    def stop(self, clean=True):
        if clean and self.fail_clean_stop:
            raise RuntimeError("unclean shutdown")
        self.process = None
        if self.socket_dir.exists():
            self.socket_dir.rmdir()


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.args = bench.parse_args(["fig4", "--engines", "Vanilla", "--threads", "1",
                                      "--work-dir", str(self.root), "--numa", "none"])
        FakeServer.instances = []
        FakeServer.fail_clean_stop = False

    def tearDown(self):
        self.temp.cleanup()

    def test_start_retries_connection_errors_until_ready(self):
        output = self.root / "startup"; output.mkdir()
        server = bench.Server(self.args, self.args.loader_prefix,
                              self.root / "data", self.root / "redo", output)
        process = Mock(pid=os.getpid())
        process.poll.return_value = None
        replies = [RuntimeError("socket not ready"), "1\n", "",
                   "SERIALIZABLE\tSERIALIZABLE\n", "transaction_isolation\tSERIALIZABLE\n"]
        try:
            with patch.object(bench.subprocess, "Popen", return_value=process), \
                    patch.object(server, "mysql", side_effect=replies), \
                    patch.object(bench.time, "sleep") as sleep:
                server.start()
            sleep.assert_called_once_with(1)
            self.assertTrue((output / "runtime.tsv").is_file())
        finally:
            if server.console:
                server.console.close()
            shutil.rmtree(server.socket_dir)

    def test_snapshot_is_reusable_only_after_clean_stop(self):
        with patch.object(bench, "Server", FakeServer), patch.object(bench, "logged") as logged:
            bench.prepare_dataset(self.args, "sysbench", 10000000)
            destination = bench.check_dataset(self.args, "sysbench", 10000000)
            self.assertTrue((destination / "dataset.json").exists())
            bench.prepare_dataset(self.args, "sysbench", 10000000)
            self.assertEqual(logged.call_count, 1)
            self.args.partitions = 0
            with self.assertRaises(RuntimeError):
                bench.check_dataset(self.args, "sysbench", 10000000)

    def test_failed_shutdown_never_publishes_dataset(self):
        FakeServer.fail_clean_stop = True
        with patch.object(bench, "Server", FakeServer), patch.object(bench, "logged"):
            with self.assertRaisesRegex(RuntimeError, "unclean shutdown"):
                bench.prepare_dataset(self.args, "sysbench", 10000000)
        self.assertFalse(bench.dataset_path(self.args, "sysbench", 10000000).exists())
        self.assertIsNone(FakeServer.instances[-1].process)

    def test_tpcc_checks_persisted_data_after_measured_server_stops(self):
        self.args.experiment = "fig5"
        snapshot = bench.dataset_path(self.args, "tpcc", 0)
        (snapshot / "data").mkdir(parents=True)
        (snapshot / "data/ibdata1").write_text("snapshot")
        (snapshot / "redo").mkdir()
        bench.write_json(snapshot / "dataset.json", bench.dataset_spec(self.args, "tpcc", 0))
        (self.root / "runs").mkdir()
        results = self.root / "results"; results.mkdir()

        def log(command, output, phase, args, seconds):
            (output / (phase + ".out")).write_text(tpcc_output(threads=1))
            (output / (phase + ".err")).write_text("")

        def check(checker):
            self.assertEqual(len(FakeServer.instances), 2)
            self.assertIsNone(FakeServer.instances[0].process)
            self.assertIsNotNone(checker.process)
            (checker.output / "consistency.tsv").write_text("OK\t0\tconstraint\n" * 7)

        with patch.object(bench, "Server", FakeServer), \
                patch.object(bench, "logged", side_effect=log), \
                patch.object(bench, "check_tpcc_consistency", side_effect=check):
            bench.run_case(self.args, bench.plan(self.args)[0], results, {"Vanilla": self.args.loader_prefix})
        self.assertTrue(next(results.glob("*/consistency.tsv")).is_file())
        self.assertTrue((results / "summary.csv").is_file())

    def test_measurement_uses_fresh_copy_and_excludes_warmup(self):
        with patch.object(bench, "Server", FakeServer), patch.object(bench, "logged"):
            bench.prepare_dataset(self.args, "sysbench", 10000000)
        (self.root / "runs").mkdir()
        results = self.root / "results"; results.mkdir()
        phases = []

        def log(command, output, phase, args, seconds):
            phases.append(phase)
            commits = 999 if phase == "warmup" else 100
            (output / (phase + ".out")).write_text(
                f"total time: 30.0s\n[TIME] commits={commits} aborts=10 exec_us=1000 wasted_us=500\n")
            (output / (phase + ".err")).write_text("")

        self.args.rows = [100]
        case = bench.plan(self.args)[0]
        with patch.object(bench, "Server", FakeServer), patch.object(bench, "logged", side_effect=log), patch.object(bench, "preload") as preload:
            bench.run_case(self.args, case, results, {"Vanilla": self.args.loader_prefix})
            preload.assert_called_once_with(FakeServer.instances[-1], 10000000)
        summary = json.loads(next(results.glob("*/summary.json")).read_text())
        self.assertEqual(phases, ["warmup", "run"])
        self.assertEqual(summary["commits"], 100)
        self.assertEqual(summary["row_lock_wait_ms"], 10)
        self.assertEqual(summary["commit_wait_ms"], 3)
        self.assertEqual(list((self.root / "runs").iterdir()), [])
        self.assertEqual((bench.dataset_path(self.args, "sysbench", 10000000) / "data/ibdata1").read_text(), "clean snapshot")


if __name__ == "__main__":
    unittest.main()
