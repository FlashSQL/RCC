#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import random
import time
from dataclasses import dataclass
from multiprocessing import Process, Queue
from typing import Dict, List, Tuple, Optional

# =========================
# CONFIG (edit here only)
# =========================
CONFIG = {
    "mysql": {
        "host": "127.0.0.1",
        "port": 3306,
        "user": "root",
        "passwd": "",
        "db": "occbench",
    },

    "N": 12_800_000,
    "warmup_sec": 50,
    "measure_sec": 50,
    "seed": 42,

    "H": [128],
    "ro_ratio_list": [0.3],
    "worker_list": [1,8,16,32,64,128],
    
    "worker_list2": [128],
    "ro_ratio_list2": [0.5,0.4,0.3,0.2,0.1,0.0],
    
    "p_hot_list": [0.5],

    "ro_read_keys": 16,
    "rmw_write_keys": 16,

    # Session
    "lock_wait_timeout": 1,

    # commit_tid allocation
    "tid_block": 2000,
}

# ---- MySQL driver: prefer mysqlclient (MySQLdb), fallback to PyMySQL ----
try:
    import MySQLdb as mysql
except Exception:
    import pymysql as mysql  # type: ignore

# MySQL error codes (for classification)
ER_LOCK_WAIT_TIMEOUT = 1205
ER_LOCK_DEADLOCK = 1213
ER_LOCK_NOWAIT = 3572  # MySQL 8 NOWAIT (won't happen here unless you use NOWAIT elsewhere)


# =========================
# Counters
# =========================
@dataclass
class Counters:
    commit: int = 0
    abort: int = 0

    ro_txn: int = 0
    rmw_txn: int = 0

    abort_lock: int = 0
    abort_validate: int = 0
    abort_deadlock: int = 0
    abort_timeout: int = 0
    abort_other: int = 0

    # ---- phase timing (ms) ----
    t_begin_ms: float = 0.0
    t_read_ms: float = 0.0
    t_tidalloc_ms: float = 0.0
    t_install_ms: float = 0.0
    t_commit_ms: float = 0.0
    t_rollback_ms: float = 0.0

    def add(self, other: "Counters"):
        for k in self.__dict__.keys():
            setattr(self, k, getattr(self, k) + getattr(other, k))


# =========================
# TID Allocator
# =========================
class TidAllocator:
    """
    Pre-allocate commit TIDs from global_tid using an autocommit connection.
    Avoid per-tx global_tid update bottleneck.
    """

    def __init__(self, tid_conn, block_size: int):
        self.tid_conn = tid_conn
        self.block_size = max(1, int(block_size))
        self.cur = tid_conn.cursor()
        self.next_tid = 0
        self.end_tid = -1

    def _alloc_block(self):
        self.cur.execute(
            "UPDATE global_tid SET tid = LAST_INSERT_ID(tid + %s) WHERE k=1",
            (self.block_size,),
        )
        self.cur.execute("SELECT LAST_INSERT_ID()")
        end_tid = int(self.cur.fetchone()[0])
        self.next_tid = end_tid - self.block_size + 1
        self.end_tid = end_tid

    def get(self) -> int:
        if self.next_tid > self.end_tid:
            self._alloc_block()
        tid = self.next_tid
        self.next_tid += 1
        return tid

    def close(self):
        try:
            self.cur.close()
        except Exception:
            pass


# =========================
# MySQL helpers
# =========================
def connect_mysql(autocommit: bool):
    cfg = CONFIG["mysql"]
    kwargs = dict(
        user=cfg["user"],
        passwd=cfg.get("passwd", ""),
        db=cfg["db"],
        charset="utf8mb4",
        autocommit=autocommit,
    )
    unix_socket = cfg.get("unix_socket")
    if unix_socket:
        kwargs["unix_socket"] = unix_socket
    else:
        kwargs["host"] = cfg.get("host", "127.0.0.1")
        kwargs["port"] = int(cfg.get("port", 3306))
    return mysql.connect(**kwargs)


def set_session(conn):
    cur = conn.cursor()
    cur.execute("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")
    cur.execute(
        "SET SESSION innodb_lock_wait_timeout = %s",
        (int(CONFIG["lock_wait_timeout"]),),
    )
    cur.close()


def _select_in(cur, cols: str, ids: List[int]) -> List[Tuple]:
    if not ids:
        return []
    ph = ",".join(["%s"] * len(ids))
    sql = f"SELECT {cols} FROM kv WHERE id IN ({ph})"
    cur.execute(sql, tuple(ids))
    return list(cur.fetchall())


def classify_abort(counters: Counters, e: Exception):
    counters.abort += 1
    code = e.args[0] if getattr(e, "args", None) else None

    if code == ER_LOCK_NOWAIT:
        counters.abort_lock += 1
    elif code == ER_LOCK_DEADLOCK:
        counters.abort_deadlock += 1
        counters.abort_lock += 1
    elif code == ER_LOCK_WAIT_TIMEOUT:
        counters.abort_timeout += 1
        counters.abort_lock += 1
    else:
        counters.abort_other += 1


def _rollback_timed(conn, counters: Counters):
    t0 = time.monotonic()
    conn.rollback()
    counters.t_rollback_ms += (time.monotonic() - t0) * 1000.0


def _commit_timed(conn, counters: Counters):
    t0 = time.monotonic()
    conn.commit()
    counters.t_commit_ms += (time.monotonic() - t0) * 1000.0


# =========================
# Workload helpers
# =========================
def pick_keys(rng: random.Random, k: int) -> List[int]:
    """
    Pick exactly k unique keys (best-effort without early stop).
    hotset [1..H] with prob p_hot, else coldset [H+1..N].

    NOTE:
      - If p_hot == 1.0 and H < k => impossible to pick k unique hot keys.
        This benchmark uses p_hot=0.5 by default, so it's fine.
    """
    N = int(CONFIG["N"])
    H = int(CONFIG["h_temp"])
    p_hot = float(CONFIG["p_hot"])

    if k <= 0:
        return []

    if p_hot >= 1.0 and H < k:
        raise ValueError(f"Cannot pick {k} unique hot keys when H={H} and p_hot=1.0")

    keys = set()
    # generous attempt budget; if still short, force-fill from cold side
    max_attempts = k * 500

    attempts = 0
    while len(keys) < k and attempts < max_attempts:
        attempts += 1
        if rng.random() < p_hot and H > 0:
            keys.add(rng.randint(1, H))
        else:
            if N <= H:
                keys.add(rng.randint(1, max(1, N)))
            else:
                keys.add(rng.randint(H + 1, N))

    # force fill from cold if needed (should be rare)
    while len(keys) < k:
        if N <= H:
            keys.add(rng.randint(1, max(1, N)))
        else:
            keys.add(rng.randint(H + 1, N))

    # return sorted(keys)
    return sorted(keys)


# =========================
# Read-only transaction (kept; usually disabled by ro_ratio=0)
# =========================
def txn_read_only(conn, rng: random.Random, counters: Counters) -> bool:
    counters.ro_txn += 1
    ro_k = int(CONFIG["ro_read_keys"])
    cur = conn.cursor()

    try:
        t0 = time.monotonic()
        cur.execute("START TRANSACTION")
        counters.t_begin_ms += (time.monotonic() - t0) * 1000.0

        read_ids = pick_keys(rng, ro_k)

        t0 = time.monotonic()
        rows = _select_in(cur, "id, tid", read_ids)
        counters.t_read_ms += (time.monotonic() - t0) * 1000.0
        read_tid = {int(r[0]): int(r[1]) for r in rows}

        # validate by re-reading
        if read_tid:
            ids2 = sorted(read_tid.keys())
            t0 = time.monotonic()
            now_rows = _select_in(cur, "id, tid", ids2)
            counters.t_read_ms += (time.monotonic() - t0) * 1000.0
            now_tid = {int(r[0]): int(r[1]) for r in now_rows}

            for rid, old in read_tid.items():
                if now_tid.get(rid) != old:
                    _rollback_timed(conn, counters)
                    counters.abort += 1
                    counters.abort_validate += 1
                    return False

        _commit_timed(conn, counters)
        counters.commit += 1
        return True

    except Exception as e:
        try:
            _rollback_timed(conn, counters)
        except Exception:
            pass
        classify_abort(counters, e)
        return False
    finally:
        cur.close()


# =========================
# Write-only OCC transaction (CAS install, 16 UPDATEs)
# =========================
def txn_write_cas(conn, rng: random.Random, counters: Counters, tid_alloc: TidAllocator) -> bool:
    counters.rmw_txn += 1
    w = max(1, int(CONFIG["rmw_write_keys"]))
    cur = conn.cursor()

    try:
        # BEGIN
        t0 = time.monotonic()
        cur.execute("START TRANSACTION")
        counters.t_begin_ms += (time.monotonic() - t0) * 1000.0

        # choose write-set (exactly w)
        write_ids_sorted = sorted(set(pick_keys(rng, w)))

        # Read phase (unlocked): fetch old tids for write-set
        t0 = time.monotonic()
        rows = _select_in(cur, "id, tid", write_ids_sorted)
        counters.t_read_ms += (time.monotonic() - t0) * 1000.0

        old_tid: Dict[int, int] = {int(r[0]): int(r[1]) for r in rows}
        if len(old_tid) != len(write_ids_sorted):
            _rollback_timed(conn, counters)
            counters.abort += 1
            counters.abort_other += 1
            return False

        # allocate commit_tid
        t0 = time.monotonic()
        commit_tid = tid_alloc.get()
        counters.t_tidalloc_ms += (time.monotonic() - t0) * 1000.0

        # CAS install: early abort on first mismatch (hot first)
        H = int(CONFIG["h_temp"])
        write_ids_sorted = sorted(write_ids_sorted, key=lambda x: (0 if x <= H else 1, x))

        t0 = time.monotonic()
        for wid in write_ids_sorted:
            cur.execute(
                "UPDATE kv SET v = v + 1, tid = %s WHERE id = %s AND tid = %s",
                (commit_tid, wid, old_tid[wid]),
            )
            if cur.rowcount != 1:
                counters.t_install_ms += (time.monotonic() - t0) * 1000.0
                _rollback_timed(conn, counters)
                counters.abort += 1
                counters.abort_validate += 1
                return False

        counters.t_install_ms += (time.monotonic() - t0) * 1000.0

        # COMMIT
        _commit_timed(conn, counters)
        counters.commit += 1
        return True

    except Exception as e:
        try:
            _rollback_timed(conn, counters)
        except Exception:
            pass
        classify_abort(counters, e)
        return False
    finally:
        cur.close()


# =========================
# Worker
# =========================
def worker_main(wid: int, p_hot: float, out_q: Queue, h: int, r: float):
    CONFIG["p_hot"] = float(p_hot)
    CONFIG["h_temp"] = int(h)

    rng = random.Random(int(CONFIG["seed"]) + wid * 1_000_003)

    conn = connect_mysql(autocommit=False)
    set_session(conn)

    tid_conn = connect_mysql(autocommit=True)
    tid_alloc = TidAllocator(tid_conn, block_size=int(CONFIG["tid_block"]))

    warmup_end = time.time() + int(CONFIG["warmup_sec"])
    measure_end = warmup_end + int(CONFIG["measure_sec"])

    # Warmup
    dummy = Counters()
    while time.time() < warmup_end:
        if rng.random() < float(r):
            txn_read_only(conn, rng, dummy)
        else:
            txn_write_cas(conn, rng, dummy, tid_alloc)

    # Measure
    counters = Counters()
    t0 = time.time()
    while time.time() < measure_end:
        if rng.random() < float(r):
            txn_read_only(conn, rng, counters)
        else:
            txn_write_cas(conn, rng, counters, tid_alloc)
    elapsed = max(1e-9, time.time() - t0)

    tid_alloc.close()
    try:
        tid_conn.close()
    except Exception:
        pass
    try:
        conn.close()
    except Exception:
        pass

    out_q.put((wid, elapsed, counters))


# =========================
# Run once
# =========================
def run_once(p_hot: float, worker: int, h: int, r: float) -> Tuple[float, Counters]:
    q = Queue()
    procs: List[Process] = []
    for wid in range(worker):
        p = Process(target=worker_main, args=(wid, float(p_hot), q, int(h), float(r)))
        p.start()
        procs.append(p)

    results = [q.get() for _ in range(worker)]
    for p in procs:
        p.join()

    elapsed = min(r[1] for r in results)
    agg = Counters()
    for _, _, c in results:
        agg.add(c)

    return elapsed, agg


# =========================
# Pretty printing
# =========================
def print_pretty_result(p_hot: float, worker: int, h: int, r: float, elapsed: float, agg: Counters):
    total = agg.commit + agg.abort
    abort_rate = (agg.abort / total) if total > 0 else 0.0
    tps = agg.commit / elapsed if elapsed > 0 else 0.0

    print("\n" + "=" * 70)
    print(f"[p_hot={p_hot:.3f}]  elapsed={elapsed:.3f}s  workers={worker}   H={h}   RO={r}")
    print(f"TPS(commit/s)={tps:.2f}  commit={agg.commit}  abort={agg.abort}  abort_rate={abort_rate*100:.2f}%")

    print("\n--- abort breakdown ---")
    print(f"LOCK     : {agg.abort_lock}   (deadlock/timeout/nowait)")
    print(f"VALIDATE : {agg.abort_validate}   (CAS rowcount==0)")
    print(f"DEADLOCK : {agg.abort_deadlock}")
    print(f"TIMEOUT  : {agg.abort_timeout}")
    print(f"OTHER    : {agg.abort_other}")

    print("\n--- avg latency per commit (ms) ---")
    if agg.commit > 0:
        print(f"BEGIN     : {agg.t_begin_ms   / agg.commit:.3f}")
        print(f"READ      : {agg.t_read_ms    / agg.commit:.3f}")
        print(f"TID alloc : {agg.t_tidalloc_ms/ agg.commit:.3f}")
        print(f"INSTALL   : {agg.t_install_ms / agg.commit:.3f}")
        print(f"COMMIT    : {agg.t_commit_ms  / agg.commit:.3f}")
    else:
        print("(no commits)")

    print("\n--- avg rollback latency (ms) ---")
    print(f"ROLLBACK* : {agg.t_rollback_ms / max(1, agg.abort):.3f}")
    print("=" * 70)


# =========================
# Main
# =========================
def main():
    for w in CONFIG["worker_list"]:
        for p_hot in CONFIG["p_hot_list"]:
            for h in CONFIG["H"]:
                for r in CONFIG["ro_ratio_list"]:
                    elapsed, agg = run_once(float(p_hot), int(w), int(h), float(r))
                    print_pretty_result(float(p_hot), int(w), int(h), float(r), elapsed, agg)
    print("\n\n--------------------\n\n\n\n\n\n\n")
    for w in CONFIG["worker_list2"]:
        for p_hot in CONFIG["p_hot_list"]:
            for h in CONFIG["H"]:
                for r in CONFIG["ro_ratio_list2"]:
                    elapsed, agg = run_once(float(p_hot), int(w), int(h), float(r))
                    print_pretty_result(float(p_hot), int(w), int(h), float(r), elapsed, agg)


if __name__ == "__main__":
    main()
