#!/usr/bin/env python3
"""Latency breakdown (paper Fig 7a style) from TPC-C result dirs.

Usage: breakdown_report.py <results_dir>   # dir holding s1_t1, s1_t8, ... subdirs
Each subdir must have breakdown.txt (key=value) written by run_tpcc_threads.sh.

Categories:
  Execution   = committed_total - lock_wait - spin_wait - commit_wait
  Lock wait   = lock_wait (tuplock) + spin_wait (tuple_update)   [committed]
  Commit wait = commit_wait (txwait)                              [committed]
  Wasted      = aborted_total  (full aborted txn time: exec + waits)
  Total       = committed_total + aborted_total
"""
import sys, os, re, glob

def read_breakdown(path):
    d = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if '=' in line:
                k, v = line.split('=', 1)
                try: d[k] = float(v)
                except ValueError: pass
    return d

def read_sysbench(sysbench_log):
    """Return (committed_count, ignored_errors, tps) from sysbench output."""
    if not os.path.exists(sysbench_log): return (None, None, None)
    txt = open(sysbench_log, errors='ignore').read()
    m = re.search(r'transactions:\s+(\d+)\s+\(([\d.]+)\s+per sec', txt)
    e = re.search(r'ignored errors:\s+(\d+)', txt)
    committed = int(m.group(1)) if m else None
    tps = float(m.group(2)) if m else None
    ignored = int(e.group(1)) if e else 0
    return (committed, ignored, tps)

def fmt(ms):
    return f"{ms/1000:.1f}s" if ms >= 1000 else f"{ms:.0f}ms"

def report(d, n_committed, ignored, tps):
    ct = d.get('committed_total_ms', 0.0)
    at = d.get('aborted_total_ms', 0.0)
    lw = d.get('lock_wait_ms', 0.0)      # tuplock (committed)
    sw = d.get('spin_wait_ms', 0.0)      # tuple_update (committed)
    cw = d.get('commit_wait_ms', 0.0)    # txwait (committed)
    # amortized denominator = sysbench committed (business) transactions
    n = float(n_committed) if n_committed else (d.get('xact_commit', 0.0) or 1.0)
    ig = float(ignored or 0)
    abort_rate = 100*ig/(n+ig) if (n+ig) else 0   # aborts / all attempts
    execu = ct - lw - sw - cw
    total = ct + at
    parts = [
        ("Execution",        execu),
        ("Lock wait",        lw + sw),
        ("  - tuplock",      lw),
        ("  - spin(update)", sw),
        ("Commit wait",      cw),
        ("Wasted (abort)",   at),
    ]
    print(f"  TPS={tps}  commits={n:.0f}  ignored_errors={ig:.0f}  abort_rate={abort_rate:.1f}%  total={fmt(total)}")
    print(f"  {'component':<18}{'ms':>12}{'%':>8}{'ms/commit':>12}")
    for name, ms in parts:
        pct = 100*ms/total if total else 0
        print(f"  {name:<18}{ms:>12.0f}{pct:>7.1f}%{ms/n:>12.4f}")

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    root = sys.argv[1]
    subs = sorted(glob.glob(os.path.join(root, '*')),
                  key=lambda p: int(re.search(r't(\d+)$', p).group(1)) if re.search(r't(\d+)$', p) else 0)
    for sub in subs:
        bf = os.path.join(sub, 'breakdown.txt')
        if not os.path.isdir(sub) or not os.path.exists(bf): continue
        print(f"\n### {os.path.basename(sub)} ###")
        n_committed, ignored, tps = read_sysbench(os.path.join(sub, 'sysbench.log'))
        report(read_breakdown(bf), n_committed, ignored, tps)

if __name__ == '__main__':
    main()
