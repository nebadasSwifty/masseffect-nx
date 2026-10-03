#!/usr/bin/env python3
"""Run every fuzz case in its own build (cases may define clashing stubs), 4 in parallel.
  python3 tests/hot_fuzz/run_all.py [--iters N] [--seed S] [addr ...]
Prints one summary line per case. REXSDK (env) selects the SDK headers (default: build.py's search)."""
import glob, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
args = sys.argv[1:]
iters, seed, only = '200000', '1', []
i = 0
while i < len(args):
    if args[i] == '--iters': iters = args[i + 1]; i += 2
    elif args[i] == '--seed': seed = args[i + 1]; i += 2
    else: only.append(args[i]); i += 1
cases = sorted(re.findall(r'case_([0-9A-F]{8})\.inc', ' '.join(os.listdir(os.path.join(ROOT, 'tests/hot_fuzz/cases')))))
if only: cases = [c for c in cases if c in only]

def run(a):
    out = os.path.join(ROOT, 'out', 'hot_fuzz', 'fz_%s_%s' % (a, seed))
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tests/hot_fuzz/build.py'), '--iters', iters, '--seed', seed, '--out', out, a],
                       capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.startswith('sub_') or 'FAIL' in l or 'error' in l]
    return a, (lines[-1] if lines else 'NO RESULT: ' + (r.stderr or r.stdout)[-300:])

with ThreadPoolExecutor(4) as ex:
    for a, l in ex.map(run, cases):
        print(a, l[:220], flush=True)
