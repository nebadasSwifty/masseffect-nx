#!/usr/bin/env python3
"""CPU cost per frame from the profiler logs of console runs (the stable metric for CPU changes, see docs/measuring.md).

  python3 tools/console-test/cpu_per_frame.py RUN_OR_LOG [RUN_OR_LOG ...] [--skip N]

Each argument is a profiler log (rex_profile.log) or a folder that holds one (a run folder written by
switch_cycle.sh, out/console-test/<name>/). The profiler writes one block every 10 seconds. For every block the CPU
percentages are converted to core-milliseconds per frame (CPU % x 10 / fps) and averaged over the blocks:
  total   all threads together
  main    the busiest game thread (XThread*)
  ring    the GPU ring thread
The first --skip blocks (default 3: menus and loading) and blocks below 5 fps are left out.
"""
import os
import re
import sys

BLOCK_RE = re.compile(r'game ([0-9.]+) fps \| CPU total ([0-9]+)%')   # the profiler line of a block
THREAD_RE = re.compile(r'-- thread \d+ "([^"]+)": CPU ([0-9.]+)%')


def find_log(arg):
    if os.path.isdir(arg):
        for name in ('rex_profile.log', 'profile.log', 'profile.log'):
            path = os.path.join(arg, name)
            if os.path.isfile(path):
                return path
        return None
    return arg if os.path.isfile(arg) else None


def main(argv):
    skip = 3
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == '--skip':
            skip = int(argv[i + 1]); i += 2
        else:
            paths.append(argv[i]); i += 1
    if not paths:
        sys.exit(__doc__)
    for arg in paths:
        log = find_log(arg)
        if not log:
            print(f'{arg}: no profiler log'); continue
        blocks = open(log, errors='replace').read().split('\n====')
        rows = []
        for block in blocks[skip:]:
            m = BLOCK_RE.search(block)
            if not m:
                continue
            fps, total = float(m.group(1)), float(m.group(2))
            threads = THREAD_RE.findall(block)
            main_cpu = [float(c) for name, c in threads if name.startswith('XThread')]
            ring_cpu = [float(c) for name, c in threads if name.startswith('GPU ring')]
            if fps < 5 or not main_cpu:
                continue
            rows.append((fps, total, max(main_cpu), ring_cpu[0] if ring_cpu else 0.0))
        if not rows:
            print(f'{arg}: no usable blocks'); continue
        n = len(rows)
        per_frame = lambda k: sum(r[k] / 100 * 1000 / r[0] for r in rows) / n   # core-ms per frame
        fps = sum(r[0] for r in rows) / n
        print(f'{os.path.basename(arg.rstrip("/")):20s} fps {fps:5.1f}  total {per_frame(1):6.1f} core-ms/frame  '
              f'main {per_frame(2):5.1f}  ring {per_frame(3):5.1f}  (n={n})')


if __name__ == '__main__':
    main(sys.argv[1:])
