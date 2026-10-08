#!/usr/bin/env python3
"""Coverage of a pipeline prewarm list (NFPL, docs/cold-start-hitches.md section C) by a shader package index.

A record names its shaders by container fingerprint (masseffect::native::Shader::fingerprint = XXH3-64 of the whole
original guest container: header, constants and microcode). The game resolves them with
ShadersNative::PerFingerprint against the loaded package; a record whose shaders are missing (or of the wrong stage)
is skipped as kListNoShader. key.vs / key.ps are package entry numbers + 1 (position in the .idx entry table, which is
sorted by fingerprint), so they are edition specific: the ring builds its keys with the numbers of the package it
loaded.

usage:
  tools/prewarm_list_coverage.py LIST.bin TARGET.mesp.idx [--source-idx SOURCE.mesp.idx]
                                 [--filter-to TARGET.mesp.idx --out OUT.bin [--no-renumber]]

--source-idx      also check the list's key.vs/key.ps against the package the list was recorded with, and compare the
                  stored SPIR-V hash of each shared shader (identical SPIR-V = the same driver cache entries).
--filter-to IDX   write OUT with only the records whose shaders are all in IDX. key.vs/key.ps are rewritten to IDX's
                  entry numbers (so the ring's keys match the list's), unless --no-renumber. Duplicates dropped.
"""
import argparse
import struct
import sys

NFPL = 0x4C50464E
LIST_VERSION = 5
RECORD = 448
KEY = 104            # sizeof(PipelineKey)
OFF_VS_FP = KEY      # u64 vs_fingerprint
OFF_PS_FP = KEY + 8  # u64 ps_fingerprint
HEADER_IDX = 56
ENTRY_IDX = 40


def read_list(path):
    data = open(path, "rb").read()
    magic, version, size, count = struct.unpack_from("<IIII", data, 0)
    if magic != NFPL or len(data) != 16 + size * count:
        raise SystemExit(f"{path}: not a prewarm list")
    if version != LIST_VERSION or size != RECORD:
        raise SystemExit(f"{path}: list version {version} / record {size} bytes, this tool knows {LIST_VERSION}/{RECORD}")
    return version, size, [data[16 + i * size:16 + (i + 1) * size] for i in range(count)]


def read_idx(path):
    """fingerprint -> (entry number, is_vertex, spirv xxh3)"""
    data = open(path, "rb").read()
    if data[:8] != b"MESSIDX\0":
        raise SystemExit(f"{path}: not a shader package index")
    version, count = struct.unpack_from("<II", data, 8)
    bytes_original = struct.unpack_from("<Q", data, 32)[0]
    if version != 1 or len(data) != HEADER_IDX + count * ENTRY_IDX + bytes_original:
        raise SystemExit(f"{path}: index version {version} or truncated")
    out = {}
    pos = HEADER_IDX + count * ENTRY_IDX
    for i in range(count):
        original, words, fp, spirv_fp = struct.unpack_from("<IIQQ", data, HEADER_IDX + i * ENTRY_IDX)
        signature = struct.unpack_from(">I", data, pos)[0]
        pos += original
        out[fp] = (i, bool(signature & 1), spirv_fp)
    return out


def fields(r):
    vs, ps = struct.unpack_from("<II", r, 0)
    vs_fp, ps_fp = struct.unpack_from("<QQ", r, OFF_VS_FP)
    return vs, ps, vs_fp, ps_fp


def covered(r, idx):
    """None if all shaders are in idx with the right stage, else the reason."""
    vs, ps, vs_fp, ps_fp = fields(r)
    if not vs_fp or (ps and not ps_fp):
        return "no fingerprint (pre-fingerprint record)"
    e = idx.get(vs_fp)
    if not e:
        return "vs missing"
    if not e[1]:
        return "vs is not a vertex shader"
    if ps:
        e = idx.get(ps_fp)
        if not e:
            return "ps missing"
        if e[1]:
            return "ps is not a pixel shader"
    return None


def pct(a, b):
    return f"{100.0 * a / b:.1f} %" if b else "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("list")
    ap.add_argument("idx", nargs="?")
    ap.add_argument("--source-idx")
    ap.add_argument("--filter-to")
    ap.add_argument("--out")
    ap.add_argument("--no-renumber", action="store_true")
    a = ap.parse_args()
    if not a.idx and not a.filter_to:
        ap.error("give a target idx or --filter-to")
    if a.filter_to and not a.out:
        ap.error("--filter-to needs --out")

    version, size, records = read_list(a.list)
    n = len(records)
    print(f"{a.list}: {n} records")
    target = read_idx(a.idx or a.filter_to)
    vs_fps = {fields(r)[2] for r in records if fields(r)[2]}
    ps_fps = {fields(r)[3] for r in records if fields(r)[3]}
    print(f"  distinct shaders: {len(vs_fps)} vs, {len(ps_fps)} ps")

    if a.idx:
        reasons = {}
        ok = 0
        same_numbers = 0
        for r in records:
            why = covered(r, target)
            if why is None:
                ok += 1
                vs, ps, vs_fp, ps_fp = fields(r)
                if target[vs_fp][0] + 1 == vs and (not ps or target[ps_fp][0] + 1 == ps):
                    same_numbers += 1
            else:
                reasons[why] = reasons.get(why, 0) + 1
        print(f"{a.idx}: {len(target)} shaders")
        print(f"  records with all shaders present: {ok}/{n} ({pct(ok, n)})")
        for why, k in sorted(reasons.items(), key=lambda x: -x[1]):
            print(f"    {why}: {k}")
        print(f"  shaders present: vs {sum(f in target for f in vs_fps)}/{len(vs_fps)}, "
              f"ps {sum(f in target for f in ps_fps)}/{len(ps_fps)}")
        print(f"  covered records whose key.vs/key.ps equal this package's entry numbers: {same_numbers}/{ok}")

    if a.source_idx and a.idx:
        source = read_idx(a.source_idx)
        shared = [f for f in vs_fps | ps_fps if f in source and f in target]
        same_spirv = sum(source[f][2] == target[f][2] for f in shared)
        print(f"source {a.source_idx}: list shaders in both packages: {len(shared)}, "
              f"identical stored SPIR-V: {same_spirv}/{len(shared)}")
        src_ok = sum(1 for r in records if covered(r, source) is None)
        print(f"  records covered by the source package: {src_ok}/{n}")

    if a.filter_to:
        out, seen, renumbered = [], set(), 0
        for r in records:
            if covered(r, target) is not None:
                continue
            if not a.no_renumber:
                vs, ps, vs_fp, ps_fp = fields(r)
                nvs = target[vs_fp][0] + 1
                nps = target[ps_fp][0] + 1 if ps else 0
                if (nvs, nps) != (vs, ps):
                    renumbered += 1
                    r = struct.pack("<II", nvs, nps) + r[8:]
            if r[:KEY] in seen:
                continue
            seen.add(r[:KEY])
            out.append(r)
        with open(a.out, "wb") as f:
            f.write(struct.pack("<IIII", NFPL, version, size, len(out)))
            for r in out:
                f.write(r)
        print(f"{a.out}: {len(out)} records written ({n - len(out)} dropped, {renumbered} renumbered)")


if __name__ == "__main__":
    main()
