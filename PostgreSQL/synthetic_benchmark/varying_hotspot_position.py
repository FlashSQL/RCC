# PostgreSQL version of distance sweep benchmark
import time
import random
import math
from multiprocessing import Process, Queue, current_process
import psycopg2
from psycopg2.extras import RealDictCursor

CONFIG = {
    "host": "localhost",
    "port": 5432,
    "dbname": "synt",
    "user": "postgres",
    "password": "",
    "warmup_sec": 50,
    "measure_sec": 50,
    "read_partitions": 128,
    "hotspot_id": 1,
    # Isolation level of every transaction: "RC" or "RR" (override: --isolation RC|RR)
    "isolation": "RR",
}

# Fixed experiment parameters
THREADS_FIXED = 128
READS_FIXED = 256
DISTANCES = [0.0, 0.25, 0.5, 0.75, 1.0]

ISOLATION_LEVELS = {"RC": "read committed", "RR": "repeatable read"}

def connect():
    return psycopg2.connect(
        options="-c default_transaction_isolation=" + ISOLATION_LEVELS[CONFIG["isolation"]].replace(" ", "\\ "),
        host=CONFIG.get("host", "localhost"),
        port=CONFIG.get("port", 5432),
        dbname=CONFIG["dbname"],
        user=CONFIG["user"],
        password=CONFIG.get("password", ""),
    )

def fetch_ids_for_p(p):
    conn = connect()
    try:
        cur = conn.cursor()
        cur.execute("SELECT id FROM bench_hotread WHERE p=%s", (p,))
        ids = [r[0] for r in cur.fetchall()]
        cur.close()
    finally:
        conn.close()
    return ids

def worker(proc_idx, total_procs, reads_per_tx, update_distance, result_q):
    conn = connect()
    conn.autocommit = False
    cur = conn.cursor()

    p = proc_idx % CONFIG["read_partitions"]
    id_pool = fetch_ids_for_p(p)
    if not id_pool:
        result_q.put(("error", f"empty partition p={p}"))
        cur.close()
        conn.close()
        return

    # PostgreSQL doesn't support partition hints in the same way as MySQL
    # We use direct table access when possible
    sfu_sql = "SELECT val FROM bench_hotread WHERE p=-1 AND id=%s FOR UPDATE"
    upd_sql = "UPDATE bench_hotread SET val=val+1 WHERE p=-1 AND id=%s"
    read_sql = "SELECT payload FROM bench_hotread WHERE p=%s AND id=%s"

    # Calculate update position: number of reads before update
    n_before = int(round(update_distance * reads_per_tx))
    n_before = max(0, min(reads_per_tx, n_before))  # clamp

    def do_reads(n):
        for _ in range(n):
            rid = random.choice(id_pool)
            cur.execute(read_sql, (p, rid))
            cur.fetchall()  # Consume results

    def do_update():
        # Get X_LOCK with FOR UPDATE then immediately UPDATE
        cur.execute(sfu_sql, (CONFIG["hotspot_id"],))
        cur.fetchall()  # Consume SELECT result
        cur.execute(upd_sql, (CONFIG["hotspot_id"],))

    # Warmup phase
    stop_at = time.monotonic() + CONFIG["warmup_sec"]
    while time.monotonic() < stop_at:
        try:
            cur.execute("BEGIN")
            do_reads(n_before)
            do_update()
            do_reads(reads_per_tx - n_before)
            conn.commit()
        except Exception:
            conn.rollback()

    # Measurement phase
    commits = 0
    aborts = 0
    latencies = []
    stop_at = time.monotonic() + CONFIG["measure_sec"]

    while time.monotonic() < stop_at:
        t0 = time.monotonic()
        try:
            cur.execute("BEGIN")
            do_reads(n_before)
            do_update()
            do_reads(reads_per_tx - n_before)
            conn.commit()
            commits += 1
            latencies.append((time.monotonic() - t0) * 1000.0)
        except Exception:
            conn.rollback()
            aborts += 1

    cur.close()
    conn.close()

    # Statistics
    def pct(xs, q):
        if not xs:
            return 0.0
        xs = sorted(xs)
        k = (q/100) * (len(xs) - 1)
        i = int(k)
        j = min(i + 1, len(xs) - 1)
        if i == j:
            return xs[i]
        return xs[i] * (j - k + i) + xs[j] * (k - i)

    sum_lat = sum(latencies)
    cnt_lat = len(latencies)
    avg = (sum_lat / cnt_lat) if cnt_lat else 0.0
    p95 = pct(latencies, 95)
    p99 = pct(latencies, 99)

    result_q.put(("ok", {
        "proc": proc_idx,
        "commits": commits,
        "aborts": aborts,
        "sum_lat": sum_lat,
        "cnt_lat": cnt_lat,
        "avg": avg,
        "p95": p95,
        "p99": p99
    }))

def run_one(procs, reads_per_tx, update_distance):
    q = Queue()
    ps = [Process(target=worker, args=(i, procs, reads_per_tx, update_distance, q), daemon=True)
          for i in range(procs)]
    for p in ps:
        p.start()
    results = [q.get() for _ in ps]
    for p in ps:
        p.join()

    ok_results = [r[1] for r in results if r[0] == "ok"]
    if not ok_results:
        return {
            "procs": procs,
            "reads": reads_per_tx,
            "distance": update_distance,
            "tps": 0.0,
            "commits": 0,
            "aborts": 0,
            "avg_ms": 0.0,
            "p95_ms": 0.0,
            "p99_ms": 0.0,
            "note": "; ".join(r[1] for r in results if r[0] == "error")[:200]
        }

    commits = sum(x["commits"] for x in ok_results)
    aborts = sum(x["aborts"] for x in ok_results)
    tps = commits / max(1, CONFIG["measure_sec"])

    # Weighted average (weighted by sample count)
    total_sum_lat = sum(x["sum_lat"] for x in ok_results)
    total_cnt_lat = sum(x["cnt_lat"] for x in ok_results)
    avg_ms = (total_sum_lat / total_cnt_lat) if total_cnt_lat else 0.0

    # Conservative upper bound: max of per-process p95/p99
    p95_ms = max(x["p95"] for x in ok_results) if ok_results else 0.0
    p99_ms = max(x["p99"] for x in ok_results) if ok_results else 0.0

    return {
        "procs": procs,
        "reads": reads_per_tx,
        "distance": update_distance,
        "tps": tps,
        "commits": commits,
        "aborts": aborts,
        "avg_ms": avg_ms,
        "p95_ms": p95_ms,
        "p99_ms": p99_ms,
        "note": ""
    }

def run_sweep(distances=DISTANCES, threads=THREADS_FIXED, reads=READS_FIXED):
    summaries = []
    print(f"# Running sweep (threads={threads}, reads={reads}, distances={distances})")

    for d in distances:
        print(f"\n=== UPDATE_DISTANCE = {d:.2f} ===")
        s = run_one(threads, reads, d)
        summaries.append(s)
        print(f"[RUN] th={s['procs']} reads={s['reads']} dist={s['distance']:.2f} | "
              f"TPS={s['tps']:.1f} commits={s['commits']} aborts={s['aborts']} | "
              f"avg={s['avg_ms']:.2f}ms p95={s['p95_ms']:.2f}ms p99={s['p99_ms']:.2f}ms"
              + (f" | note={s['note']}" if s['note'] else ""))

    print("\n# SUMMARY (CSV)")
    print("distance,tps,commits,aborts,avg_ms,p95_ms,p99_ms")
    for s in summaries:
        print(f"{s['distance']},{s['tps']:.1f},{s['commits']},{s['aborts']},{s['avg_ms']:.2f},{s['p95_ms']:.2f},{s['p99_ms']:.2f}")

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--isolation", choices=sorted(ISOLATION_LEVELS), default=CONFIG["isolation"])
    CONFIG["isolation"] = ap.parse_args().isolation
    print(f"[CONFIG] isolation={CONFIG['isolation']} ({ISOLATION_LEVELS[CONFIG['isolation']]})")
    run_sweep()
