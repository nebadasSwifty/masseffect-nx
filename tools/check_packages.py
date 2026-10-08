#!/usr/bin/env python3
"""Checks Mass Effect 1 (Xbox 360) UE3 packages for truncation and header damage.

usage: python3 tools/check_packages.py [-v] [--glob PATTERN] [--json] <folder or file>...

Every *.xxx / *.upk / *.sfm / *.u file under the given folders is parsed. A file is reported BAD when
its package summary does not parse, a table offset lies outside the package, or (compressed packages)
a chunk or one of its LZX blocks lies past the end of the file. Packages of one game root (same LayerN
parent) that share a package GUID under different names are reported as DUP (placeholder copy of
another map). Exit code 1 when anything is reported.

ME1 Xbox 360 package summary (big-endian, file version 391, licensee 92), offsets for the usual
5-byte folder name "None":

  0x00 u32  Tag 0x9E2A83C1
  0x04 u32  (licensee << 16) | file version          = 0x005C0187
  0x08 u32  TotalHeaderSize (end of the header in the uncompressed stream)
  0x0C FStr FolderName (i32 length incl. NUL, ASCII; negative length = UTF-16)
  0x15 u32  PackageFlags
  0x19 u32  NameCount      0x1D u32 NameOffset
  0x21 u32  ExportCount    0x25 u32 ExportOffset
  0x29 u32  ImportCount    0x2D u32 ImportOffset
  0x31 16B  Guid
  0x41 u32  GenerationCount, then GenerationCount x (ExportCount, NameCount, NetObjectCount)
  +0  u32   EngineVersion (2674)
  +4  u32   CookerVersion (33)
  +8  7 x u32  ME1-specific fields (one per-file hash, the rest constant: 0xBA53, 0, 0x02830000, ...)
  +36 u32   CompressionFlags (0 none, 1 zlib, 2 LZX, 4 LZO)
  +40 u32   ChunkCount, then ChunkCount x (UncompressedOffset, UncompressedSize,
            CompressedOffset, CompressedSize)
  Uncompressed packages carry one more u32 after the (empty) chunk table; the name table follows.

Each compressed chunk starts with: Tag 0x9E2A83C1, BlockSize (ME1 Xbox: the tag again, blocks are 0x20000),
CompressedSize, UncompressedSize,
then one (CompressedSize, UncompressedSize) pair per block and the block data back to back.
Name/Export/Import offsets and TotalHeaderSize are positions in the uncompressed stream.
"""
import argparse
import fnmatch
import json
import os
import struct
import sys

TAG = 0x9E2A83C1
EXTENSIONS = (".xxx", ".upk", ".sfm", ".u")
EXPECTED_VERSION = 0x005C0187


class Bad(Exception):
    pass


def u32(buf, off):
    if off + 4 > len(buf):
        raise Bad("summary cut at 0x%X" % off)
    return struct.unpack_from(">I", buf, off)[0]


def parse_summary(head):
    """Returns a dict with the summary fields; raises Bad on a malformed header."""
    if len(head) < 4 or u32(head, 0) != TAG:
        raise Bad("no package tag (first bytes %s)" % head[:4].hex())
    s = {"version": u32(head, 4), "header_size": u32(head, 8)}
    flen = struct.unpack_from(">i", head, 12)[0] if len(head) >= 16 else None
    if flen is None or not -256 <= flen <= 256:
        raise Bad("bad folder name length %r" % flen)
    pos = 16 + (flen if flen >= 0 else -2 * flen)
    s["package_flags"] = u32(head, pos)
    (s["name_count"], s["name_offset"], s["export_count"], s["export_offset"],
     s["import_count"], s["import_offset"]) = (u32(head, pos + 4 + 4 * i) for i in range(6))
    s["guid"] = head[pos + 28:pos + 44].hex()
    pos += 28 + 16  # flags, 6 table fields, guid
    gens = u32(head, pos)
    if gens > 1000:
        raise Bad("generation count %d" % gens)
    pos += 4 + 12 * gens
    s["engine_version"] = u32(head, pos)
    s["cooker_version"] = u32(head, pos + 4)
    pos += 8 + 28
    s["compression"] = u32(head, pos)
    count = u32(head, pos + 4)
    if s["compression"] not in (0, 1, 2, 4):
        raise Bad("compression flags %d at 0x%X" % (s["compression"], pos))
    if count > 4096 or (count and s["compression"] == 0) or (s["compression"] and not count):
        raise Bad("chunk count %d with compression %d" % (count, s["compression"]))
    pos += 8
    chunks = []
    if pos + 16 * count > len(head):
        raise Bad("chunk table cut")
    for i in range(count):
        chunks.append(struct.unpack_from(">IIII", head, pos + 16 * i))
    s["chunks"] = chunks
    s["table_end"] = pos + 16 * count
    return s


def check_file(path):
    """Returns (status, problems, info). status is 'ok' or 'bad'."""
    size = os.path.getsize(path)
    problems = []
    info = {"size": size}
    with open(path, "rb") as fh:
        head = fh.read(0x10000)
        try:
            s = parse_summary(head)
        except Bad as e:
            return "bad", [str(e)], info
        except struct.error as e:
            return "bad", ["summary cut: %s" % e], info
        info.update(compression=s["compression"], chunks=len(s["chunks"]), guid=s["guid"])
        if s["version"] != EXPECTED_VERSION:
            problems.append("version 0x%08X (expected 0x%08X)" % (s["version"], EXPECTED_VERSION))
        chunks = s["chunks"]
        if chunks:
            stream_size = max(uo + us for uo, us, _, _ in chunks)
            if chunks[0][0] > s["header_size"]:
                problems.append("first chunk uncompressed offset %d implausible" % chunks[0][0])
            prev_uend = chunks[0][0]
            need = 0
            for i, (uo, us, co, cs) in enumerate(chunks):
                if uo != prev_uend:
                    problems.append("chunk %d uncompressed offset %d, expected %d" % (i, uo, prev_uend))
                prev_uend = uo + us
                need = max(need, co + cs)
                if co + cs > size:
                    problems.append("chunk %d needs bytes %d..%d, file has %d" % (i, co, co + cs, size))
                    continue
                fh.seek(co)
                ch = fh.read(16)
                tag, block, csz, usz = struct.unpack(">IIII", ch) if len(ch) == 16 else (0, 0, 0, 0)
                if tag != TAG:
                    problems.append("chunk %d at %d: no tag (%08X)" % (i, co, tag))
                    continue
                if usz != us or block == 0:
                    problems.append("chunk %d header sizes %d/%d disagree with table %d" % (i, usz, block, us))
                    continue
                if block == TAG:  # ME1 Xbox writes the tag again where UE3 stores BlockSize
                    block = 0x20000
                nblocks = (usz + block - 1) // block
                bt = fh.read(8 * nblocks)
                if len(bt) != 8 * nblocks:
                    problems.append("chunk %d block table cut" % i)
                    continue
                pairs = struct.unpack(">%dI" % (2 * nblocks), bt)
                sum_c, sum_u = sum(pairs[0::2]), sum(pairs[1::2])
                if sum_u != usz or sum_c != csz:
                    problems.append("chunk %d block sums %d/%d != %d/%d" % (i, sum_c, sum_u, csz, usz))
                if co + 16 + 8 * nblocks + sum_c > co + cs or co + 16 + 8 * nblocks + sum_c > size:
                    problems.append("chunk %d blocks end at %d (chunk end %d, file %d)"
                                    % (i, co + 16 + 8 * nblocks + sum_c, co + cs, size))
            info["needs"] = need
            info["stream_size"] = stream_size
        else:
            stream_size = size
        for name in ("name", "export", "import"):
            off, cnt = s[name + "_offset"], s[name + "_count"]
            if cnt and not 0 < off < stream_size:
                problems.append("%s table offset %d outside package (%d)" % (name, off, stream_size))
        if not 0 < s["header_size"] <= stream_size:
            problems.append("header size %d outside package (%d)" % (s["header_size"], stream_size))
        if not chunks and s["header_size"] > size:
            problems.append("file cut inside the header")
    return ("bad" if problems else "ok"), problems, info


def walk(roots, pattern):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, dirs, names in os.walk(root):
            dirs.sort()
            for n in sorted(names):
                if n.lower().endswith(EXTENSIONS) and (not pattern or fnmatch.fnmatch(n.upper(), pattern.upper())):
                    yield os.path.join(dirpath, n)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--glob", help="only file names matching this pattern, e.g. 'BIOA_WAR*'")
    ap.add_argument("-v", "--verbose", action="store_true", help="also list good files")
    ap.add_argument("--json", action="store_true", help="print one JSON object per file")
    a = ap.parse_args()
    total = bad = 0
    per_dir = {}
    by_guid = {}
    for path in walk(a.paths, a.glob):
        status, problems, info = check_file(path)
        total += 1
        d = per_dir.setdefault(os.path.dirname(path), [0, 0])
        d[0] += 1
        if "guid" in info:
            by_guid.setdefault((os.path.dirname(os.path.dirname(path)), info["guid"]), []).append(path)
        if status == "bad":
            bad += 1
            d[1] += 1
        if a.json:
            print(json.dumps({"path": path, "status": status, "problems": problems, **info}))
        elif status == "bad" or a.verbose:
            print("%-4s %s (%d bytes)%s" % ("BAD" if status == "bad" else "ok", path, info["size"],
                                             "".join("\n       " + p for p in problems)))
    # The same package GUID under two different names marks a placeholder copy of another map (the RU
    # two-disc repack ships Feros' BIOA_WAR00 data as BIOA_LOS00 on one disc); the retail EN disc has none.
    dupes = [v for v in by_guid.values() if len({os.path.basename(p).lower() for p in v}) > 1]
    for v in dupes:
        bad += 1
        print("DUP  same package GUID under different names:" + "".join("\n       " + p for p in v))
    if not a.json:
        for d, (n, b) in sorted(per_dir.items()):
            print("  %4d bad / %4d  %s" % (b, n, d))
        print("%d packages checked, %d bad (incl. %d duplicate-GUID groups)" % (total, bad, len(dupes)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
