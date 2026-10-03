#!/usr/bin/env python3
"""tools/extract_iso.py against small synthetic XDVDFS images (no game data involved)."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import extract_iso as x

SECTOR = 2048


def pad4(n):
    return (n + 3) & ~3


class ExtractIsoTests(unittest.TestCase):
    TREE = {"default.xex": b"XEX2" + b"\0" * 100, "readme.txt": b"hello", "Data": {"a.bin": os.urandom(5000),
            "Deep": {"b.bin": b"b" * 4096}}, "empty.bin": b""}

    def _roundtrip(self, base):
        image = make_image(self.TREE, base)
        with tempfile.TemporaryDirectory() as tmp:
            iso = os.path.join(tmp, "game.iso")
            with open(iso, "wb") as fh:
                fh.write(image)
            out = os.path.join(tmp, "out")
            with contextlib.redirect_stdout(io.StringIO()):
                x.extract_image(iso, out)
            self._compare(self.TREE, out)

    def _compare(self, tree, folder):
        for name, value in tree.items():
            path = os.path.join(folder, name)
            if isinstance(value, dict):
                self.assertTrue(os.path.isdir(path), path)
                self._compare(value, path)
            else:
                with open(path, "rb") as fh:
                    self.assertEqual(fh.read(), value, path)

    def test_raw_partition(self):
        self._roundtrip(0)

    def test_partition_found_by_scanning(self):
        self._roundtrip(0x40000)

    def test_unsafe_names_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            for bad in ("../evil", "a/../../evil", "a\\b", "C:x", ""):
                with self.assertRaises(x.ExtractError):
                    x._safe_destination(tmp, bad)
            self.assertTrue(x._safe_destination(tmp, "Data/a.bin").startswith(tmp))

    def test_not_an_xdvdfs_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            iso = os.path.join(tmp, "bad.iso")
            with open(iso, "wb") as fh:
                fh.write(b"\0" * (64 * SECTOR))
            with self.assertRaises(x.ExtractError):
                x.extract_image(iso, os.path.join(tmp, "out"))


def make_rooted_table(entries):
    """Directory table whose first entry (offset 0) is the root of the search tree, as on real discs."""
    entries = sorted(entries, key=lambda e: e[0].lower())

    def split(lst):
        if not lst:
            return None
        mid = len(lst) // 2
        return (lst[mid], split(lst[:mid]), split(lst[mid + 1:]))

    tree = split(entries)
    order = []

    def preorder(node):
        if node:
            order.append(node)
            preorder(node[1])
            preorder(node[2])

    preorder(tree)
    offsets, cursor = {}, 0
    for node in order:
        offsets[node[0][0]] = cursor
        cursor += pad4(14 + len(node[0][0]))
    table = bytearray(b"\xff" * max(cursor, 14))
    for (name, sector, size, is_dir), l, r in order:
        left = offsets[l[0][0]] // 4 if l else 0
        right = offsets[r[0][0]] // 4 if r else 0
        off = offsets[name]
        struct.pack_into("<HHIIBB", table, off, left, right, sector, size, 0x10 if is_dir else 0x20, len(name))
        table[off + 14:off + 14 + len(name)] = name.encode("latin-1")
    return bytes(table)


def make_image(tree, base=0):
    sectors, nxt = {}, [34]

    def alloc(data):
        n = nxt[0]
        sectors[n] = data
        nxt[0] += max(1, (len(data) + SECTOR - 1) // SECTOR)
        return n

    def build_dir(node):
        entries = []
        for name, value in node.items():
            if isinstance(value, dict):
                s, z = build_dir(value)
                entries.append((name, s, z, True))
            else:
                entries.append((name, alloc(value) if value else 0, len(value), False))
        table = make_rooted_table(entries)
        return alloc(table), len(table)

    root_sector, root_size = build_dir(tree)
    image = bytearray(base + nxt[0] * SECTOR)
    vd = bytearray(SECTOR)
    vd[:20] = x.XDVDFS_MAGIC
    struct.pack_into("<II", vd, 0x14, root_sector, root_size)
    vd[0x7EC:0x7EC + 20] = x.XDVDFS_MAGIC
    image[base + 32 * SECTOR:base + 33 * SECTOR] = vd
    for n, data in sectors.items():
        image[base + n * SECTOR:base + n * SECTOR + len(data)] = data
    return bytes(image)


if __name__ == "__main__":
    unittest.main()
