# mp_bench_template_looped.py
import time, random, math
from multiprocessing import Process, Queue, current_process
import MySQLdb as mysql  

##### YOU CAN CUSTOMIZE HERE #####
WARM_UP = 50
RUNTIME = 50
THREADS_LIST = [1,8,16,32,64,128]
READS_LIST   = [64,128,256]
##################################

CONFIG = {
    "user": "root", "passwd": "", "db": "synthetic",
    "warmup_sec": WARM_UP, "measure_sec": RUNTIME,
    "read_partitions": 128, "hotspot_id": 1,
    "use_partition_hint": True,
}


def connect():
    return mysql.connect(unix_socket="/tmp/mysql.sock",
                         user=CONFIG["user"],
                         passwd=CONFIG["passwd"],
                         db=CONFIG["db"],
                         charset="utf8mb4",
                         autocommit=False)

def fetch_ids_for_p(p):
    with connect() as conn:
        cur = conn.cursor()
        cur.execute("SELECT id FROM bench_hotread WHERE p=%s", (p,))
        ids = [r[0] for r in cur.fetchall()]
        cur.close()
    return ids

def worker(proc_idx, total_procs, reads_per_tx, result_q):
    conn = connect(); cur = conn.cursor()
    p = proc_idx % CONFIG["read_partitions"]
    id_pool = fetch_ids_for_p(p)
    if not id_pool:
        result_q.put(("error", f"empty partition p={p}")); 
        cur.close(); conn.close()
        return

    sfu_sql = "SELECT val FROM bench_hotread PARTITION (p_hot) WHERE p=-1 AND id=%s FOR UPDATE" \
              if CONFIG["use_partition_hint"] else \
              "SELECT val FROM bench_hotread WHERE p=-1 AND id=%s FOR UPDATE"
    upd_sql = "UPDATE bench_hotread PARTITION (p_hot) SET val=val+1 WHERE p=-1 AND id=%s" \
              if CONFIG["use_partition_hint"] else \
              "UPDATE bench_hotread SET val=val+1 WHERE p=-1 AND id=%s"
    read_sql = f"SELECT payload FROM bench_hotread PARTITION (p{p}) WHERE p=%s AND id=%s" \
               if CONFIG["use_partition_hint"] else \
               "SELECT payload FROM bench_hotread WHERE p=%s AND id=%s"


    stop_at = time.monotonic() + CONFIG["warmup_sec"]
    while time.monotonic() < stop_at:
        try:
            cur.execute("BEGIN")
            cur.execute(sfu_sql, (CONFIG["hotspot_id"],))
            cur.execute(upd_sql, (CONFIG["hotspot_id"],))
            for _ in range(reads_per_tx):
                rid = random.choice(id_pool)
                cur.execute(read_sql, (p, rid))
            conn.commit()
        except Exception:
            conn.rollback()

    commits = 0; aborts = 0; latencies = []
    stop_at = time.monotonic() + CONFIG["measure_sec"]
    while time.monotonic() < stop_at:
        t0 = time.monotonic()
        try:
            cur.execute("BEGIN")
            cur.execute(sfu_sql, (CONFIG["hotspot_id"],))
            cur.execute(upd_sql, (CONFIG["hotspot_id"],))
            for _ in range(reads_per_tx):
                rid = random.choice(id_pool)
                cur.execute(read_sql, (p, rid))
            conn.commit()
            commits += 1
            latencies.append((time.monotonic() - t0) * 1000.0)
        except Exception:
            conn.rollback(); aborts += 1

    cur.close(); conn.close()

    def pct(xs, q):
        if not xs: return 0.0
        xs = sorted(xs)
        k = (q/100)*(len(xs)-1)
        i = int(k); j = min(i+1, len(xs)-1)
        if i == j: return xs[i]
        return xs[i] * (j - k + i) + xs[j] * (k - i)

    sum_lat = sum(latencies)
    cnt_lat = len(latencies)
    avg = (sum_lat / cnt_lat) if cnt_lat else 0.0
    p95 = pct(latencies, 95)
    p99 = pct(latencies, 99)

    result_q.put(("ok", {
        "proc": proc_idx, "commits": commits, "aborts": aborts,
        "sum_lat": sum_lat, "cnt_lat": cnt_lat,
        "avg": avg, "p95": p95, "p99": p99
    }))

def run_one(procs, reads_per_tx):
    q = Queue()
    ps = [Process(target=worker, args=(i, procs, reads_per_tx, q), daemon=True) for i in range(procs)]
    for p in ps: p.start()
    results = [q.get() for _ in ps]
    for p in ps: p.join()

    ok_results = [r[1] for r in results if r[0] == "ok"]
    if not ok_results:
        return {
            "procs": procs, "reads": reads_per_tx,
            "tps": 0.0, "commits": 0, "aborts": 0,
            "avg_ms": 0.0, "p95_ms": 0.0, "p99_ms": 0.0,
            "note": "; ".join(r[1] for r in results if r[0] == "error")[:200]
        }

    commits = sum(x["commits"] for x in ok_results)
    aborts  = sum(x["aborts"]  for x in ok_results)

    tps = commits / max(1, CONFIG["measure_sec"])

    total_sum_lat = sum(x["sum_lat"] for x in ok_results)
    total_cnt_lat = sum(x["cnt_lat"] for x in ok_results)
    avg_ms = (total_sum_lat / total_cnt_lat) if total_cnt_lat else 0.0

    p95_ms = max(x["p95"] for x in ok_results)
    p99_ms = max(x["p99"] for x in ok_results)

    return {
        "procs": procs, "reads": reads_per_tx,
        "tps": tps, "commits": commits, "aborts": aborts,
        "avg_ms": avg_ms, "p95_ms": p95_ms, "p99_ms": p99_ms,
        "note": ""
    }


def run_sweep(threads_list=THREADS_LIST, reads_list=READS_LIST):
    summaries = []
    print("# Running sweep...")
    for reads in reads_list:
        print(f"\n=== READS_PER_TX = {reads} ===")
        for th in threads_list:
            s = run_one(th, reads)
            summaries.append(s)
            print(f"[RUN] threads={s['procs']:>2} reads={s['reads']:>3} | "
                  f"TPS={s['tps']:.1f} commits={s['commits']} aborts={s['aborts']} | "
                  f"avg={s['avg_ms']:.2f}ms p95={s['p95_ms']:.2f}ms p99={s['p99_ms']:.2f}ms"
                  + (f" | note={s['note']}" if s['note'] else ""))    

if __name__ == "__main__":
    run_sweep()
