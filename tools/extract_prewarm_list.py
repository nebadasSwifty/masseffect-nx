#!/usr/bin/env python3
"""Extract the pipeline prewarm list from cache/masseffect_native_pipelines.bin.

The pipelines file is "NFPC" (u32 magic, u32 version, u64 list bytes, u64 cache bytes) followed by the list
("NFPL": u32 magic, u32 version, u32 record size, u32 record count, then the records) and the Vulkan cache data.
Only the list is written out: pipeline keys, vertex formats and shader fingerprints (our data, no game data, no
compiled code). Ship it next to the NRO and set masseffect_native_pipelines_shipped_list to its name
(docs/cold-start-hitches.md).

usage: tools/extract_prewarm_list.py masseffect_native_pipelines.bin masseffect_prewarm_list.bin [more.bin ...]
Several input files are merged (duplicates by record bytes dropped, first occurrence order kept).
"""
import struct
import sys

NFPC = 0x4350464E
NFPL = 0x4C50464E


def read_list(path):
    data = open(path, "rb").read()
    magic, version, list_bytes, cache_bytes = struct.unpack_from("<IIQQ", data, 0)
    if magic != NFPC or 24 + list_bytes + cache_bytes != len(data):
        raise SystemExit(f"{path}: not a pipelines file (magic {magic:08X}, version {version})")
    lst = data[24:24 + list_bytes]
    if len(lst) < 16:
        raise SystemExit(f"{path}: no prewarm list in it")
    lmagic, lversion, size, count = struct.unpack_from("<IIII", lst, 0)
    if lmagic != NFPL or len(lst) != 16 + size * count:
        raise SystemExit(f"{path}: damaged list")
    return lversion, size, [lst[16 + i * size:16 + (i + 1) * size] for i in range(count)]


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    out = sys.argv[2]
    inputs = [sys.argv[1]] + sys.argv[3:]
    version = size = None
    records, seen = [], set()
    for path in inputs:
        v, s, recs = read_list(path)
        if version is None:
            version, size = v, s
        elif (v, s) != (version, size):
            raise SystemExit(f"{path}: list version {v}/record size {s} differs from {version}/{size}")
        for r in recs:
            if r not in seen:
                seen.add(r)
                records.append(r)
    with open(out, "wb") as f:
        f.write(struct.pack("<IIII", NFPL, version, size, len(records)))
        for r in records:
            f.write(r)
    print(f"{out}: list version {version}, {len(records)} records of {size} bytes")


if __name__ == "__main__":
    main()
