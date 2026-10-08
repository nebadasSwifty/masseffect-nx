#!/usr/bin/env python3
"""Inspect and extract Xbox 360 STFS (CON / LIVE / PIRS) content packages.

Port of the block math in sdk/src/filesystem/devices/stfs_container_device.cpp
(Xenia's StfsContainerDevice). Read-only: the package is never modified.

Usage:
  stfs_extract.py info    PACKAGE [PACKAGE...]      header + file tree
  stfs_extract.py extract PACKAGE... --content-root DIR [--title 4D5307E8]

`extract` writes the layout the runtime ContentManager reads (xuid 0 = shared
content, which is where Marketplace content lives):

  DIR/0000000000000000/<TITLE>/<TYPE>/<PACKAGE FILE NAME>/<files...>
  DIR/0000000000000000/<TITLE>/Headers/<TYPE>/<PACKAGE FILE NAME>.header

The .header file is an XCONTENT_AGGREGATE_DATA (0x148 bytes, big-endian, the
same bytes the guest sees) followed by the 32-bit license mask, exactly what
ContentManager::WriteContentHeaderFile produces.

Never commit the output: it is copyrighted game content.
"""

import argparse
import hashlib
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = (0xAA, 0xAA * 0xAA, 0xAA * 0xAA * 0xAA)
END_OF_CHAIN = 0xFFFFFF

MAGICS = {b"CON ": "CON", b"LIVE": "LIVE", b"PIRS": "PIRS"}
CONTENT_TYPES = {
    0x00000001: "SavedGame",
    0x00000002: "MarketplaceContent",
    0x00000003: "Publisher",
    0x00001000: "IPTV",
    0x00002000: "InstalledGame",
    0x00004000: "GamesOnDemand",
    0x000B0000: "GameTitle",
    0x000D0000: "ArcadeTitle",
    0x00080000: "GameDemo",
    0x00090000: "Video",
    0x000F0000: "AvatarItem",
}
# XLanguage order of the 9 V1 display-name slots (XLanguage id - 1).
LANGS_V1 = ["en", "ja", "de", "fr", "es", "it", "ko", "zh", "pt"]
LANGS_V2 = ["pl", "ru", "sv"]  # slots 9..11 (metadata v2 only)

# Offsets inside the package header (see stfs_xbox.h: StfsHeader / XContentMetadata).
OFF_LICENSES = 0x22C
OFF_CONTENT_ID = 0x32C
OFF_HEADER_SIZE = 0x340
OFF_META = 0x344
OFF_CONTENT_TYPE = 0x344
OFF_META_VERSION = 0x348
OFF_CONTENT_SIZE = 0x34C
OFF_EXEC_INFO = 0x354  # media_id, version, base_version, title_id, platform, exe type, disc, discs, savegame_id
OFF_CONSOLE_ID = 0x36C
OFF_PROFILE_ID = 0x371
OFF_VOLUME = 0x379
OFF_DATA_FILE_COUNT = 0x39D
OFF_DATA_FILE_SIZE = 0x3A1
OFF_VOLUME_TYPE = 0x3A9
OFF_DEVICE_ID = 0x3FD
OFF_DISPLAY_NAME = 0x411
OFF_DESCRIPTION = 0xD11
OFF_PUBLISHER = 0x1611
OFF_TITLE_NAME = 0x1691
OFF_FLAGS = 0x1711
OFF_THUMB_SIZE = 0x1712
OFF_DISPLAY_NAME_EX = 0x541A
OFF_DESCRIPTION_EX = 0x941A


def u16str(raw):
    s = raw.decode("utf-16-be", errors="replace")
    return s.split("\0", 1)[0]


def u24le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


class Stfs:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        h = self.f.read(0xA000)
        self.raw_header = h
        self.magic = MAGICS.get(h[0:4])
        if not self.magic:
            raise ValueError(f"{path}: not an STFS package (magic {h[0:4]!r})")
        self.header_size = struct.unpack_from(">I", h, OFF_HEADER_SIZE)[0]
        self.content_type = struct.unpack_from(">I", h, OFF_CONTENT_TYPE)[0]
        self.meta_version = struct.unpack_from(">I", h, OFF_META_VERSION)[0]
        self.content_size = struct.unpack_from(">Q", h, OFF_CONTENT_SIZE)[0]
        (self.media_id, self.version, self.base_version, self.title_id) = struct.unpack_from(
            ">IIII", h, OFF_EXEC_INFO)
        self.content_id = h[OFF_CONTENT_ID:OFF_CONTENT_ID + 0x14]
        self.profile_id = struct.unpack_from(">Q", h, OFF_PROFILE_ID)[0]
        self.volume_type = struct.unpack_from(">I", h, OFF_VOLUME_TYPE)[0]
        self.data_file_count = struct.unpack_from(">I", h, OFF_DATA_FILE_COUNT)[0]
        self.licenses = []
        for i in range(0x10):
            lid, bits, flags = struct.unpack_from(">QII", h, OFF_LICENSES + i * 0x10)
            if lid or bits or flags:
                self.licenses.append((lid, bits, flags))
        self.display_names = {}
        self.descriptions = {}
        for i, lang in enumerate(LANGS_V1):
            self.display_names[lang] = u16str(h[OFF_DISPLAY_NAME + i * 0x100:OFF_DISPLAY_NAME + (i + 1) * 0x100])
            self.descriptions[lang] = u16str(h[OFF_DESCRIPTION + i * 0x100:OFF_DESCRIPTION + (i + 1) * 0x100])
        if self.meta_version >= 2:
            for i, lang in enumerate(LANGS_V2):
                self.display_names[lang] = u16str(
                    h[OFF_DISPLAY_NAME_EX + i * 0x100:OFF_DISPLAY_NAME_EX + (i + 1) * 0x100])
                self.descriptions[lang] = u16str(
                    h[OFF_DESCRIPTION_EX + i * 0x100:OFF_DESCRIPTION_EX + (i + 1) * 0x100])
        self.publisher = u16str(h[OFF_PUBLISHER:OFF_PUBLISHER + 0x80])
        self.title_name = u16str(h[OFF_TITLE_NAME:OFF_TITLE_NAME + 0x80])

        vd = h[OFF_VOLUME:OFF_VOLUME + 0x24]
        self.vd_len = vd[0]
        self.vd_flags = vd[2]
        self.read_only = bool(self.vd_flags & 1)
        self.root_active_index = bool(self.vd_flags & 2)
        self.ft_block_count = struct.unpack_from("<H", vd, 3)[0]
        self.ft_block = u24le(vd[5:8])
        self.total_blocks, self.free_blocks = struct.unpack_from(">II", vd, 0x1C)
        self.bpht = 1 if self.read_only else 2
        self.step0 = PER_LEVEL[0] + self.bpht
        self.step1 = PER_LEVEL[1] + (PER_LEVEL[0] + 1) * self.bpht
        self.base = (self.header_size + BLOCK - 1) & ~(BLOCK - 1)
        self._hash_cache = {}
        self.entries = []

    # --- block math (StfsContainerDevice::BlockToOffsetSTFS & co) ---
    def block_to_offset(self, index):
        base = PER_LEVEL[0]
        block = index
        for _ in range(3):
            block += ((index + base) // base) * self.bpht
            if index < base:
                break
            base *= PER_LEVEL[0]
        return self.base + (block << 12)

    def hash_block_number(self, index, level):
        if level == 0:
            if index < PER_LEVEL[0]:
                return 0
            block = (index // PER_LEVEL[0]) * self.step0
            block += ((index // PER_LEVEL[1]) + 1) * self.bpht
            if index < PER_LEVEL[1]:
                return block
            return block + self.bpht
        if level == 1:
            if index < PER_LEVEL[1]:
                return self.step0
            return (index // PER_LEVEL[1]) * self.step1 + self.bpht
        return self.step1

    def _table(self, offset):
        t = self._hash_cache.get(offset)
        if t is None:
            self.f.seek(offset)
            t = self.f.read(BLOCK)
            self._hash_cache[offset] = t
        return t

    def block_hash(self, index):
        """Returns (sha1, info) of the level-0 hash entry for a data block."""
        sec = BLOCK if self.root_active_index else 0
        lv0 = self.base + (self.hash_block_number(index, 0) << 12)
        if not self.read_only:
            if self.total_blocks > PER_LEVEL[0]:
                lv1 = self.base + (self.hash_block_number(index, 1) << 12)
                if self.total_blocks > PER_LEVEL[1]:
                    lv2 = self.base + (self.hash_block_number(index, 2) << 12)
                    t2 = self._table(lv2 + sec)
                    rec = (index // PER_LEVEL[1]) % PER_LEVEL[0]
                    info = struct.unpack_from(">I", t2, rec * 0x18 + 0x14)[0]
                    sec = BLOCK if info & 0x40000000 else 0
                t1 = self._table(lv1 + sec)
                rec = (index // PER_LEVEL[0]) % PER_LEVEL[0]
                info = struct.unpack_from(">I", t1, rec * 0x18 + 0x14)[0]
                sec = BLOCK if info & 0x40000000 else 0
        else:
            sec = 0
        t0 = self._table(lv0 + sec)
        rec = index % PER_LEVEL[0]
        return t0[rec * 0x18:rec * 0x18 + 0x14], struct.unpack_from(">I", t0, rec * 0x18 + 0x14)[0]

    def next_block(self, index):
        return self.block_hash(index)[1] & 0xFFFFFF

    # --- file table ---
    def read_tree(self):
        if self.volume_type != 0:
            raise ValueError("SVOD packages are not supported by this tool")
        entries = []
        block = self.ft_block
        for _ in range(self.ft_block_count):
            self.f.seek(self.block_to_offset(block))
            data = self.f.read(BLOCK)
            done = False
            for m in range(0x40):
                e = data[m * 0x40:(m + 1) * 0x40]
                if e[0] == 0:
                    done = True
                    break
                flags = e[0x28]
                name = e[:flags & 0x3F].decode("latin-1")
                valid_blocks = u24le(e[0x29:0x2C])
                alloc_blocks = u24le(e[0x2C:0x2F])
                start = u24le(e[0x2F:0x32])
                parent = struct.unpack_from(">H", e, 0x32)[0]
                length = struct.unpack_from(">I", e, 0x34)[0]
                entries.append({
                    "name": name,
                    "dir": bool(flags & 0x80),
                    "contiguous": bool(flags & 0x40),
                    "valid_blocks": valid_blocks,
                    "alloc_blocks": alloc_blocks,
                    "start": start,
                    "parent": parent,
                    "length": length,
                })
            if done:
                break
            block = self.next_block(block)
            if block == END_OF_CHAIN:
                break
        for e in entries:
            parts = [e["name"]]
            p = e["parent"]
            guard = 0
            while p != 0xFFFF and guard < 64:
                parts.append(entries[p]["name"])
                p = entries[p]["parent"]
                guard += 1
            e["path"] = "/".join(reversed(parts))
        self.entries = entries
        return entries

    def iter_file_blocks(self, e, verify=False):
        """Yields (offset, size) runs for a file entry, following the hash chain."""
        remaining = e["length"]
        block = e["start"]
        count = 0
        while remaining and block != END_OF_CHAIN:
            size = min(BLOCK, remaining)
            off = self.block_to_offset(block)
            if verify:
                sha, _ = self.block_hash(block)
                self.f.seek(off)
                if hashlib.sha1(self.f.read(BLOCK)).digest() != sha:
                    raise ValueError(f"{e['path']}: SHA-1 mismatch in block {block}")
            yield off, size
            remaining -= size
            count += 1
            if e["contiguous"] and not verify:
                block += 1
            else:
                block = self.next_block(block)
        if remaining:
            raise ValueError(f"{e['path']}: chain ended with {remaining} bytes missing")

    def extract_file(self, e, out_path, verify=False):
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as o:
            for off, size in self.iter_file_blocks(e, verify):
                self.f.seek(off)
                o.write(self.f.read(size))

    def license_mask(self):
        # Same rule as ContentManager::InstallContent.
        mask = 0
        for _, bits, flags in self.licenses:
            if flags:
                mask |= bits
        return mask

    def aggregate_data(self, file_name, device_id=1):
        """XCONTENT_AGGREGATE_DATA (0x148 bytes, big-endian) for the .header file."""
        b = bytearray(0x148)
        struct.pack_into(">II", b, 0, device_id, self.content_type)
        name = (self.display_names.get("en") or file_name)[:127]
        enc = name.encode("utf-16-be")
        b[8:8 + len(enc)] = enc
        fn = file_name.encode("ascii")[:42]
        b[0x108:0x108 + len(fn)] = fn
        # be<uint64_t> xuid is 8-aligned: 0x134 -> 0x138; title_id at 0x140.
        struct.pack_into(">QI", b, 0x138, 0, self.title_id)
        return bytes(b)


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def cmd_info(args):
    for path in args.packages:
        s = Stfs(path)
        print(f"== {path}")
        print(f"  magic {s.magic}  header_size 0x{s.header_size:X}  metadata v{s.meta_version}")
        print(f"  title {s.title_id:08X}  content type {s.content_type:08X} "
              f"({CONTENT_TYPES.get(s.content_type, '?')})  media id {s.media_id:08X}")
        print(f"  content id {s.content_id.hex().upper()}  profile {s.profile_id:016X}")
        print(f"  content size {s.content_size} ({human(s.content_size)})  file size {s.size}")
        print(f"  title name '{s.title_name}'  publisher '{s.publisher}'")
        for lang, name in s.display_names.items():
            if name:
                print(f"  name[{lang}] '{name}'")
        for lang, d in s.descriptions.items():
            if d:
                print(f"  desc[{lang}] '{d[:160]}'")
        for lid, bits, flags in s.licenses:
            print(f"  license id {lid:016X} bits {bits:08X} flags {flags:08X}")
        print(f"  license mask (installer rule) {s.license_mask():08X}")
        print(f"  volume: {'STFS' if s.volume_type == 0 else 'SVOD'} read_only={s.read_only} "
              f"file table block {s.ft_block} x{s.ft_block_count} total blocks {s.total_blocks} "
              f"free {s.free_blocks}")
        if s.volume_type != 0:
            continue
        entries = s.read_tree()
        files = [e for e in entries if not e["dir"]]
        print(f"  {len(entries)} entries, {len(files)} files, "
              f"{human(sum(e['length'] for e in files))}")
        for e in entries:
            kind = "d" if e["dir"] else ("c" if e["contiguous"] else "f")
            print(f"    {kind} {e['length']:>11}  {e['path']}")


def cmd_extract(args):
    for path in args.packages:
        s = Stfs(path)
        title = int(args.title, 16) if args.title else s.title_id
        file_name = os.path.basename(path)
        ctype = f"{s.content_type:08X}"
        root = os.path.join(args.content_root, "0000000000000000", f"{title:08X}")
        dest = os.path.join(root, ctype, file_name)
        hdr = os.path.join(root, "Headers", ctype, file_name + ".header")
        entries = s.read_tree()
        print(f"{file_name}: '{s.display_names.get('en', '')}' -> {dest}")
        total = 0
        for e in entries:
            out = os.path.join(dest, *e["path"].split("/"))
            if e["dir"]:
                os.makedirs(out, exist_ok=True)
                continue
            s.extract_file(e, out, verify=args.verify)
            total += e["length"]
        os.makedirs(os.path.dirname(hdr), exist_ok=True)
        with open(hdr, "wb") as o:
            o.write(s.aggregate_data(file_name))
            # Native-endian u32, as ContentManager writes it with fwrite (little-endian on
            # both the Switch and the Mac host).
            o.write(struct.pack("<I", args.license_mask if args.license_mask is not None
                                else 0xFFFFFFFF))
        print(f"  {len(entries)} entries, {human(total)}; header {hdr}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("packages", nargs="+")
    p.set_defaults(func=cmd_info)
    p = sub.add_parser("extract")
    p.add_argument("packages", nargs="+")
    p.add_argument("--content-root", required=True,
                   help="the runtime content root (the folder that holds 0000000000000000/)")
    p.add_argument("--title", help="override the title id folder (hex)")
    p.add_argument("--license-mask", type=lambda v: int(v, 0),
                   help="license mask stored in the .header (default 0xFFFFFFFF = every "
                        "license bit granted; the package's own bits are shown by `info`)")
    p.add_argument("--verify", action="store_true", help="check every block's SHA-1")
    p.set_defaults(func=cmd_extract)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
