#!/usr/bin/env python3
"""Simulate the block cache (masseffect_io_bcache_*) on a recorded read trace.

  usage: python3 tools/io_cache_sim.py TRACE.bin [--block-kb 32] [--sizes 32,64,128,192,256]
                                       [--ahead-kb 0,256,512] [--from-s 0]

TRACE.bin is the file written by the startup trace recorder (masseffect_io_trace_record = true,
default <parent of game folder>/cache/startup_reads.bin). To capture gameplay and not only the
start-up, raise masseffect_io_trace_record_s (e.g. 900 for 15 minutes); see docs/streaming-io.md.

For every cap and read-ahead it replays the guest reads in order through an LRU of blocks, the
same policy as block_cache.cpp (demand misses read in one call per contiguous run, synchronous
read-ahead appended to the last run when the request ends on a block boundary and is >= 64 KB),
and prints hit rates, SD bytes and SD calls, plus the SD time under the cost model measured on
the console: 2.3 ms per call + 69.5 MB/s (128 KB = 4.1 ms; the logs average 3.1-5.8 ms).
Requests larger than 1 MB bypass, as in the cache. --from-s skips the start (loading) so the
numbers describe gameplay only (the cache still warms up with the skipped reads).
"""
import argparse, collections, struct, sys

FIXED_MS, MB_PER_S = 2.3, 69.5


def load(path):
    d = open(path, 'rb').read()
    if d[:8] != b'RXSTRC01':
        sys.exit('not a RXSTRC01 trace: ' + path)
    _, fc, ec, _ = struct.unpack_from('<IIII', d, 8)
    p, files = 24, []
    for _ in range(fc):
        n, = struct.unpack_from('<H', d, p); p += 2
        name = d[p:p + n].decode(errors='replace'); p += n
        size, _ = struct.unpack_from('<QQ', d, p); p += 16
        files.append((name, size))
    reads = []
    for i in range(ec):
        kind, _, thread, fi, off, length, t_us = struct.unpack_from('<BBHIQII', d, p + 24 * i)
        if kind == 0 and length:
            reads.append((t_us / 1e6, fi, off, length, thread))
    return files, reads


def cost_ms(nbytes):
    return FIXED_MS + nbytes / (MB_PER_S * 1048576) * 1000


def simulate(files, reads, cap_mb, block, ahead_kb, from_s):
    cap = cap_mb * 1048576 // block
    lru = collections.OrderedDict()  # key -> is_ahead_unused
    ahead_blocks = (ahead_kb * 1024 + block - 1) // block
    st = collections.Counter()
    for t, fi, off, length, _ in reads:
        size = files[fi][1] if fi < len(files) else 0
        count = t >= from_s
        if length > 1048576 or (size and off + length > size):
            if count:
                st['bypass_bytes'] += length; st['sd_calls'] += 1; st['sd_bytes'] += length
                st['ms'] += cost_ms(length); st['base_ms'] += cost_ms(length)
            continue
        first, last = off // block, (off + length - 1) // block
        missing = []
        for b in range(first, last + 1):
            k = (fi, b)
            if k in lru:
                if lru[k] and count:
                    st['ahead_used'] += 1
                lru[k] = False
                lru.move_to_end(k)
                if count:
                    st['hit_blocks'] += 1
            else:
                missing.append(b)
                if count:
                    st['miss_blocks'] += 1
        extra = []
        if missing and missing[-1] == last and ahead_blocks and (off + length) % block == 0 and length >= 65536:
            for b in range(last + 1, last + 1 + ahead_blocks):
                if (size and b * block >= size) or (fi, b) in lru:
                    break
                extra.append(b)
        runs, prev = 0, None
        for b in missing:
            if prev is None or b != prev + 1:
                runs += 1
            prev = b
        nbytes = len(missing) * block + len(extra) * block
        if count:
            st['req_bytes'] += length
            st['sd_calls'] += runs
            st['sd_bytes'] += nbytes
            st['ahead_bytes'] += len(extra) * block
            # SD time: one call per run; the read-ahead rides on the last run.
            st["ms"] += (runs * FIXED_MS + nbytes / (MB_PER_S * 1048576) * 1000 if runs else 0)
            st['base_ms'] += cost_ms(length)
        for b in missing:
            lru[(fi, b)] = False
        for b in extra:
            lru[(fi, b)] = True
        while len(lru) > cap:
            _, unused = lru.popitem(last=False)
            if unused and count:
                st['ahead_wasted'] += 1
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--block-kb', type=int, default=32)
    ap.add_argument('--sizes', default='32,64,128,192,256')
    ap.add_argument('--ahead-kb', default='0,256,512')
    ap.add_argument('--from-s', type=float, default=0.0)
    a = ap.parse_args()
    files, reads = load(a.trace)
    if not reads:
        sys.exit('no reads in the trace')
    span = reads[-1][0] - reads[0][0]
    total = sum(r[3] for r in reads if r[0] >= a.from_s)
    sizes = collections.Counter(r[3] for r in reads)
    aligned = sum(1 for r in reads if r[2] % 32768 == 0)
    print(f'{len(reads)} reads over {span:.0f} s, {total / 1048576:.1f} MB after {a.from_s:.0f} s; '
          f'top sizes {sizes.most_common(3)}; offsets 32 KB-aligned {100 * aligned / len(reads):.0f} %')
    print(f'{"cap MB":>7} {"ahead KB":>8} {"hit %":>6} {"from RAM MB":>11} {"SD MB":>8} {"SD calls":>8} '
          f'{"SD s":>6} {"no-cache s":>10} {"ahead used/wasted":>18}')
    for cap in [int(x) for x in a.sizes.split(',')]:
        for ahead in [int(x) for x in a.ahead_kb.split(',')]:
            st = simulate(files, reads, cap, a.block_kb * 1024, ahead, a.from_s)
            blocks = st['hit_blocks'] + st['miss_blocks']
            ram = st['req_bytes'] - min(st['req_bytes'], st['miss_blocks'] * a.block_kb * 1024)
            print(f'{cap:>7} {ahead:>8} {100 * st["hit_blocks"] / max(blocks, 1):>6.1f} {ram / 1048576:>11.1f} '
                  f'{st["sd_bytes"] / 1048576:>8.1f} {st["sd_calls"]:>8} {st["ms"] / 1000:>6.1f} '
                  f'{st["base_ms"] / 1000:>10.1f} {st["ahead_used"]:>9}/{st["ahead_wasted"]}')


if __name__ == '__main__':
    main()
