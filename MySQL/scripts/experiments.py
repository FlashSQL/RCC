#!/usr/bin/env python3
"""MySQL Fig. 4/5/6 preparation and runs. Only owned scratch servers are stopped."""
import argparse
import contextlib
import csv
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BASE = Path(__file__).resolve().parent.parent
ENGINES = ("Vanilla", "Bamboo", "Rebirth", "RCC", "RCC-C")
THREADS = "1,16,32,64,96,128"
ROWS = "10M,100K,10K,1K,0.1K"
DEFAULT_ENGINES = {
    "fig4": "Vanilla,Bamboo,Rebirth,RCC",
    "fig5": "Vanilla,Bamboo,Rebirth,RCC,RCC-C",
    "fig6": "Bamboo,Rebirth,RCC",
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, data):
    Path(path).write_text(json.dumps(data, indent=2, default=str) + "\n")


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sizes(value):
    result = []
    for token in re.split(r"[,\s]+", value.strip()):
        match = re.fullmatch(r"(\d+(?:\.\d+)?)([KkMm]?)", token)
        require(match, f"Invalid size: {token}")
        number = float(match[1]) * {"": 1, "K": 1000, "M": 1000000}[match[2].upper()]
        require(number.is_integer() and number >= 1, f"Invalid size: {token}")
        result.append(int(number))
    require(result and len(set(result)) == len(result), "List is empty or contains duplicate values")
    return result


def engine_prefix(engine, args):
    if engine == "Vanilla":
        return args.loader_prefix
    output = subprocess.check_output([
        "bash", "-c", 'source "$1"; mysql_build_config "$2" || exit; printf "%s" "$INSTALL_PATH"',
        "build-config", str(BASE / "scripts/build_config.sh"), engine,
    ], text=True)
    return Path(output)


def client_env(args):
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(args.loader_prefix / "lib") + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    env["LUA_PATH"] = str(BASE / "sysbench/?.lua") + ";;"
    env.update(SB_SORT="1", SB_ABUF="1", SB_EXPBO="0", SB_PENALTY_US="5000",
               SB_BACKOFF_US="10000", SB_ABUF_SIZE="0", SB_DUMPATT="0",
               TPCC_BACKOFF_ON="1", TPCC_BACKOFF_BASE_US="2000",
               TPCC_BACKOFF_CAP_US="200000", TPCC_BACKOFF_FACTOR="2",
               TPCC_BACKOFF_JITTER="1", TPCC_BACKOFF_SPIN_US="0")
    return env


def logged(command, directory, name, args, seconds=180):
    command = [str(x) for x in command]
    write_json(directory / (name + ".command.json"), command)
    with (directory / (name + ".out")).open("w") as out, (directory / (name + ".err")).open("w") as err:
        subprocess.run(command, stdout=out, stderr=err, check=True,
                       env=client_env(args), timeout=seconds)


def cnf_page_size(args):
    text = args.config.read_text()
    values = re.findall(r"^\s*innodb_page_size\s*=\s*([^#\s]+)", text, re.M)
    return values[-1] if values else "16KB"


class Server:
    def __init__(self, args, prefix, data, redo, output):
        self.args, self.prefix = args, prefix
        self.data, self.redo, self.output = data, redo, output
        self.process = None
        self.console = None
        self.socket_dir = Path(tempfile.mkdtemp(prefix="rcc-mysql-"))
        self.socket = self.socket_dir / "mysql.sock"
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.cnf = output / "my.cnf"
        self.cnf.write_text(args.config.read_text() + "\n[mysqld]\n" +
            f"basedir={prefix}\ndatadir={data}\ninnodb_log_group_home_dir={redo}\n" +
            f"socket={self.socket}\nport={self.port}\npid-file={self.socket_dir / 'mysql.pid'}\n" +
            f"log-error={output / 'server-error.log'}\nbind-address=127.0.0.1\n" +
            "loose-mysqlx=OFF\nskip-log-bin\ntransaction-isolation=SERIALIZABLE\n" +
            "innodb_buffer_pool_dump_at_shutdown=OFF\ninnodb_buffer_pool_load_at_startup=OFF\n")

    def command(self):
        command = [str(self.prefix / "bin/mysqld"), f"--defaults-file={self.cnf}"]
        if os.geteuid() == 0:
            command.append("--user=root")
        return command

    def mysql(self, sql, database=None, timeout=600):
        command = [str(self.args.loader_prefix / "bin/mysql"), "--no-defaults", "-uroot",
                   "--protocol=SOCKET", f"--socket={self.socket}", "--connect-timeout=5", "-N", "-B"]
        if database:
            command.append(database)
        # SQL files and generated preload batches are sent over stdin, without shell expansion.
        try:
            return subprocess.check_output(command, input=sql, text=True, stderr=subprocess.PIPE,
                                           env=client_env(self.args), timeout=timeout)
        except subprocess.CalledProcessError as error:
            (self.output / "mysql-error.log").write_text(error.stderr)
            raise RuntimeError(f"MySQL query failed: {error.stderr.strip()}; see {self.output}") from error

    def initialize(self):
        self.data.mkdir(); self.redo.mkdir()
        logged(self.command() + ["--initialize-insecure"], self.output, "initialize", self.args, 1800)

    def start(self):
        command = self.command()
        if self.args.numa == "interleave":
            command = ["numactl", "--interleave=all"] + command
        write_json(self.output / "server.command.json", command)
        self.console = (self.output / "server-console.log").open("w")
        self.process = subprocess.Popen(command, stdout=self.console, stderr=subprocess.STDOUT,
                                        env=client_env(self.args))
        for _ in range(180):
            require(self.process.poll() is None, f"Server exited; see {self.output / 'server-error.log'}")
            try:
                if self.mysql("SELECT 1").strip() == "1":
                    break
            except (subprocess.SubprocessError, RuntimeError):
                time.sleep(1)
        else:
            raise RuntimeError(f"Server readiness timeout: {self.output}")
        self.mysql("SET GLOBAL transaction_isolation='SERIALIZABLE';")
        require(self.mysql("SELECT @@global.transaction_isolation, @@session.transaction_isolation").strip()
                == "SERIALIZABLE\tSERIALIZABLE", "Serializable isolation was not applied")
        runtime = self.mysql("SHOW GLOBAL VARIABLES WHERE Variable_name IN "
                            "('transaction_isolation','innodb_buffer_pool_size','innodb_page_size',"
                            "'innodb_page_cleaners','innodb_flush_log_at_trx_commit','socket','datadir');")
        (self.output / "runtime.tsv").write_text(runtime)
        proc_status = Path(f"/proc/{self.process.pid}/status")
        if proc_status.exists():
            (self.output / "placement.txt").write_text("\n".join(
                line for line in proc_status.read_text().splitlines()
                if line.startswith(("Cpus_allowed_list:", "Mems_allowed_list:"))) + "\n")

    def stop(self, clean=True):
        try:
            if self.process is not None:
                if self.process.poll() is not None:
                    if clean:
                        raise RuntimeError(f"Server exited unexpectedly: {self.output}")
                else:
                    if clean:
                        self.mysql("SET GLOBAL innodb_fast_shutdown=0;")
                    command = [str(self.args.loader_prefix / "bin/mysqladmin"), "--no-defaults",
                               "-uroot", "--protocol=SOCKET", f"--socket={self.socket}", "shutdown"]
                    subprocess.run(command, check=clean, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, env=client_env(self.args), timeout=180)
                    self.process.wait(timeout=180)
                    if clean:
                        require(self.process.returncode == 0, "Server shutdown failed")
        finally:
            if self.process is not None and self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    self.process.kill(); self.process.wait()
            self.process = None
            if self.console:
                self.console.close()
            shutil.rmtree(self.socket_dir, ignore_errors=True)


@contextlib.contextmanager
def running(server, initialize=False):
    try:
        if initialize:
            server.initialize()
        server.start()
        yield server
        server.stop()
    finally:
        if server.process is not None or server.socket_dir.exists():
            server.stop(clean=False)


def dataset_spec(args, kind, rows):
    spec = {"format": 1, "kind": kind, "page_size": cnf_page_size(args)}
    if kind == "sysbench":
        spec.update(rows=rows, partitions=args.partitions,
                    schema_sha256=digest(BASE / "sysbench/oltp_common.lua"))
    else:
        spec.update(warehouses=1, items=100000,
                    schema_sha256=digest(BASE / "tpcc-mysql/create_table.sql"))
    return spec


def dataset_path(args, kind, rows):
    name = f"sysbench-{rows}-p{args.partitions}" if kind == "sysbench" else "tpcc-w1-i100000"
    return args.work_dir / "datasets" / name


def check_dataset(args, kind, rows):
    path = dataset_path(args, kind, rows)
    manifest = path / "dataset.json"
    require(manifest.is_file(), f"Dataset missing: {path}; run the loader or use --prepare")
    require(json.loads(manifest.read_text()) == dataset_spec(args, kind, rows),
            f"Dataset configuration mismatch: {path}; use another --work-dir and load again")
    require((path / "data/ibdata1").is_file() and (path / "redo").is_dir(), f"Incomplete dataset: {path}")
    return path


def sysbench_command(args, server, rows, threads, seconds, action="run"):
    workload = "oltp_write_only.lua" if action == "prepare" else "ycsb_like.lua"
    command = [args.sysbench, str(BASE / "sysbench" / workload),
               f"--mysql-socket={server.socket}", "--mysql-user=root", "--mysql-password=",
               "--mysql-db=sbtest", "--tables=1", f"--table-size={rows}",
               "--point-selects=8", "--non-index-updates=8", "--rand-type=zipfian",
               "--rand-zipfian-exp=0.9", f"--threads={threads}", f"--time={seconds}",
               "--events=0", "--report-interval=1", "--create_table_options=DEFAULT CHARSET=latin1", action]
    return command


def tpcc_command(server, threads, seconds):
    # The imported main.c uses seq_init(1,1,0,0,0): NewOrder/Payment only.
    # -0 ... -4 are response-time limits, NOT transaction mix options.
    return [BASE / "tpcc-mysql/tpcc_start", "-S", server.socket, "-d", "tpcc", "-u", "root",
            "-p", "", "-w", "1", "-I", "100k", "-c", str(threads), "-r", "0",
            "-l", str(seconds), "-i", "1", "-a", "0"]


def check_tpcc_consistency(server):
    checks = (BASE / "tpcc-mysql/consistency_check.sql").read_text()
    # SERIALIZABLE reads must belong to an explicit transaction on every engine.
    result = server.mysql("START TRANSACTION;\n" + checks + "\nCOMMIT;", "tpcc")
    (server.output / "consistency.tsv").write_text(result)
    rows = [line.split("\t") for line in result.splitlines() if line.strip()]
    require(len(rows) == 7 and all(len(row) >= 2 and row[0].strip() == "OK"
                                  and row[1] == "0" for row in rows),
            f"TPC-C consistency check failed; see {server.output / 'consistency.tsv'}")


def prepare_dataset(args, kind, rows):
    destination = dataset_path(args, kind, rows)
    if destination.exists():
        check_dataset(args, kind, rows)
        print(f"[reuse] {destination}", flush=True)
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix=".loading-", dir=destination.parent))
    output = temp / "load-logs"; output.mkdir()
    server = Server(args, args.loader_prefix, temp / "data", temp / "redo", output)
    print(f"[load] {kind} rows={rows if kind == 'sysbench' else '1 warehouse'} -> {destination}", flush=True)
    with running(server, initialize=True):
        if kind == "sysbench":
            server.mysql("CREATE DATABASE sbtest;")
            logged(sysbench_command(args, server, rows, 1, 0, "prepare"), output, "prepare", args, 7200)
            if args.partitions:
                server.mysql(f"ALTER TABLE sbtest.sbtest1 PARTITION BY HASH(id) PARTITIONS {args.partitions};",
                             timeout=7200)
            server.mysql("ANALYZE TABLE sbtest.sbtest1;")
            count = server.mysql("START TRANSACTION; SELECT COUNT(*) FROM sbtest.sbtest1; COMMIT;").strip()
            require(count == str(rows), f"Loaded row count mismatch: expected {rows}, got {count}")
        else:
            server.mysql("CREATE DATABASE tpcc;")
            server.mysql((BASE / "tpcc-mysql/create_table.sql").read_text(), "tpcc", timeout=7200)
            server.mysql("CREATE USER 'root'@'127.0.0.1' IDENTIFIED BY ''; "
                         "GRANT ALL ON tpcc.* TO 'root'@'127.0.0.1';")
            logged([BASE / "tpcc-mysql/tpcc_load", "-h", "127.0.0.1", "-P", str(server.port),
                    "-d", "tpcc", "-u", "root", "-p", "", "-w", "1"], output, "prepare", args, 7200)
            server.mysql((BASE / "tpcc-mysql/add_fkey_idx.sql").read_text(), "tpcc", timeout=7200)
            expected = {"warehouse": 1, "district": 10, "customer": 30000,
                        "history": 30000, "orders": 30000, "new_orders": 9000,
                        "item": 100000, "stock": 100000}
            counts = {table: int(server.mysql(f"SELECT COUNT(*) FROM tpcc.{table};").strip())
                      for table in (*expected, "order_line")}
            write_json(output / "loaded-counts.json", counts)
            require(all(counts[table] == n for table, n in expected.items())
                    and 150000 <= counts["order_line"] <= 450000,
                    f"Incomplete TPC-C load: {counts}")
            server.mysql("ANALYZE TABLE tpcc.warehouse,tpcc.district,tpcc.customer,tpcc.history,"
                         "tpcc.orders,tpcc.new_orders,tpcc.order_line,tpcc.item,tpcc.stock;")
            check_tpcc_consistency(server)
    # A snapshot becomes available only after a clean, full shutdown.
    write_json(temp / "dataset.json", dataset_spec(args, kind, rows))
    temp.rename(destination)


def preload(server, rows):
    statements = ["SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE;"]
    for lo in range(0, rows, 1000):
        statements.extend(["START TRANSACTION;",
            "SELECT COUNT(*),COALESCE(SUM(LENGTH(c)),0),COALESCE(SUM(LENGTH(pad)),0) "
            f"FROM sbtest.sbtest1 FORCE INDEX(PRIMARY) WHERE id>{lo} AND id<={min(lo + 1000, rows)};",
            "COMMIT;"])
    output = server.mysql("\n".join(statements))
    (server.output / "preload.tsv").write_text(output)
    require(sum(int(line.split()[0]) for line in output.splitlines()) == rows, "Preload row count mismatch")


def parse_metrics(kind, text, seconds, threads):
    if kind == "sysbench":
        records = re.findall(r"\[TIME\] commits=(\d+) aborts=(\d+) exec_us=(\d+) wasted_us=(\d+)", text)
        require(len(records) == threads, f"Expected {threads} sysbench worker summaries, got {len(records)}")
        commits, aborts, success_ms, waste_ms = [sum(int(row[i]) for row in records) for i in range(4)]
        success_ms /= 1000; waste_ms /= 1000
        failures = 0
        duration = re.search(r"(?:total time|time elapsed):\s*([\d.]+)s", text)
        require(duration, "Missing sysbench total time")
        seconds = float(duration[1])
    else:
        completed = re.search(r"THREADS_COMPLETED:\s*(\d+)", text)
        require(completed and int(completed[1]) == threads,
                f"Expected {threads} completed TPC-C workers; rebuild the client if the marker is missing")
        section = re.search(r"<Raw Results2\(sum ver\.\)>(.*?)(?=\n<|\Z)", text, re.S)
        require(section, "Missing TPC-C per-thread totals")
        records = re.findall(r"\[(\d)\] sc:(\d+)\s+lt:(\d+)\s+rt:(\d+)\s+random_rt:(\d+)\s+fl:(\d+)", section[1])
        require(len(records) == 5 and {r[0] for r in records} == set("01234"), "Incomplete TPC-C totals")
        require(all(not any(map(int, row[1:])) for row in records if int(row[0]) > 1),
                "TPC-C ran transactions other than NewOrder/Payment")
        commits = sum(int(row[1]) + int(row[2]) for row in records)
        failures = sum(int(row[5]) for row in records)
        aborts = sum(int(row[3]) for row in records) + failures
        require(all(int(row[4]) == 0 for row in records), "Unexpected injected TPC-C aborts")
        totals = re.search(r"TOTAL_COMMIT:\s*([\d.]+)\s+TOTAL_WASTE:\s*([\d.]+)", text)
        require(totals, "Missing TPC-C timing totals")
        success_ms, waste_ms = map(float, totals.groups())
        duration = re.search(r"\bin (\d+) sec\.", text)
        require(duration and int(duration[1]) == seconds, "TPC-C measurement duration mismatch")
    require(commits > 0 and seconds > 0, "No completed transactions")
    require(failures == 0, f"TPC-C retry limit exhausted ({failures} failed transactions)")
    return dict(commits=commits, aborts=aborts, failures=failures, measured_seconds=seconds,
                tps=commits / seconds, abort_rate=aborts / (commits + aborts),
                successful_attempt_ms=success_ms, aborted_attempt_ms=waste_ms)


def status_values(text):
    return {parts[0].lower(): int(parts[1]) for line in text.splitlines()
            if len(parts := line.split()) == 2 and parts[1].isdigit()}


def run_case(args, case, results, prefixes):
    kind = case["workload"]
    snapshot = check_dataset(args, kind, case["table_rows"])
    label = f"{case['engine']}_rows{case['rows']}_t{case['threads']}_r{case['repeat']}"
    output = results / label; output.mkdir()
    temp = Path(tempfile.mkdtemp(prefix=label + "-", dir=args.work_dir / "runs"))
    for name in ("data", "redo"):
        shutil.copytree(snapshot / name, temp / name)
    server = Server(args, prefixes[case["engine"]], temp / "data", temp / "redo", output)
    metadata = {**case, "isolation": "SERIALIZABLE", "warmup_seconds": args.warmup,
                "runtime_seconds": args.runtime, "zipf": 0.9 if kind == "sysbench" else None,
                "skip_consistency_check": args.skip_consistency_check,
                "order": "asc" if kind == "sysbench" else None,
                "read_ops": 8 if kind == "sysbench" else None, "write_ops": 8 if kind == "sysbench" else None,
                "tpcc_mix": "NewOrder:Payment=1:1" if kind == "tpcc" else None,
                "warehouses": 1 if kind == "tpcc" else None,
                "items": 100000 if kind == "tpcc" else None,
                "partitions": args.partitions if kind == "sysbench" else None,
                "preload_rows": case["table_rows"] if kind == "sysbench" else None,
                "snapshot": str(snapshot), "numa": args.numa,
                "client_environment": {key: value for key, value in client_env(args).items()
                                       if key.startswith(("SB_", "TPCC_"))},
                "status_window": "separate measurement client, including connect/drain; warmup excluded"}
    write_json(output / "run.json", metadata)
    print(f"[run] {label} warmup={args.warmup}s runtime={args.runtime}s", flush=True)
    with running(server):
        if kind == "sysbench":
            preload(server, case["table_rows"])
        else:
            server.mysql("ANALYZE TABLE tpcc.warehouse,tpcc.district,tpcc.customer,tpcc.history,"
                         "tpcc.orders,tpcc.new_orders,tpcc.order_line,tpcc.item,tpcc.stock;")
        for phase, seconds in (("warmup", args.warmup), ("run", args.runtime)):
            if not seconds:
                continue
            command = (sysbench_command(args, server, case["rows"], case["threads"], seconds)
                       if kind == "sysbench" else tpcc_command(server, case["threads"], seconds))
            if phase == "run":
                before = server.mysql("SHOW GLOBAL STATUS;")
                (output / "status_before.tsv").write_text(before)
            logged(command, output, phase, args, seconds + 180)
            text = (output / (phase + ".out")).read_text()
            parsed = parse_metrics(kind, text, seconds, case["threads"])
            require(not re.search(r"FATAL|SQLERR|mysql_stmt_prepare.*failed", (output / (phase + ".err")).read_text(), re.I),
                    f"Client error; see {output / (phase + '.err')}")
        after = server.mysql("SHOW GLOBAL STATUS;")
        (output / "status_after.tsv").write_text(after)
        before_values, after_values = status_values(before), status_values(after)
        for metric, counter in (("row_lock_wait_ms", "innodb_row_lock_time"),
                                ("commit_wait_ms", "innodb_row_commit_time")):
            parsed[metric] = (after_values[counter] - before_values[counter]
                              if counter in before_values and counter in after_values else None)
        record = {**case, **parsed, "consistency_check": "not_applicable"}
    if kind == "tpcc":
        if args.skip_consistency_check:
            record["consistency_check"] = "skipped"
            print(f"[WARN] {label}: TPC-C consistency check skipped (--skip-consistency-check)", flush=True)
        else:
            # Validate persisted rows with Vanilla after the measured engine has
            # completed a full shutdown; validation is outside all timing windows.
            check_output = output / "consistency-check"; check_output.mkdir()
            checker = Server(args, args.loader_prefix, temp / "data", temp / "redo", check_output)
            with running(checker):
                check_tpcc_consistency(checker)
            shutil.copy2(check_output / "consistency.tsv", output / "consistency.tsv")
            record["consistency_check"] = "passed"
    metadata["consistency_check"] = record["consistency_check"]
    write_json(output / "run.json", metadata)
    write_json(output / "summary.json", record)
    summary = results / "summary.csv"
    exists = summary.exists()
    with summary.open("a", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(record))
        if not exists:
            writer.writeheader()
        writer.writerow(record)
    shutil.rmtree(temp)
    print(f"[done] {label}: {record['tps']:.2f} TPS, abort={record['abort_rate']:.4%}", flush=True)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiment", choices=("fig4", "fig5", "fig6", "load-sysbench", "load-tpcc"))
    parser.add_argument("--engines", help="Comma-separated engines: " + ",".join(ENGINES))
    parser.add_argument("--threads", help="Comma-separated thread counts; Fig4/5: " + THREADS + "; Fig6: 64")
    parser.add_argument("--rows", help="Sysbench rows; Fig4: 10M; Fig6/load: " + ROWS)
    parser.add_argument("--row-mode", choices=("table", "range"), default="range",
                        help="range: vary key range in a 10M table (default); table: vary actual rows")
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--runtime", type=int, default=30)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--paper", action="store_true", help="Three repetitions, like DBx1000 --paper")
    parser.add_argument("--prepare", action="store_true", help="Load missing Vanilla snapshots before experiments")
    parser.add_argument("--skip-consistency-check", action="store_true",
                        help="Skip post-run TPC-C consistency checks and mark results as skipped; dataset loading remains checked")
    parser.add_argument("--dry-run", action="store_true", help="Print plan only; no writes, builds or server actions")
    parser.add_argument("--work-dir", type=Path, default=BASE / "benchmark-data")
    parser.add_argument("--results-dir", type=Path)
    parser.add_argument("--config", type=Path, default=BASE / "my.cnf")
    parser.add_argument("--loader-prefix", type=Path, default=BASE / "Vanilla/mysql-8.4.5-install",
                        help="Vanilla installation used for loading, client tools/libraries and Vanilla runs")
    candidates = (BASE / "tools/sysbench/src/sysbench", Path.home() / "sysbench/src/sysbench")
    parser.add_argument("--sysbench", default=next((str(p) for p in candidates if p.is_file()), "sysbench"))
    parser.add_argument("--partitions", type=int, default=128, help="Sysbench HASH(id) partitions; 0 disables")
    parser.add_argument("--numa", choices=("interleave", "none"), default="interleave")
    args = parser.parse_args(argv)
    args.engines = re.split(r"[,\s]+", (args.engines or DEFAULT_ENGINES.get(args.experiment, "Vanilla")).strip())
    require(args.engines and len(set(args.engines)) == len(args.engines)
            and all(e in ENGINES for e in args.engines), "Invalid or duplicate engine name")
    args.threads = sizes(args.threads or ("64" if args.experiment == "fig6" else THREADS))
    args.rows = sizes(args.rows or ("10M" if args.experiment == "fig4" else ROWS))
    require(all(n >= 16 for n in args.rows), "Sysbench needs at least 16 distinct keys")
    require(args.row_mode != "range" or max(args.rows) <= 10000000, "Key range exceeds the 10M dataset")
    require(args.warmup >= 0 and args.runtime > 0 and args.repeat in (1, 2, 3), "Invalid duration/repeat count")
    require(0 <= args.partitions <= 8192, "Invalid partition count")
    if args.paper:
        args.repeat = 3
    for name in ("work_dir", "loader_prefix", "config"):
        setattr(args, name, getattr(args, name).expanduser().resolve())
    if args.results_dir:
        args.results_dir = args.results_dir.expanduser().resolve()
    else:
        args.results_dir = BASE / "results" / args.experiment.capitalize()
    return args


def plan(args):
    kind = "tpcc" if args.experiment in ("fig5", "load-tpcc") else "sysbench"
    if args.experiment.startswith("load-"):
        rows = [0] if kind == "tpcc" else ([10000000] if args.row_mode == "range" else args.rows)
        return [dict(workload=kind, table_rows=r) for r in rows]
    return [dict(figure=args.experiment, workload=kind, engine=engine,
                 rows=rows, table_rows=(10000000 if args.row_mode == "range" and kind == "sysbench" else rows),
                 threads=threads, repeat=repeat)
            for engine in args.engines for rows in ([0] if kind == "tpcc" else args.rows)
            for threads in args.threads for repeat in range(1, args.repeat + 1)]


def check_tools(args, cases):
    for name in ("mysqld", "mysql", "mysqladmin"):
        require(os.access(args.loader_prefix / "bin" / name, os.X_OK), f"Missing Vanilla tool: {name}; build Vanilla first")
    if args.numa == "interleave":
        require(shutil.which("numactl"), "numactl missing; install it or select --numa none")
    prefixes = {engine: engine_prefix(engine, args) for engine in args.engines}
    for engine, prefix in prefixes.items():
        require(os.access(prefix / "bin/mysqld", os.X_OK), f"Missing {engine} binary: {prefix / 'bin/mysqld'}")
    if any(c["workload"] == "sysbench" for c in cases):
        require(shutil.which(args.sysbench), f"sysbench not found: {args.sysbench}")
        help_text = subprocess.check_output([args.sysbench, "--help"], env=client_env(args), text=True)
        require("rand-zipfian-exp" in help_text, "This workload needs sysbench with Zipfian support (1.1)")
    if any(c["workload"] == "tpcc" for c in cases):
        for name in ("tpcc_load", "tpcc_start"):
            require(os.access(BASE / "tpcc-mysql" / name, os.X_OK), "Build TPC-C first: bash scripts/build_clients.sh")
    return prefixes


def main(argv=None):
    args = parse_args(argv)
    cases = plan(args)
    print(f"[plan] {args.experiment}: {len(cases)} cases; SERIALIZABLE; warmup={args.warmup}s runtime={args.runtime}s; "
          f"sysbench zipf=0.9 asc 8R+8W; row-mode={args.row_mode}; TPC-C w=1 NO:PM=1:1", flush=True)
    if args.dry_run:
        for case in cases:
            print(json.dumps(case, separators=(",", ":")))
        return
    require(args.config.is_file(), f"Missing config: {args.config}")
    prefixes = check_tools(args, cases)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    with (args.work_dir / ".experiment.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        datasets = sorted({(c["workload"], c["table_rows"]) for c in cases}, reverse=True)
        for kind, rows in datasets:
            if args.prepare or args.experiment.startswith("load-"):
                prepare_dataset(args, kind, rows)
            else:
                check_dataset(args, kind, rows)
        if args.experiment.startswith("load-"):
            return
        stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S") + f"_{os.getpid()}"
        results = args.results_dir / stamp
        results.mkdir(parents=True)
        (args.work_dir / "runs").mkdir(exist_ok=True)
        write_json(results / "plan.json", {"settings": vars(args), "cases": cases})
        versions = {engine: {"prefix": str(prefix), "mysqld_sha256": digest(prefix / "bin/mysqld")}
                    for engine, prefix in prefixes.items()}
        versions["workloads"] = {str(p.relative_to(BASE)): digest(p) for p in
            [BASE / "sysbench/ycsb_like.lua", BASE / "sysbench/oltp_common.lua", BASE / "tpcc-mysql/src/main.c"]}
        clients = ([Path(shutil.which(args.sysbench))] if cases[0]["workload"] == "sysbench"
                   else [BASE / "tpcc-mysql/tpcc_start", BASE / "tpcc-mysql/tpcc_load"])
        versions["clients"] = {str(p): digest(p) for p in clients}
        write_json(results / "versions.json", versions)
        print(f"[results] {results}", flush=True)
        for case in cases:
            run_case(args, case, results, prefixes)
        print(f"[complete] {results / 'summary.csv'}")


if __name__ == "__main__":
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except KeyboardInterrupt:
        print("[ERROR] Interrupted; owned server cleanup requested", file=sys.stderr)
        sys.exit(130)
    except (RuntimeError, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        sys.exit(1)
