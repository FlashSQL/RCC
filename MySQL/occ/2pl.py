#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import random
import time
from dataclasses import dataclass
from multiprocessing import Process, Queue
from typing import List, Tuple, Optional

# =========================
# CONFIG
# =========================
CONFIG = {
    "mysql": {
        "host": "127.0.0.1",
        "port": 3306,
        "user": "root",
        "passwd": "",
        "db": "occbench",
        "unix_socket": "/tmp/mysql.sock",
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
    "lock_wait_timeout": 1,

    "use_commit_tid": True,
    "tid_block": 2000,
}

try:
    import MySQLdb as mysql
except Exception:
    import pymysql as mysql

ER_LOCK_WAIT_TIMEOUT = 1205
ER_LOCK_DEADLOCK = 1213


# =========================
# Counters
# =========================
@dataclass
class Counters:
    commit: int = 0
    abort: int = 0

    ro_txn: int = 0
    rmw_txn: int = 0

    abort_deadlock: int = 0
    abort_timeout: int = 0
    abort_other: int = 0

    # timing (ms)
    t_tid_alloc_ms: float = 0.0
    t_update_ms: float = 0.0
    t_read_ms: float = 0.0
    t_commit_ms: float = 0.0
    t_rollback_ms: float = 0.0
    t_begin_ms: float = 0.0

    def add(self, other: "Counters"):
        for k in self.__dict__.keys():
            setattr(self, k, getattr(self, k) + getattr(other, k))


# =========================
# TID Allocator
# =========================
class TidAllocator:
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
    if cfg.get("unix_socket"):
        kwargs["unix_socket"] = cfg["unix_socket"]
    else:
        kwargs["host"] = cfg.get("host", "127.0.0.1")
        kwargs["port"] = int(cfg.get("port", 3306))
    return mysql.connect(**kwargs)


def set_session(conn):
    cur = conn.cursor()
    cur.execute("SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE")
    cur.execute(
        "SET SESSION innodb_lock_wait_timeout = %s",
        (int(CONFIG["lock_wait_timeout"]),),
    )
    cur.close()


# =========================
# Workload helpers
# =========================
def pick_keys(rng: random.Random, k: int, p_hot: float) -> List[int]:
    N = int(CONFIG["N"])
    H = int(CONFIG["h_temp"])
    if k <= 0:
        return []

    keys = set()
    while len(keys) < k:
        if rng.random() < p_hot and H > 0:
            keys.add(rng.randint(1, H))
        else:
            if N <= H:
                keys.add(rng.randint(1, max(1, N)))
            else:
                keys.add(rng.randint(H + 1, N))
    return sorted(keys)


def _select_in(cur, cols: str, ids: List[int]):
    if not ids:
        return []
    ph = ",".join(["%s"] * len(ids))
    sql = f"SELECT {cols} FROM kv WHERE id IN ({ph})"
    cur.execute(sql, tuple(ids))
    return cur.fetchall()


def classify_abort(counters: Counters, e: Exception):
    counters.abort += 1
    code = e.args[0] if e.args else None
    if code == ER_LOCK_DEADLOCK:
        counters.abort_deadlock += 1
    elif code == ER_LOCK_WAIT_TIMEOUT:
        counters.abort_timeout += 1
    else:
        counters.abort_other += 1


# =========================
# RO transaction
# =========================
def txn_ro(conn, rng: random.Random, counters: Counters, p_hot: float) -> bool:
    counters.ro_txn += 1
    cur = conn.cursor()
    try:
        t0 = time.monotonic()
        cur.execute("START TRANSACTION")
        counters.t_begin_ms += (time.monotonic() - t0) * 1000.0

        read_ids = pick_keys(rng, int(CONFIG["ro_read_keys"]), p_hot)
        t0 = time.monotonic()
        _select_in(cur, "id, v, tid", read_ids)
        counters.t_read_ms += (time.monotonic() - t0) * 1000.0

        t0 = time.monotonic()
        conn.commit()
        counters.t_commit_ms += (time.monotonic() - t0) * 1000.0

        counters.commit += 1
        return True
    except Exception as e:
        try:
            t0 = time.monotonic()
            conn.rollback()
            counters.t_rollback_ms += (time.monotonic() - t0) * 1000.0
        except Exception:
            pass
        classify_abort(counters, e)
        return False
    finally:
        cur.close()


# =========================
# RMW transaction (write-only)
# =========================
def txn_rmw_2pl_early_lock(
    conn,
    rng: random.Random,
    counters: Counters,
    tid_alloc: Optional[TidAllocator],
    p_hot: float,
) -> bool:
    counters.rmw_txn += 1
    cur = conn.cursor()

    try:
        t0 = time.monotonic()
        cur.execute("START TRANSACTION")
        counters.t_begin_ms += (time.monotonic() - t0) * 1000.0

        write_ids = pick_keys(rng, int(CONFIG["rmw_write_keys"]), p_hot)

        if CONFIG["use_commit_tid"] and tid_alloc is not None:
            t0 = time.monotonic()
            commit_tid = tid_alloc.get()
            counters.t_tid_alloc_ms += (time.monotonic() - t0) * 1000.0
        else:
            commit_tid = 0

        t0 = time.monotonic()
        for wid in write_ids:
            if CONFIG["use_commit_tid"]:
                cur.execute(
                    "UPDATE kv SET v = v + 1, tid = %s WHERE id = %s",
                    (commit_tid, wid),
                )
            else:
                cur.execute("UPDATE kv SET v = v + 1 WHERE id = %s", (wid,))
            if cur.rowcount != 1:
                raise RuntimeError("update failed")
        counters.t_update_ms += (time.monotonic() - t0) * 1000.0

        t0 = time.monotonic()
        conn.commit()
        counters.t_commit_ms += (time.monotonic() - t0) * 1000.0

        counters.commit += 1
        return True

    except Exception as e:
        try:
            t0 = time.monotonic()
            conn.rollback()
            counters.t_rollback_ms += (time.monotonic() - t0) * 1000.0
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
    rng = random.Random(int(CONFIG["seed"]) + wid * 1_000_003)
    CONFIG["h_temp"] = int(h)

    conn = connect_mysql(False)
    set_session(conn)

    tid_conn = None
    tid_alloc: Optional[TidAllocator] = None
    if CONFIG["use_commit_tid"]:
        tid_conn = connect_mysql(True)
        tid_alloc = TidAllocator(tid_conn, int(CONFIG["tid_block"]))

    warmup_end = time.time() + int(CONFIG["warmup_sec"])
    measure_end = warmup_end + int(CONFIG["measure_sec"])

    # Warmup
    dummy = Counters()
    while time.time() < warmup_end:
        if rng.random() < float(r):
            txn_ro(conn, rng, dummy, p_hot)
        else:
            txn_rmw_2pl_early_lock(conn, rng, dummy, tid_alloc, p_hot)

    # Measure
    counters = Counters()
    t0 = time.time()
    while time.time() < measure_end:
        if rng.random() < float(r):
            txn_ro(conn, rng, counters, p_hot)
        else:
            txn_rmw_2pl_early_lock(conn, rng, counters, tid_alloc, p_hot)

    elapsed = max(1e-9, time.time() - t0)

    if tid_alloc:
        tid_alloc.close()
    if tid_conn:
        tid_conn.close()
    conn.close()

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
def print_pretty_result(p_hot: float, worker: int, h: float, r: float, elapsed: float, agg: Counters):
    total = agg.commit + agg.abort
    abort_rate = (agg.abort / total) if total > 0 else 0.0
    tps = agg.commit / elapsed if elapsed > 0 else 0.0

    print("\n" + "=" * 70)
    print(f"[p_hot={p_hot:.3f}]  elapsed={elapsed:.3f}s  workers={worker}   H={h}   RO={r}")
    print(f"TPS(commit/s)={tps:.2f}  commit={agg.commit}  abort={agg.abort}  abort_rate={abort_rate*100:.2f}%")
    print(f"RO txns={agg.ro_txn}  RMW txns={agg.rmw_txn}")

    print("\n--- abort breakdown ---")
    print(f"DEADLOCK : {agg.abort_deadlock}")
    print(f"TIMEOUT  : {agg.abort_timeout}")
    print(f"OTHER    : {agg.abort_other}")

    if agg.commit > 0:
        print("\n--- avg latency per commit (ms) ---")
        print(f"BEGIN     : {agg.t_begin_ms / agg.commit:.3f}")
        print(f"TID alloc : {agg.t_tid_alloc_ms / agg.commit:.3f}")
        print(f"UPDATE    : {agg.t_update_ms / agg.commit:.3f}")
        print(f"READ      : {agg.t_read_ms / agg.commit:.3f}")
        print(f"COMMIT    : {agg.t_commit_ms / agg.commit:.3f}")

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
