#!/usr/bin/env python3
"""Exercise RCC commit-time detection against a disposable local RCC server.

Requires psycopg2, a C compiler, installed RCC server headers, and a superuser.
The server must run as this OS user so it can load the temporary test module.
Connection parameters use --dsn or the usual PGHOST/PGPORT/PGUSER/PGDATABASE.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time

import psycopg2


def execute(connection, statement, parameters=None):
    with connection.cursor() as cursor:
        cursor.execute(statement, parameters)
        return cursor.fetchall() if cursor.description else []


def outcome(connection, statement="COMMIT", barrier=None):
    if barrier:
        barrier.wait(timeout=5)
    try:
        execute(connection, statement)
        return "00000", ""
    except psycopg2.Error as error:
        return error.pgcode, error.diag.message_primary


class Suite:
    def __init__(self, dsn, module):
        self.dsn = dsn
        self.module = str(module)
        self.control = psycopg2.connect(dsn)
        self.control.autocommit = True
        assert execute(self.control, "SHOW rcc_bg_detect")[0][0] == "off"

    @contextmanager
    def sessions(self, count):
        connections = []
        try:
            for _ in range(count):
                connection = psycopg2.connect(self.dsn)
                connections.append(connection)
                connection.autocommit = True
                execute(connection, "SET deadlock_timeout = '10s'")
                execute(connection, "SET statement_timeout = '3s'")
                execute(connection, "SET idle_in_transaction_session_timeout = '60s'")
                execute(connection, """
                    CREATE FUNCTION pg_temp.rcc_test_depend(xid) RETURNS void
                    AS %s, 'rcc_test_depend' LANGUAGE C STRICT
                """, (self.module,))
            yield connections
        finally:
            for connection in connections:
                connection.close()

    def begin(self, connections, same_partition=False):
        xids = []
        for connection in connections:
            if same_partition and xids:
                while True:
                    xid = int(execute(self.control,
                                      "SELECT pg_current_xact_id()::text")[0][0])
                    if (xid + 1) % 256 == xids[0] % 256:
                        break
            execute(connection, "BEGIN")
            xids.append(int(execute(connection,
                                    "SELECT pg_current_xact_id()::text")[0][0]))
        if same_partition:
            assert len({xid % 256 for xid in xids}) == 1, xids
        return xids

    @staticmethod
    def depend(connection, xid):
        execute(connection, "SELECT pg_temp.rcc_test_depend(%s::xid)", (str(xid),))

    def wait_for_lock(self, connections, event="transactionid"):
        pids = [connection.get_backend_pid() for connection in connections]
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            count = execute(self.control, """
                SELECT count(*) FROM pg_stat_activity
                WHERE pid = ANY(%s) AND wait_event_type = 'Lock'
                  AND wait_event = %s
            """, (pids, event))[0][0]
            if count == len(pids):
                return
            time.sleep(0.005)
        raise AssertionError(f"Backends did not enter dependency waits: {pids}")

    def no_dependencies(self):
        with self.sessions(1) as connections:
            self.begin(connections)
            assert outcome(connections[0])[0] == "00000"

    def cycle(self, size, same_partition=False):
        with self.sessions(size) as connections:
            xids = self.begin(connections, same_partition)
            for i, connection in enumerate(connections):
                self.depend(connection, xids[(i + 1) % size])
            code, message = outcome(connections[0])
            assert code == "40001", (code, message)
            assert "read/write dependencies" in message, message
            assert "bgdd" not in message, message

    def acyclic_diamond(self):
        with self.sessions(5) as connections:
            xids = self.begin(connections)
            # 0 -> 4; 1 -> 0; 2 -> 0; 3 -> {1, 2}. Reverse DFS revisits 3.
            for source, target in [(0, 4), (1, 0), (2, 0), (3, 1), (3, 2)]:
                self.depend(connections[source], xids[target])
            with ThreadPoolExecutor(max_workers=4) as pool:
                pending = [pool.submit(outcome, c) for c in connections[:4]]
                self.wait_for_lock(connections[:4])
                assert outcome(connections[4])[0] == "00000"
                results = [future.result(timeout=5) for future in pending]
                assert all(code == "00000" for code, _ in results), results

    def removed_dependent(self):
        with self.sessions(3) as connections:
            xids = self.begin(connections)
            self.depend(connections[1], xids[0])
            self.depend(connections[0], xids[2])
            execute(connections[1], "ROLLBACK")
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(outcome, connections[0])
                self.wait_for_lock(connections[:1])
                assert outcome(connections[2])[0] == "00000"
                assert pending.result(timeout=5)[0] == "00000"

    def cascade_abort(self):
        with self.sessions(2) as connections:
            xids = self.begin(connections)
            self.depend(connections[0], xids[1])
            execute(connections[1], "ROLLBACK")
            code, message = outcome(connections[0])
            assert code == "40001", (code, message)
            assert "bgdd" not in message, message

    def concurrent_cycles(self):
        with self.sessions(3) as connections, ThreadPoolExecutor(max_workers=3) as pool:
            for _ in range(40):
                xids = self.begin(connections)
                for i, connection in enumerate(connections):
                    self.depend(connection, xids[(i + 1) % 3])
                barrier = threading.Barrier(3)
                pending = [pool.submit(outcome, c, "COMMIT", barrier)
                           for c in connections]
                results = [future.result(timeout=5) for future in pending]
                assert all(code == "40001" for code, _ in results), results
                assert any("read/write dependencies" in message
                           for _, message in results), results
                assert all("bgdd" not in message for _, message in results), results
                for connection in connections:
                    execute(connection, "ROLLBACK")

    def native_deadlock(self):
        with self.sessions(2) as connections, ThreadPoolExecutor(max_workers=2) as pool:
            self.begin(connections)
            for i, connection in enumerate(connections):
                execute(connection, "SET LOCAL deadlock_timeout = '100ms'")
                execute(connection, "SELECT pg_advisory_xact_lock(%s)", (739010 + i,))
            barrier = threading.Barrier(2)
            pending = [pool.submit(outcome, c,
                                   f"SELECT pg_advisory_xact_lock({739011 - i})",
                                   barrier) for i, c in enumerate(connections)]
            results = [future.result(timeout=5) for future in pending]
            assert sorted(code for code, _ in results) == ["00000", "40P01"], results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsn", default="")
    parser.add_argument("--pg-config", required=True)
    args = parser.parse_args()

    def config(flag):
        return subprocess.check_output([args.pg_config, flag], text=True).strip()

    with tempfile.TemporaryDirectory(prefix="rcc-deadlock-test-") as directory:
        module = Path(directory) / "rcc_deadlock_helpers.so"
        subprocess.run(shlex.split(config("--cc")) + [
            "-shared", "-fPIC", "-O2", "-DRCC",
            "-I" + config("--includedir-server"), "-I" + config("--includedir"),
            str(Path(__file__).with_name("rcc_deadlock_helpers.c")), "-o", str(module),
        ], check=True)
        suite = Suite(args.dsn, module)
        try:
            cases = [
                ("no dependencies", suite.no_dependencies),
                ("two-node cycle", lambda: suite.cycle(2)),
                ("three-node cycle", lambda: suite.cycle(3)),
                ("64-node cycle", lambda: suite.cycle(64)),
                ("same-partition cycle", lambda: suite.cycle(3, True)),
                ("acyclic diamond with commit waits", suite.acyclic_diamond),
                ("removed dependent", suite.removed_dependent),
                ("dependency abort cascades", suite.cascade_abort),
                ("40 concurrent cycle/cleanup rounds", suite.concurrent_cycles),
                ("native heavyweight deadlock fallback", suite.native_deadlock),
            ]
            for name, test in cases:
                test()
                print(f"PASS: {name}", flush=True)
            print(f"PASS: {len(cases)} cases", flush=True)
        finally:
            suite.control.close()


if __name__ == "__main__":
    main()
