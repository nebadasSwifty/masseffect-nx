#!/usr/bin/env python3
"""Builds a full, installable NSP of the port: the program plus the user's own game data, from an installer output.

    python3 tools/build_full_nsp.py --input <folder> --keys <prod.keys> --output masseffect-nx-full.nsp

<folder> is what the installer zip extracts to (masseffect-nx/): masseffect-nx.nro, masseffect.toml,
masseffect_shaders.mesp(.idx), game_root/, optionally masseffect_prewarm_list.bin (the edition's shipped pipeline
prewarm list) and masseffect/0000000000000000/ (DLC). An SD install folder works
too: only those entries are taken (saves, cache/, logs/ are left out).

Result: one application (title ID per game edition, --edition: ru 01a5eec700010000, en 01a5eec700020000; next to the
forwarder's 01a5eec700000000), made of
  * a Program NCA: ExeFS = main (NSO converted from the NRO) + main.npdm (39-bit address space, full application
    memory, the same permissions as the forwarder's hbloader), RomFS = game_root/, shader package, masseffect.toml,
    the prewarm list when present, DLC and the marker file masseffect-nx-package.txt that switches the program to packaged mode
    (app/src/me_packaged.cpp),
  * a Control NCA: control.nacp + icon (taken from the NRO, patched like tools/build_nsp.sh),
  * a Meta NCA: the Application content meta (CNMT),
inside a PFS0 container (.nsp). Installing it needs the usual signature patches (same as the forwarder).

Keys: only header_key and key_area_key_application_00 are read from the given prod.keys. They are never printed,
stored or copied. Nothing is downloaded. Formats and the design are in docs/full-nsp.md.

The game data is read twice (pass 1 hashes the RomFS, pass 2 encrypts and writes) and the NSP is written once, then
its header is patched (the NCA names are their SHA-256). Peak memory is a few tens of MB.
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import shutil
import struct
import sys
import time

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding, rsa
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:  # pragma: no cover
    sys.exit("error: the Python package 'cryptography' is required (python3 -m pip install cryptography)")

DEFAULT_TITLE_ID = 0x01A5EEC700010000
FORWARDER_TITLE_ID = 0x01A5EEC700000000
DEFAULT_DATA_DIR = "sdmc:/switch/masseffect-nx-nsp"
# Per game edition: the title ID and the writable SD folder, so the editions install side by side (docs/full-nsp.md;
# the same values as `nsp` of each edition in installer/config.js). Without --edition the defaults above apply.
EDITIONS = {
    "ru": {"title_id": 0x01A5EEC700010000, "data_dir": "sdmc:/switch/masseffect-nx"},
    "en": {"title_id": 0x01A5EEC700020000, "data_dir": "sdmc:/switch/masseffect-nx-en"},
}
MARKER_NAME = "masseffect-nx-package.txt"  # app/src/me_packaged.cpp kMarker
NRO_NAME = "masseffect-nx.nro"
TOML_NAME = "masseffect.toml"
SHADERS_NAME = "masseffect_shaders.mesp"
# Optional: the edition's shipped pipeline prewarm list (masseffect_native_pipelines_shipped_list in masseffect.toml,
# docs/cold-start-hitches.md C). Packaged mode reads it from the SD data folder first, then from the RomFS.
PREWARM_LIST_NAME = "masseffect_prewarm_list.bin"
DLC_DIR = ("masseffect", "0000000000000000")

MEDIA_UNIT = 0x200
IVFC_BLOCK_LOG2 = 14
IVFC_BLOCK = 1 << IVFC_BLOCK_LOG2  # 0x4000
IVFC_LEVELS = 6  # hash levels 1..5 + the data
EXEFS_HASH_BLOCK = 0x10000
META_HASH_BLOCK = 0x1000
SDK_VERSION = 0x000C1100
FAT32_PART = 0xFFFF0000  # part size of a split NSP folder (the convention DBI/Tinfoil/Goldleaf read)
CHUNK = 8 << 20

CONTENT_PROGRAM, CONTENT_META, CONTENT_CONTROL = 0, 1, 2  # NCA header content types
CNMT_PROGRAM, CNMT_CONTROL = 1, 3  # CNMT content record types


class PackError(Exception):
    pass


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def sha256(data):
    return hashlib.sha256(data).digest()


# ---- keys ----------------------------------------------------------------------------------------------------------

def load_keys(path):
    """Reads header_key and key_area_key_application_00 from a prod.keys file. Values are never printed."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        raise PackError(f"cannot read the key file {path}: {e.strerror}") from None
    found = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        name, value = (part.strip() for part in line.split("=", 1))
        found[name.lower()] = value
    keys = {}
    for name, size in (("header_key", 32), ("key_area_key_application_00", 16)):
        value = found.get(name, "")
        try:
            raw = bytes.fromhex(value)
        except ValueError:
            raw = b""
        if len(raw) != size:
            raise PackError(f"{path}: {name} is missing or malformed (expected {size * 2} hex digits)")
        keys[name] = raw
    if keys["header_key"][:16] == keys["header_key"][16:]:
        raise PackError(f"{path}: header_key is malformed (both halves are equal)")
    return keys


# ---- NRO -> NSO, NACP, icon ------------------------------------------------------------------------------------------

class Nro:
    def __init__(self, data):
        if len(data) < 0x80 or data[0x10:0x14] != b"NRO0":
            raise PackError("not an NRO (no NRO0 header)")
        self.data = data
        self.size = struct.unpack_from("<I", data, 0x18)[0]
        self.segments = [struct.unpack_from("<II", data, 0x20 + 8 * i) for i in range(3)]  # (offset, size)
        self.bss = struct.unpack_from("<I", data, 0x38)[0]
        self.build_id = bytes(data[0x40:0x60])
        for off, size in self.segments:
            if off % 0x1000 or off + size > len(data):
                raise PackError("NRO segments are outside the file or not page aligned")
        if self.segments[0][0] != 0:
            raise PackError("NRO text segment does not start at 0")
        self.assets = {}
        if self.size + 0x38 <= len(data) and data[self.size:self.size + 4] == b"ASET":
            for name, pos in (("icon", 8), ("nacp", 24), ("romfs", 40)):
                off, size = struct.unpack_from("<QQ", data, self.size + pos)
                if size:
                    start = self.size + off
                    if start + size > len(data):
                        raise PackError("NRO asset section is truncated")
                    self.assets[name] = bytes(data[start:start + size])


def nso_from_nro(nro):
    """An uncompressed NSO with the NRO's segments (what elf2nso makes from the ELF, minus LZ4).

    The NRO header lives in text[0x10:0x80], a gap libnx's crt0 leaves empty (.org _start+0x80); it is cleared so
    the text is byte-identical to the ELF's. The segments keep the NRO's page-aligned sizes (the tail is zeros)."""
    segs = []
    for i, (off, size) in enumerate(nro.segments):
        seg = bytearray(nro.data[off:off + size])
        if i == 0:
            if seg[0x10:0x14] != b"NRO0":
                raise PackError("unexpected NRO layout (header not at text+0x10)")
            seg[0x10:0x80] = bytes(0x70)
        segs.append(bytes(seg))
    header = bytearray(0x100)
    header[0:4] = b"NSO0"
    struct.pack_into("<I", header, 0x0C, 0x38)  # flags: no compression, check the three segment hashes
    file_off = 0x100
    for i, ((mem_off, _), seg) in enumerate(zip(nro.segments, segs)):
        # Segment header: file offset, memory offset, size, then (text, ro) the module name offset/size as elf2nso
        # writes them (1), (data) the bss size.
        extra = nro.bss if i == 2 else 1
        struct.pack_into("<IIII", header, 0x10 + 0x10 * i, file_off, mem_off, len(seg), extra)
        struct.pack_into("<I", header, 0x60 + 4 * i, len(seg))  # stored size (= size: not compressed)
        header[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = sha256(seg)
        file_off += len(seg)
    header[0x40:0x60] = nro.build_id
    return bytes(header) + b"".join(segs)


# NACP fields that carry the application's own ID (nacptool --titleid fills them; the port's NROs leave them 0):
# PresenceGroupId, SaveDataOwnerId, LocalCommunicationId[8], SeedForPseudoDeviceId; AddOnContentBaseId is ID + 0x1000.
NACP_ID_FIELDS = (0x3038, 0x3078) + tuple(range(0x30B0, 0x30F0, 8)) + (0x30F8,)
NACP_AOC_BASE = 0x3070


def patch_nacp(nacp, name=None, display_version=None, title_id=None):
    """The NRO's NACP patched like tools/build_nsp.sh: no account selection at start, screenshots and video on,
    no account save data (the game saves on the SD). Optionally a new title name in every language. With title_id,
    the ID fields the NRO set (non-zero) are rewritten to this title, so they never name another edition."""
    if len(nacp) != 0x4000:
        raise PackError("the NRO's NACP is not 0x4000 bytes")
    nacp = bytearray(nacp)
    if title_id is not None:
        for off in NACP_ID_FIELDS:
            if struct.unpack_from("<Q", nacp, off)[0]:
                struct.pack_into("<Q", nacp, off, title_id)
        if struct.unpack_from("<Q", nacp, NACP_AOC_BASE)[0]:
            struct.pack_into("<Q", nacp, NACP_AOC_BASE, title_id + 0x1000)
    nacp[0x3025] = 0  # StartupUserAccount: none
    nacp[0x3034] = 0  # Screenshot: allowed
    nacp[0x3035] = 2  # VideoCapture: enabled
    nacp[0x3080:0x3090] = bytes(16)  # UserAccountSaveDataSize / JournalSize
    if display_version:
        raw = display_version.encode("ascii")[:0xF]
        nacp[0x3060:0x3070] = raw + bytes(0x10 - len(raw))
    if name:
        raw = name.encode("utf-8")[:0x1FF]
        for lang in range(16):
            entry = lang * 0x300
            nacp[entry:entry + 0x200] = raw + bytes(0x200 - len(raw))
    return bytes(nacp)


# ---- NPDM (port of switch-tools npdmtool, ISC license, for the fields used here) ------------------------------------

def npdm_config(title_id):
    """The forwarder's hbloader configuration (Forwarder-Mod hbl.json, nx-hbloader, ISC license) as tools/build_nsp.sh
    patches it: the NRO runs today inside exactly this process, so the NSO gets the same permissions."""
    return {
        "name": "Mass Effect",
        "program_id": title_id,
        "main_thread_stack_size": 0x100000,
        "main_thread_priority": 44,
        "default_cpu_id": 0,
        "version": 0,  # process_category
        "address_space_type": 3,  # AddressSpace64Bit (39 bits); 1 is 36 bits
        "is_64_bit": True,
        "is_retail": True,
        "pool_partition": 0,  # application pool: full application memory
        "fs_permissions": 0xFFFFFFFFFFFFFFFF,
        "content_owner_ids": [0x0100000000001000],
        "service_host": ["*"],
        "service_access": ["*"],
        "kernel_flags": (59, 28, 2, 0),  # highest/lowest thread priority, highest/lowest cpu id
        "syscalls": list(range(0xC0)),
        "application_type": 1,
        "min_kernel_version": 0x30,
        "handle_table_size": 512,
        "debug_flags": (False, False, True),  # allow_debug, force_debug_prod, force_debug
        "legacy_debug_flags": False,  # True: pre-19.0.0 DebugFlags layout (see npdm_emulator_compatible)
        "map_regions": [(1, True)],  # (region type, read-only)
    }


def npdm_emulator_compatible(cfg):
    """The same NPDM for yuzu-based emulators (Eden v0.2.1 and older, yuzu): only the DebugFlags capability changes.

    Since 19.0.0 the DebugFlags capability is allow_debug (bit 17), force_debug_prod (bit 18), force_debug (bit 19);
    npdmtool and this packer write force_debug to bit 19, which real hardware (and Atmosphere) needs. yuzu's
    KCapabilities still has the old layout, allow_debug (17), force_debug (18), reserved (19..31), and
    SetDebugFlagsCapability returns ResultReservedUsed when a reserved bit is set; KProcess::LoadFromMetadata then
    fails and the loader reports ErrorUnableToParseKernelMetadata ("Unable to completely parse the kernel metadata
    when loading the emulated process"). Eden dropped that check after v0.2.1 (commit 300a646a34). The emulator
    variant writes force_debug in the old layout (bit 18); every other field, including the 39-bit address space,
    the application pool and the syscall masks, stays as it is."""
    cfg = dict(cfg)
    cfg["legacy_debug_flags"] = True
    return cfg


def build_npdm(cfg, acid_modulus=bytes(0x100)):
    # Kernel capabilities, in npdmtool's order for hbl.json: kernel_flags, syscalls, application_type,
    # min_kernel_version, handle_table_size, debug_flags, map_region.
    caps = []
    hi_prio, lo_prio, hi_cpu, lo_cpu = cfg["kernel_flags"]
    real_hi, real_lo = min(hi_prio, lo_prio), max(hi_prio, lo_prio)
    desc = (((hi_cpu << 8 | lo_cpu) << 6 | (real_hi & 0x3F)) << 6) | (real_lo & 0x3F)
    caps.append((desc << 4) | 0x7)
    descriptors = [0] * 8
    for sc in cfg["syscalls"]:
        descriptors[sc // 0x18] |= 1 << (sc % 0x18)
    for i, d in enumerate(descriptors):
        if d:
            caps.append(((d | (i << 24)) << 5) | 0xF)
    caps.append(((cfg["application_type"] & 7) << 14) | 0x1FFF)
    caps.append(((cfg["min_kernel_version"] & 0xFFFF) << 15) | 0x3FFF)
    caps.append((cfg["handle_table_size"] << 16) | 0x7FFF)
    allow, force_prod, force = cfg["debug_flags"]
    if cfg.get("legacy_debug_flags"):
        # Pre-19.0.0 layout: no force_debug_prod, force_debug in bit 18.
        caps.append(((int(allow) | int(force or force_prod) << 1) << 17) | 0xFFFF)
    else:
        caps.append(((int(allow) | int(force_prod) << 1 | int(force) << 2) << 17) | 0xFFFF)
    regions = cfg["map_regions"] + [(0, False)] * (3 - len(cfg["map_regions"]))
    cap = 0x3FF
    for i, (rtype, ro) in enumerate(regions[:3]):
        cap |= ((rtype & 0x3F) | (int(ro) << 6)) << (11 + 7 * i)
    caps.append(cap)
    kac = struct.pack(f"<{len(caps)}I", *(c & 0xFFFFFFFF for c in caps))

    sac = b""
    for name in cfg["service_host"]:
        sac += bytes([(len(name) - 1) | 0x80]) + name.encode()
    for name in cfg["service_access"]:
        sac += bytes([len(name) - 1]) + name.encode()

    # ACI0 (0x40 header, FAH, SAC, KAC)
    cois = cfg["content_owner_ids"]
    coi = struct.pack("<I", len(cois)) + b"".join(struct.pack("<Q", c) for c in cois) if cois else b""
    fah = struct.pack("<IQIIII", 1, cfg["fs_permissions"], 0x1C, len(coi), 0x1C + len(coi), 0) + coi
    aci0_sac_off = align(0x40 + len(fah), 0x10)
    aci0_kac_off = align(aci0_sac_off + len(sac), 0x10)
    aci0 = bytearray(aci0_kac_off + len(kac))
    struct.pack_into("<4s12xQ8xIIIIII8x", aci0, 0, b"ACI0", cfg["program_id"], 0x40, len(fah),
                     aci0_sac_off, len(sac), aci0_kac_off, len(kac))
    aci0[0x40:0x40 + len(fah)] = fah
    aci0[aci0_sac_off:aci0_sac_off + len(sac)] = sac
    aci0[aci0_kac_off:] = kac

    # ACID (signature, modulus, 0x40 header, FAC, SAC, KAC)
    fac = struct.pack("<BBBxQQQQQ", 1, 0, 0, cfg["fs_permissions"], 0, 0, 0, 0)
    acid_sac_off = align(0x240 + len(fac), 0x10)
    acid_kac_off = align(acid_sac_off + len(sac), 0x10)
    acid = bytearray(acid_kac_off + len(kac))
    acid[0x100:0x200] = acid_modulus
    flags = int(cfg["is_retail"]) | (cfg["pool_partition"] & 3) << 2
    struct.pack_into("<4sIIIQQIIIIII8x", acid, 0x200, b"ACID", len(acid) - 0x100, 0, flags,
                     cfg["program_id"], cfg["program_id"], 0x240, len(fac), acid_sac_off, len(sac),
                     acid_kac_off, len(kac))
    acid[0x240:0x240 + len(fac)] = fac
    acid[acid_sac_off:acid_sac_off + len(sac)] = sac
    acid[acid_kac_off:] = kac

    header = bytearray(0x80)
    mmu = ((cfg["address_space_type"] & 3) << 1) | int(cfg["is_64_bit"])
    name = cfg["name"].encode()[:0xF]
    struct.pack_into("<4sII BBBB II II 16s 16s 48x IIII", header, 0, b"META", 0, 0, mmu, 0,
                     cfg["main_thread_priority"], cfg["default_cpu_id"], 0, 0, cfg["version"],
                     cfg["main_thread_stack_size"], name, b"", 0, 0, 0, 0)
    acid_off = 0x80
    aci0_off = align(acid_off + len(acid), 0x10)
    struct.pack_into("<IIII", header, 0x70, aci0_off, len(aci0), acid_off, len(acid))
    out = bytearray(aci0_off + len(aci0))
    out[:0x80] = header
    out[acid_off:acid_off + len(acid)] = acid
    out[aci0_off:] = aci0
    return bytes(out)


# ---- PFS0 -------------------------------------------------------------------------------------------------------------

def pfs0_header(entries, header_align=0x20):
    """entries: [(name, size)]. Returns the header (string table zero-padded so the header is aligned)."""
    names = b""
    offsets = []
    for name, _ in entries:
        offsets.append(len(names))
        names += name.encode() + b"\0"
    raw = 0x10 + 0x18 * len(entries) + len(names)
    names += bytes(align(raw, header_align) - raw)
    out = bytearray(struct.pack("<4sIII", b"PFS0", len(entries), len(names), 0))
    data_off = 0
    for (name, size), str_off in zip(entries, offsets):
        out += struct.pack("<QQII", data_off, size, str_off, 0)
        data_off += size
    return bytes(out) + names


def pfs0_bytes(files):
    """files: [(name, bytes)] -> a complete in-memory PFS0."""
    return pfs0_header([(n, len(d)) for n, d in files]) + b"".join(d for _, d in files)


# ---- RomFS (the layout of switch-tools build_romfs, which libnx's romfs device reads) ----------------------------------

ROMFS_EMPTY = 0xFFFFFFFF
ROMFS_DATA_OFFSET = 0x200


def romfs_hash(parent, name):
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def romfs_table_count(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


class RomfsSource:
    """A file of the image: data from a host path or from bytes."""

    def __init__(self, size, path=None, data=None):
        self.size, self.path, self.data = size, path, data

    def chunks(self):
        if self.data is not None:
            yield self.data
            return
        left = self.size
        with open(self.path, "rb") as fh:
            while left:
                block = fh.read(min(CHUNK, left))
                if not block:
                    raise PackError(f"{self.path} became shorter while packing")
                left -= len(block)
                yield block


class _Dir:
    def __init__(self, name, parent):
        self.name, self.parent = name, parent
        self.dirs, self.files = {}, {}
        self.offset = 0


class Romfs:
    def __init__(self):
        self.root = _Dir(b"", None)
        self.count = 0
        self.data_bytes = 0

    def add(self, path, source):
        parts = [p.encode("utf-8") for p in path.split("/") if p]
        if not parts:
            raise PackError(f"bad RomFS path {path!r}")
        node = self.root
        for part in parts[:-1]:
            if part in node.files:
                raise PackError(f"RomFS path conflict at {path!r}")
            node = node.dirs.setdefault(part, _Dir(part, node))
        if parts[-1] in node.files or parts[-1] in node.dirs:
            raise PackError(f"duplicate RomFS path {path!r}")
        node.files[parts[-1]] = source
        self.count += 1
        self.data_bytes += source.size

    def layout(self):
        """Fixes the image layout: returns (image size, [(data offset, source)], metadata bytes, metadata offset)."""
        dirs = []

        def visit(d):
            dirs.append(d)
            for name in sorted(d.dirs):
                visit(d.dirs[name])

        visit(self.root)
        off = 0
        for d in dirs:
            d.offset = off
            off += 0x18 + align(len(d.name), 4)
        dir_table_size = off
        files = []  # (dir, name, source, entry offset, data offset)
        entry_off = 0
        data_off = 0
        for d in dirs:
            for name in sorted(d.files):
                src = d.files[name]
                data_off = align(data_off, 0x10)
                files.append([d, name, src, entry_off, data_off])
                entry_off += 0x20 + align(len(name), 4)
                data_off += src.size
        file_table_size = entry_off
        data_size = data_off

        dir_count, file_count = romfs_table_count(len(dirs)), romfs_table_count(len(files))
        dir_hash = [ROMFS_EMPTY] * dir_count
        file_hash = [ROMFS_EMPTY] * file_count
        dir_table = bytearray(dir_table_size)
        file_table = bytearray(file_table_size)

        file_entry_of = {}
        first_file = {}
        for i, (d, name, src, eoff, doff) in enumerate(files):
            file_entry_of[(id(d), name)] = eoff
            first_file.setdefault(id(d), eoff)
        for d in dirs:
            names = sorted(d.files)
            for i, name in enumerate(names):
                eoff = file_entry_of[(id(d), name)]
                sibling = file_entry_of[(id(d), names[i + 1])] if i + 1 < len(names) else ROMFS_EMPTY
                src = d.files[name]
                h = romfs_hash(d.offset, name) % file_count
                struct.pack_into("<IIQQII", file_table, eoff, d.offset, sibling, 0, src.size, file_hash[h], len(name))
                file_hash[h] = eoff
                file_table[eoff + 0x20:eoff + 0x20 + len(name)] = name
        for d, name, src, eoff, doff in files:
            struct.pack_into("<Q", file_table, eoff + 8, doff)
        for d in dirs:
            children = sorted(d.dirs)
            child = d.dirs[children[0]].offset if children else ROMFS_EMPTY
            if d.parent is None:
                parent, sibling = 0, ROMFS_EMPTY
            else:
                parent = d.parent.offset
                peers = sorted(d.parent.dirs)
                i = peers.index(d.name)
                sibling = d.parent.dirs[peers[i + 1]].offset if i + 1 < len(peers) else ROMFS_EMPTY
            h = romfs_hash(parent if d.parent is not None else 0, d.name) % dir_count
            struct.pack_into("<IIIIII", dir_table, d.offset, parent, sibling, child,
                             first_file.get(id(d), ROMFS_EMPTY), dir_hash[h], len(d.name))
            dir_hash[h] = d.offset
            dir_table[d.offset + 0x18:d.offset + 0x18 + len(d.name)] = d.name

        # 16 (not 4): every region of the image then starts 16-aligned, which patch (BKTR) entries need.
        meta_off = align(ROMFS_DATA_OFFSET + data_size, 0x10)
        dh = struct.pack(f"<{dir_count}I", *dir_hash)
        fh = struct.pack(f"<{file_count}I", *file_hash)
        header = struct.pack("<10Q", 0x50, meta_off, len(dh), meta_off + len(dh), len(dir_table),
                             meta_off + len(dh) + len(dir_table), len(fh),
                             meta_off + len(dh) + len(dir_table) + len(fh), len(file_table), ROMFS_DATA_OFFSET)
        meta = dh + bytes(dir_table) + fh + bytes(file_table)
        self.header = header
        self.meta = meta
        self.meta_off = meta_off
        self.placements = [(doff, src) for _, _, src, _, doff in files]
        # (path, image offset, size) in data order, for the per-file chunk hashes of patches.
        self.file_ranges = [(self._path(d, name), ROMFS_DATA_OFFSET + doff, src.size) for d, name, src, _, doff in files]
        self.size = meta_off + len(meta)
        return self.size

    @staticmethod
    def _path(d, name):
        parts = [name]
        while d.parent is not None:
            parts.append(d.name)
            d = d.parent
        return "/".join(p.decode("utf-8") for p in reversed(parts))

    def chunks(self):
        """The whole image, in order (call layout() first)."""
        yield self.header + bytes(ROMFS_DATA_OFFSET - len(self.header))
        pos = ROMFS_DATA_OFFSET
        for doff, src in self.placements:
            target = ROMFS_DATA_OFFSET + doff
            if target > pos:
                yield bytes(target - pos)
                pos = target
            for block in src.chunks():
                yield block
                pos += len(block)
        if self.meta_off > pos:
            yield bytes(self.meta_off - pos)
        yield self.meta


# ---- hashing helpers ---------------------------------------------------------------------------------------------------

class BlockHasher:
    """SHA-256 of every block of a stream. Full blocks: the last one zero-padded (IVFC) or not (HierarchicalSha256)."""

    def __init__(self, block, pad_last):
        self.block, self.pad_last = block, pad_last
        self.pending = bytearray()
        self.hashes = []
        self.total = 0

    def update(self, data):
        self.total += len(data)
        mv = memoryview(data)
        if self.pending:
            need = self.block - len(self.pending)
            self.pending += mv[:need]
            mv = mv[need:]
            if len(self.pending) < self.block:
                return
            self.hashes.append(sha256(self.pending))
            self.pending = bytearray()
        full = len(mv) - len(mv) % self.block
        for off in range(0, full, self.block):
            self.hashes.append(hashlib.sha256(mv[off:off + self.block]).digest())
        if full < len(mv):
            self.pending += mv[full:]

    def finish(self):
        if self.pending:
            tail = bytes(self.pending) + (bytes(self.block - len(self.pending)) if self.pad_last else b"")
            self.hashes.append(sha256(tail))
            self.pending = bytearray()
        return b"".join(self.hashes)


def hash_blocks(data, block, pad_last):
    h = BlockHasher(block, pad_last)
    h.update(data)
    return h.finish()


# ---- NCA sections ------------------------------------------------------------------------------------------------------

class Section:
    """One NCA section: its FS header and its plaintext content as chunks (size known up front)."""

    def __init__(self, fs_header, size, chunk_fn, generation=0, secure_value=0):
        assert len(fs_header) == 0x200 and size % MEDIA_UNIT == 0
        # The AES-CTR upper IV (FS header 0x140): u32 generation, u32 secure value; IV = BE32(secure) BE32(generation)
        # BE64(offset / 16).
        fs_header = bytearray(fs_header)
        struct.pack_into("<II", fs_header, 0x140, generation, secure_value)
        self.fs_header, self.size, self.chunks = bytes(fs_header), size, chunk_fn
        self.ctr_upper = (secure_value << 32) | generation


def pfs0_section(pfs0, hash_block):
    table = hash_blocks(pfs0, hash_block, pad_last=False)
    pfs0_off = align(len(table), MEDIA_UNIT)
    body = table + bytes(pfs0_off - len(table)) + pfs0
    body += bytes(align(len(body), MEDIA_UNIT) - len(body))
    fs = bytearray(0x200)
    struct.pack_into("<HBBB", fs, 0, 2, 1, 2, 3)  # version 2, PartitionFs, HierarchicalSha256, AES-CTR
    fs[0x08:0x28] = sha256(table)
    struct.pack_into("<IIQQQQ", fs, 0x28, hash_block, 2, 0, len(table), pfs0_off, len(pfs0))
    return Section(bytes(fs), len(body), lambda: iter((body,)))


class IvfcPlan:
    """IVFC (HierarchicalIntegrity) over a data image: level 6 = the data, levels 5..1 = SHA-256 of the 0x4000 blocks
    of the level below (last block zero-padded), master hash = SHA-256 of level 1 (one zero-padded block). Every level
    starts on a block boundary; the section ends on one."""

    def __init__(self, data_hashes, data_size):
        levels = [None] * IVFC_LEVELS
        levels[-1] = (data_size, None)
        below = data_hashes
        for i in range(IVFC_LEVELS - 2, -1, -1):
            levels[i] = (len(below), below)
            below = hash_blocks(below, IVFC_BLOCK, pad_last=True) if i else None
        level1 = levels[0][1]
        if len(level1) > IVFC_BLOCK:
            raise PackError("RomFS too large for 6 IVFC levels")
        self.master = sha256(level1 + bytes(IVFC_BLOCK - len(level1)))
        self.levels = levels
        self.offsets = []
        off = 0
        for size, _ in levels:
            self.offsets.append(off)
            off = align(off + size, IVFC_BLOCK)
        self.size = off

    @classmethod
    def for_image(cls, image_size, master):
        """The plan of an existing section from its image size and master hash (the hash levels are not available:
        only the layout and the FS header are needed, e.g. for a program-only update)."""
        plan = cls.__new__(cls)
        levels = [None] * IVFC_LEVELS
        levels[-1] = (image_size, None)
        below = image_size
        for i in range(IVFC_LEVELS - 2, -1, -1):
            size = (below + IVFC_BLOCK - 1) // IVFC_BLOCK * 32
            levels[i] = (size, None)
            below = size
        if levels[0][0] > IVFC_BLOCK:
            raise PackError("RomFS too large for 6 IVFC levels")
        plan.master = master
        plan.levels = levels
        plan.offsets = []
        off = 0
        for size, _ in levels:
            plan.offsets.append(off)
            off = align(off + size, IVFC_BLOCK)
        plan.size = off
        return plan

    def fs_header(self):
        fs = bytearray(0x200)
        struct.pack_into("<HBBB", fs, 0, 2, 0, 3, 3)  # version 2, RomFs, HierarchicalIntegrity, AES-CTR
        struct.pack_into("<4sIII", fs, 0x08, b"IVFC", 0x20000, 0x20, IVFC_LEVELS + 1)
        for i, (size, _) in enumerate(self.levels):
            struct.pack_into("<QQII", fs, 0x18 + 0x18 * i, self.offsets[i], size, IVFC_BLOCK_LOG2, 0)
        fs[0xC8:0xE8] = self.master
        return bytes(fs)

    def chunks(self, data_chunks):
        pos = 0
        for i, (size, blob) in enumerate(self.levels):
            if self.offsets[i] > pos:
                yield bytes(self.offsets[i] - pos)
                pos = self.offsets[i]
            if blob is not None:
                yield blob
                pos += len(blob)
            else:
                for block in data_chunks():
                    yield block
                    pos += len(block)
        if self.size > pos:
            yield bytes(self.size - pos)


PATCH_CHUNK = 0x10000  # granularity of the per-file comparison between a base and an update (64 KiB)


class RangeChunkHasher:
    """SHA-256 of fixed-size chunks of given ranges of a stream: ranges = [(key, start, end)], sorted, disjoint.
    Chunk k of a range covers [start + k*chunk, min(start + (k+1)*chunk, end))."""

    def __init__(self, ranges, chunk):
        self.ranges, self.chunk = ranges, chunk
        self.index = 0
        self.pos = 0
        self.current = None
        self.result = {}

    def update(self, data):
        mv = memoryview(data)
        while mv:
            if self.index >= len(self.ranges):
                self.pos += len(mv)
                return
            key, start, end = self.ranges[self.index]
            if self.pos < start:  # before the next range
                skip = min(len(mv), start - self.pos)
                mv = mv[skip:]
                self.pos += skip
                continue
            if self.current is None:
                self.current = hashlib.sha256()
                self.result.setdefault(key, [])
            chunk_end = min(start + ((self.pos - start) // self.chunk + 1) * self.chunk, end)
            take = min(len(mv), chunk_end - self.pos)
            self.current.update(mv[:take])
            mv = mv[take:]
            self.pos += take
            if self.pos == chunk_end:
                self.result[key].append(self.current.digest())
                self.current = None
                if self.pos == end:
                    self.index += 1


def file_hash_ranges(romfs):
    """The compared span of every file: its bytes plus the zero padding up to 16 (the next region starts there)."""
    return [(path, off, off + align(size, 0x10)) for path, off, size in romfs.file_ranges if size]


def romfs_section(romfs, progress=None):
    """Pass 1: hash the whole image (IVFC levels) and every file in 64 KiB chunks (for patches). Returns the Section;
    romfs.plan is the IVFC plan and romfs.chunk_hashes {path: [sha256]} (pass 2 re-reads the files)."""
    romfs.layout()
    hasher = BlockHasher(IVFC_BLOCK, pad_last=True)
    chunks = RangeChunkHasher(file_hash_ranges(romfs), PATCH_CHUNK)
    done = 0
    for block in romfs.chunks():
        hasher.update(block)
        chunks.update(block)
        done += len(block)
        if progress:
            progress(done, romfs.size)
    if hasher.total != romfs.size:
        raise PackError("RomFS size changed while hashing (files modified?)")
    plan = IvfcPlan(hasher.finish(), romfs.size)
    romfs.plan = plan
    romfs.chunk_hashes = chunks.result
    return Section(plan.fs_header(), plan.size, lambda: plan.chunks(romfs.chunks))


# ---- NCA -------------------------------------------------------------------------------------------------------------

class NcaBuilder:
    def __init__(self, keys, title_id, content_type, sections, key_area_key=None, sign_key=None):
        self.keys, self.title_id, self.content_type = keys, title_id, content_type
        self.sections = sections
        self.aes_key = key_area_key or os.urandom(16)
        self.sign_key = sign_key
        self.starts = []
        off = 0xC00
        for s in sections:
            self.starts.append(off)
            off += s.size
        self.size = off

    def header(self):
        h = bytearray(0xC00)
        h[0x200:0x204] = b"NCA3"
        h[0x204] = 0  # distribution: download
        h[0x205] = self.content_type
        h[0x206] = 0  # key generation (old field): master key 00
        h[0x207] = 0  # key area encryption key index: application
        struct.pack_into("<QQII", h, 0x208, self.size, self.title_id, 0, SDK_VERSION)
        for i, (s, start) in enumerate(zip(self.sections, self.starts)):
            struct.pack_into("<IIII", h, 0x240 + 0x10 * i, start // MEDIA_UNIT, (start + s.size) // MEDIA_UNIT, 1, 0)
            h[0x280 + 0x20 * i:0x2A0 + 0x20 * i] = sha256(s.fs_header)
            h[0x400 + 0x200 * i:0x600 + 0x200 * i] = s.fs_header
        key_area = bytes(0x20) + self.aes_key + bytes(0x10)  # slot 2 = the AES-CTR key
        ecb = Cipher(algorithms.AES(self.keys["key_area_key_application_00"]), modes.ECB()).encryptor()
        h[0x300:0x340] = ecb.update(key_area) + ecb.finalize()
        if self.sign_key is not None:
            h[0x100:0x200] = self.sign_key.sign(bytes(h[0x200:0x400]),
                                                padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                                                hashes.SHA256())
        return encrypt_header(bytes(h), self.keys["header_key"])

    def chunks(self):
        """The encrypted NCA, in order."""
        yield self.header()
        for s, start in zip(self.sections, self.starts):
            iv = struct.pack(">QQ", s.ctr_upper, start >> 4)  # upper IV (generation, secure value), offset / 16
            enc = Cipher(algorithms.AES(self.aes_key), modes.CTR(iv)).encryptor()
            written = 0
            for block in s.chunks():
                written += len(block)
                yield enc.update(block)
            tail = enc.finalize()
            if tail:
                yield tail
            if written != s.size:
                raise PackError(f"NCA section size mismatch ({written} != {s.size})")

    def to_bytes(self):
        return b"".join(self.chunks())


def encrypt_header(plain, header_key):
    """AES-128-XTS over 0x200-byte sectors, sector numbers 0..5 as big-endian tweaks (Nintendo's convention)."""
    out = bytearray()
    for sector in range(len(plain) // 0x200):
        enc = Cipher(algorithms.AES(header_key), modes.XTS(sector.to_bytes(16, "big"))).encryptor()
        out += enc.update(plain[sector * 0x200:(sector + 1) * 0x200]) + enc.finalize()
    return bytes(out)


def decrypt_header(data, header_key):
    out = bytearray()
    for sector in range(len(data) // 0x200):
        dec = Cipher(algorithms.AES(header_key), modes.XTS(sector.to_bytes(16, "big"))).decryptor()
        out += dec.update(data[sector * 0x200:(sector + 1) * 0x200]) + dec.finalize()
    return bytes(out)


CNMT_APPLICATION, CNMT_PATCH = 0x80, 0x81


def build_cnmt(title_id, version, records, meta_type=CNMT_APPLICATION, application_id=None):
    """records: [(sha256 of the NCA, size, CNMT content type)]. Application: extended header {patch ID, required system
    version, required application version}; Patch: {application ID, required system version, extended data size,
    reserved 8} (ncm PatchMetaExtendedHeader), no extended data."""
    if meta_type == CNMT_APPLICATION:
        ext = struct.pack("<QII", title_id + 0x800, 0, 0)
    else:
        ext = struct.pack("<QII8x", application_id, 0, 0)
    out = struct.pack("<QIBBHHHB3xI4x", title_id, version, meta_type, 0, len(ext), len(records), 0, 0, 0)
    out += ext
    for digest, size, ctype in records:
        out += digest + digest[:16] + size.to_bytes(6, "little") + bytes([ctype, 0])
    return out + bytes(0x20)


# ---- output (single file or FAT32 split folder) ------------------------------------------------------------------------

class Output:
    def __init__(self, path, split):
        self.path, self.split = path, split
        self.pos = 0
        self.parts = {}
        if split:
            os.makedirs(path, exist_ok=True)
        else:
            self.fh = open(path, "w+b")

    def _part(self, index):
        if index not in self.parts:
            self.parts[index] = open(os.path.join(self.path, f"{index:02d}"), "w+b")
        return self.parts[index]

    def seek(self, pos):
        self.pos = pos
        if not self.split:
            self.fh.seek(pos)

    def write(self, data):
        if not self.split:
            self.fh.write(data)
            self.pos += len(data)
            return
        mv = memoryview(data)
        while mv:
            index, inner = divmod(self.pos, FAT32_PART)
            n = min(len(mv), FAT32_PART - inner)
            part = self._part(index)
            part.seek(inner)
            part.write(mv[:n])
            mv = mv[n:]
            self.pos += n

    def close(self):
        if self.split:
            for fh in self.parts.values():
                fh.close()
        else:
            self.fh.close()


# ---- input -------------------------------------------------------------------------------------------------------------

def skip_name(name):
    return name.startswith("._") or name in (".DS_Store", "Thumbs.db", "desktop.ini")


def collect_tree(base, prefix, romfs):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if not skip_name(d))
        rel = os.path.relpath(dirpath, base)
        for name in sorted(filenames):
            if skip_name(name):
                continue
            host = os.path.join(dirpath, name)
            path = prefix + ("" if rel == "." else rel.replace(os.sep, "/") + "/") + name
            romfs.add(path, RomfsSource(os.path.getsize(host), path=host))


def check_toml(path):
    """The game loads masseffect.toml before logging starts; a file that does not parse (for example the same key twice)
    is ignored as a whole, silently. Refuse to pack one."""
    try:
        import tomllib
    except ImportError:  # Python < 3.11: no check
        return
    try:
        with open(path, "rb") as fh:
            tomllib.load(fh)
    except tomllib.TOMLDecodeError as e:
        raise PackError(f"{path} is not valid TOML ({e}); the game would ignore all of it") from None


def build_romfs(input_dir, marker_text, include_dlc=True):
    romfs = Romfs()
    for name in (TOML_NAME, SHADERS_NAME, SHADERS_NAME + ".idx"):
        path = os.path.join(input_dir, name)
        if not os.path.isfile(path):
            raise PackError(f"{name} not found in {input_dir} (is it an installer output folder?)")
        romfs.add(name, RomfsSource(os.path.getsize(path), path=path))
    check_toml(os.path.join(input_dir, TOML_NAME))
    prewarm = os.path.join(input_dir, PREWARM_LIST_NAME)
    if os.path.isfile(prewarm):
        romfs.add(PREWARM_LIST_NAME, RomfsSource(os.path.getsize(prewarm), path=prewarm))
    game_root = os.path.join(input_dir, "game_root")
    if not os.path.isfile(os.path.join(game_root, "default.xex")):
        raise PackError(f"{game_root}/default.xex not found: the NSP needs the full install (not the update zip)")
    collect_tree(game_root, "game_root/", romfs)
    dlc = os.path.join(input_dir, *DLC_DIR)
    has_dlc = include_dlc and os.path.isdir(dlc)
    if has_dlc:
        collect_tree(dlc, "/".join(DLC_DIR) + "/", romfs)
    marker = marker_text.encode()
    romfs.add(MARKER_NAME, RomfsSource(len(marker), data=marker))
    return romfs, has_dlc


# ---- base metadata and reading a base NSP ------------------------------------------------------------------------------
#
# An update (patch) only stores what differs from the base application. To know what differs it needs, for every file
# of the base RomFS, where its bytes are in the base Program NCA's RomFS section and the SHA-256 of each 64 KiB chunk of
# them. A full pack writes that next to the NSP (<nsp>.basemeta.json: offsets, sizes and hashes, no keys and no game
# bytes); for a base without it, the same is computed by decrypting the base NSP (needs the keys and one read of it).

BASE_META_FORMAT = 1


def base_metadata(title_id, romfs, program_nca, data_dir):
    plan = romfs.plan
    return {
        "format": BASE_META_FORMAT,
        "kind": "masseffect-nx base RomFS",
        "title_id": f"{title_id:016x}",
        "program_nca": program_nca,
        "data_dir": data_dir,
        "romfs": {"section_size": plan.size, "l6_offset": plan.offsets[-1], "image_size": romfs.size,
                  "master_hash": plan.master.hex(), "chunk": PATCH_CHUNK},
        "files": {path: {"offset": off, "size": size,
                         "hashes": base64.b64encode(b"".join(romfs.chunk_hashes.get(path, []))).decode()}
                  for path, off, size in romfs.file_ranges},
    }


def metadata_path(output):
    return output.rstrip("/\\") + ".basemeta.json"


def load_base_metadata(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            meta = json.load(fh)
    except (OSError, ValueError) as e:
        raise PackError(f"cannot read the base metadata {path}: {e}") from None
    if meta.get("format") != BASE_META_FORMAT or meta.get("romfs", {}).get("chunk") != PATCH_CHUNK:
        raise PackError(f"{path}: unsupported base metadata format")
    return meta


class InputFile:
    """A single file, or a FAT32 split folder (00, 01, ...) read as one."""

    def __init__(self, path):
        if os.path.isdir(path):
            names = sorted(n for n in os.listdir(path) if n.isdigit())
            self.parts = [os.path.join(path, n) for n in names]
        else:
            self.parts = [path]
        if not self.parts:
            raise PackError(f"{path}: empty split folder")
        self.sizes = [os.path.getsize(p) for p in self.parts]
        self.handles = [open(p, "rb") for p in self.parts]
        self.size = sum(self.sizes)

    def read(self, offset, length):
        out = bytearray()
        for fh, size in zip(self.handles, self.sizes):
            if offset >= size:
                offset -= size
                continue
            fh.seek(offset)
            data = fh.read(min(length - len(out), size - offset))
            out += data
            offset = 0
            if len(out) == length:
                break
        if len(out) != length:
            raise PackError("unexpected end of the base NSP")
        return bytes(out)

    def close(self):
        for fh in self.handles:
            fh.close()


class NcaReader:
    """Reads a plain AES-CTR NCA (as this packer writes them) inside a container."""

    def __init__(self, src, offset, keys):
        self.src, self.offset = src, offset
        self.h = decrypt_header(src.read(offset, 0xC00), keys["header_key"])
        if self.h[0x200:0x204] != b"NCA3":
            raise PackError("base NCA header does not decrypt (wrong header_key?)")
        self.content_type = self.h[0x205]
        self.size, self.title_id = struct.unpack_from("<QQ", self.h, 0x208)
        if max(self.h[0x206], self.h[0x220]) > 1 or self.h[0x207] != 0 or any(self.h[0x230:0x240]):
            raise PackError("base NCA uses another key generation or a title key (not made by this packer)")
        ecb = Cipher(algorithms.AES(keys["key_area_key_application_00"]), modes.ECB()).decryptor()
        self.ctr_key = (ecb.update(self.h[0x300:0x340]) + ecb.finalize())[0x20:0x30]

    def section(self, index):
        start, end, _, _ = struct.unpack_from("<IIII", self.h, 0x240 + 0x10 * index)
        fs = self.h[0x400 + 0x200 * index:0x600 + 0x200 * index]
        return start * MEDIA_UNIT, end * MEDIA_UNIT, fs

    def read_section(self, index, offset, length):
        start, end, fs = self.section(index)
        if fs[4] != 3:
            raise PackError("base RomFS section is not plain AES-CTR (a patch cannot be a base)")
        aligned = offset & ~0xF
        generation, secure = struct.unpack_from("<II", fs, 0x140)
        iv = struct.pack(">QQ", (secure << 32) | generation, (start + aligned) >> 4)
        raw = self.src.read(self.offset + start + aligned, offset - aligned + length)
        dec = Cipher(algorithms.AES(self.ctr_key), modes.CTR(iv)).decryptor()
        return (dec.update(raw) + dec.finalize())[offset - aligned:]


def read_pfs0_entries(src):
    magic, count, strtab, _ = struct.unpack("<4sIII", src.read(0, 0x10))
    if magic != b"PFS0":
        raise PackError("the base is not an NSP (no PFS0 header)")
    table = src.read(0x10, 0x18 * count + strtab)
    names = table[0x18 * count:]
    data_off = 0x10 + 0x18 * count + strtab
    out = []
    for i in range(count):
        off, size, name_off, _ = struct.unpack_from("<QQII", table, 0x18 * i)
        name = names[name_off:names.index(b"\0", name_off)].decode()
        out.append((name, data_off + off, size))
    return out


def valid_data_dir(d):
    """Same rules as ValidDataDir in app/src/me_packaged.cpp (an invalid value there silently means the default)."""
    return not (not d.startswith("sdmc:/") or len(d) <= 6 or len(d) > 700 or ".." in d or "//" in d or ":" in d[6:]
                or d.endswith("/"))


def marker_data_dir(text):
    """data_dir= of a package marker (masseffect-nx-package.txt), or None."""
    for line in text.decode("utf-8", "replace").splitlines():
        if line.startswith("data_dir="):
            d = line[len("data_dir="):].strip()
            return d if valid_data_dir(d) else None
    return None


def metadata_from_nsp(path, keys, log=print):
    """The base metadata of an existing full NSP, by decrypting its Program NCA's RomFS (one read of the section)."""
    src = InputFile(path)
    try:
        program = None
        for name, off, size in read_pfs0_entries(src):
            if name.endswith(".nca") and not name.endswith(".cnmt.nca"):
                nca = NcaReader(src, off, keys)
                if nca.content_type == CONTENT_PROGRAM:
                    program, program_name = nca, name[:-4]
        if program is None:
            raise PackError("the base NSP has no Program NCA")
        start, end, fs = program.section(1)
        if fs[2:4] != b"\x00\x03" or fs[8:12] != b"IVFC":
            raise PackError("the base Program NCA has no RomFS section")
        levels = [struct.unpack_from("<QQII", fs, 0x18 + 0x18 * i) for i in range(IVFC_LEVELS)]
        l6, image_size = levels[-1][0], levels[-1][1]
        header = struct.unpack("<10Q", program.read_section(1, l6, 0x50))
        _, _, _, dt_off, dt_size, _, _, ft_off, ft_size, data_off = header
        dirs = program.read_section(1, l6 + dt_off, dt_size)
        files = program.read_section(1, l6 + ft_off, ft_size)
        found = []

        def walk(doff, prefix):
            _, _, child, file, _, _ = struct.unpack_from("<6I", dirs, doff)
            while file != ROMFS_EMPTY:
                _, sibling, foff, fsize, _, nlen = struct.unpack_from("<IIQQII", files, file)
                name = files[file + 0x20:file + 0x20 + nlen].decode("utf-8")
                found.append((prefix + name, data_off + foff, fsize))
                file = sibling
            while child != ROMFS_EMPTY:
                _, sibling, _, _, _, nlen = struct.unpack_from("<6I", dirs, child)
                walk(child, prefix + dirs[child + 0x18:child + 0x18 + nlen].decode("utf-8") + "/")
                child = sibling

        walk(0, "")
        found.sort(key=lambda f: f[1])
        log(f"Base: {path}: title {program.title_id:016x}, {len(found)} files; hashing its RomFS")
        ranges = [(p, off, off + align(size, 0x10)) for p, off, size in found if size]
        hasher = RangeChunkHasher(ranges, PATCH_CHUNK)
        if ranges:
            pos, end_pos = ranges[0][1], ranges[-1][2]
            hasher.pos = pos
            while pos < end_pos:
                n = min(CHUNK, end_pos - pos)
                hasher.update(program.read_section(1, l6 + pos, n))
                pos += n
        data_dir = None
        for p, off, size in found:
            if p == MARKER_NAME and 0 < size <= 0x1000:
                data_dir = marker_data_dir(program.read_section(1, l6 + off, size))
        return {
            "format": BASE_META_FORMAT, "kind": "masseffect-nx base RomFS (from the NSP)",
            "title_id": f"{program.title_id:016x}", "program_nca": program_name, "data_dir": data_dir,
            "romfs": {"section_size": end - start, "l6_offset": l6, "image_size": image_size,
                      "master_hash": fs[0xC8:0xE8].hex(), "chunk": PATCH_CHUNK},
            "files": {p: {"offset": off, "size": size,
                          "hashes": base64.b64encode(b"".join(hasher.result.get(p, []))).decode()}
                      for p, off, size in found},
        }
    finally:
        src.close()


# ---- patch RomFS (BKTR: indirect storage + AES-CTR-EX) ------------------------------------------------------------------
#
# The patch's RomFS section describes the *new* RomFS section (IVFC levels + image, exactly what a full pack of the new
# input would contain) as a virtual storage. Its physical content is:
#
#   [patch data: the bytes that are not in the base] [indirect table] [AES-CTR-EX table]
#
# * Indirect table (bucket tree, 16 KiB nodes, 0x14-byte entries {virtual offset, physical offset, storage}): maps
#   every range of the virtual storage either to the base Program NCA's RomFS section (storage 0, decrypted section
#   offset) or to the patch data (storage 1).
# * AES-CTR-EX table (bucket tree, 16 KiB nodes, 0x10-byte entries {offset, encryption, generation}): how the physical
#   region [0, table offset) is encrypted: here one entry, AES-CTR with this section's key and generation, i.e. the same
#   counter as plain AES-CTR with upper IV (secure value 0, generation). The table itself uses the FS header's upper IV.
# * FS header: encryption type 4 (AesCtrEx), patch info at 0x100 (indirect offset/size/bucket header) and 0x120
#   (AES-CTR-EX offset/size/bucket header), the IVFC superblock of the virtual storage, upper IV at 0x140.
# Every boundary is 16-byte aligned (AES-CTR-EX reads in 16-byte blocks). Formats: switchbrew "NCA", Atmosphere
# fssystem (bucket tree, indirect storage, aes_ctr_counter_extended_storage), hactool bktr.c.

BUCKET_NODE = 0x4000


def bucket_tree(entries, entry_size, end_offset):
    """entries: [(offset, packed entry bytes)] sorted. Returns (16-byte header, node storage + entry storage)."""
    per_set = (BUCKET_NODE - 0x10) // entry_size
    sets = [entries[i:i + per_set] for i in range(0, len(entries), per_set)]
    if len(sets) > (BUCKET_NODE - 0x10) // 8:
        raise PackError("patch table too large (more than one L1 node of entry sets)")
    l1 = struct.pack("<iiq", 0, len(sets), end_offset) + b"".join(struct.pack("<q", s[0][0]) for s in sets)
    out = bytearray(l1 + bytes(BUCKET_NODE - len(l1)))
    for i, s in enumerate(sets):
        set_end = sets[i + 1][0][0] if i + 1 < len(sets) else end_offset
        node = struct.pack("<iiq", i, len(s), set_end) + b"".join(e for _, e in s)
        out += node + bytes(BUCKET_NODE - len(node))
    header = struct.pack("<4sIii", b"BKTR", 1, len(entries), 0)
    return header, bytes(out)


def plan_patch_runs(romfs, base_meta):
    """Maps the new virtual RomFS section: [(virtual offset, length, storage, physical offset)], storage 0 = base
    section, 1 = patch data. Files are compared chunk by chunk with the base file of the same path; everything else
    (IVFC levels, the RomFS header and tables) is always in the patch."""
    plan = romfs.plan
    l6, total = plan.offsets[-1], plan.size
    base_l6 = base_meta["romfs"]["l6_offset"]
    base_files = base_meta["files"]
    runs = []
    patch = [0]

    def add(virt, length, storage, phys):
        if length <= 0:
            return
        if runs:
            v, n, st, ph = runs[-1]
            if st == storage and v + n == virt and ph + n == phys:
                runs[-1] = (v, n + length, st, ph)
                return
        runs.append((virt, length, storage, phys))

    def add_patch(virt, length):
        add(virt, length, 1, patch[0])
        patch[0] += max(length, 0)

    pos = 0
    mapped = 0
    for path, off, size in romfs.file_ranges:
        if not size:
            continue
        vs, ve = l6 + off, l6 + off + align(size, 0x10)
        add_patch(pos, vs - pos)
        new_hashes = romfs.chunk_hashes[path]
        base = base_files.get(path)
        base_hashes = base64.b64decode(base["hashes"]) if base else b""
        base_span = align(base["size"], 0x10) if base else 0
        for k, digest in enumerate(new_hashes):
            cs, ce = vs + k * PATCH_CHUNK, min(vs + (k + 1) * PATCH_CHUNK, ve)
            base_len = min(PATCH_CHUNK, base_span - k * PATCH_CHUNK) if base else 0
            if base and base_len == ce - cs and base_hashes[32 * k:32 * k + 32] == digest:
                add(cs, ce - cs, 0, base_l6 + base["offset"] + k * PATCH_CHUNK)
                mapped += ce - cs
            else:
                add_patch(cs, ce - cs)
        pos = ve
    add_patch(pos, total - pos)
    assert sum(n for _, n, _, _ in runs) == total and all(v % 0x10 == 0 and p % 0x10 == 0 for v, _, _, p in runs)
    return runs, patch[0], mapped


def patch_romfs_section(romfs, base_meta, generation):
    """The BKTR RomFS section of an update (after romfs_section's pass 1 on the new input)."""
    plan = romfs.plan
    runs, patch_size, mapped = plan_patch_runs(romfs, base_meta)
    return bktr_section(plan, runs, patch_size, mapped, generation, lambda: plan.chunks(romfs.chunks))


def program_only_romfs_section(base_meta, generation):
    """The RomFS section of a program-only update: the base's section mapped 1:1 (one indirect entry, storage 0), no
    patch data. Needs only the base metadata (no game data); the RomFS, including the marker, stays the base's."""
    r = base_meta["romfs"]
    plan = IvfcPlan.for_image(r["image_size"], bytes.fromhex(r["master_hash"]))
    if plan.size != r["section_size"] or plan.offsets[-1] != r["l6_offset"]:
        raise PackError("the base metadata does not describe a RomFS section made by this packer")
    return bktr_section(plan, [(0, plan.size, 0, 0)], 0, plan.size, generation, None)


def bktr_section(plan, runs, patch_size, mapped, generation, virtual_chunks):
    """runs: [(virtual offset, length, storage, physical offset)]; virtual_chunks() streams the virtual section (only
    read when some run is stored in the patch)."""
    indirect_offset = align(patch_size, BUCKET_NODE)
    ind_header, ind_table = bucket_tree(
        [(v, struct.pack("<qqi", v, p, st)) for v, _, st, p in runs], 0x14, plan.size)
    aes_offset = indirect_offset + len(ind_table)
    aes_header, aes_table = bucket_tree([(0, struct.pack("<qB3xi", 0, 0, generation))], 0x10, aes_offset)
    size = aes_offset + len(aes_table)

    fs = bytearray(plan.fs_header())
    fs[4] = 4  # AesCtrEx
    struct.pack_into("<qq16s", fs, 0x100, indirect_offset, len(ind_table), ind_header)
    struct.pack_into("<qq16s", fs, 0x120, aes_offset, len(aes_table), aes_header)
    patch_runs = [(v, n) for v, n, st, _ in runs if st == 1]

    def chunks():
        if patch_runs:
            yield from select_ranges(virtual_chunks(), patch_runs)
        if indirect_offset > patch_size:
            yield bytes(indirect_offset - patch_size)
        yield ind_table
        yield aes_table

    section = Section(bytes(fs), size, chunks, generation=generation)
    section.stats = {"runs": len(runs), "patch_bytes": patch_size, "base_bytes": mapped, "virtual": plan.size}
    return section


def select_ranges(stream, ranges):
    """The bytes of a sequential stream that fall in the sorted, disjoint [(start, length)] ranges, in order."""
    pos = 0
    i = 0
    for block in stream:
        mv = memoryview(block)
        block_start, block_end = pos, pos + len(mv)
        while i < len(ranges) and ranges[i][0] < block_end:
            start, length = ranges[i]
            lo, hi = max(start, block_start), min(start + length, block_end)
            if lo < hi:
                yield bytes(mv[lo - block_start:hi - block_start])
            if start + length <= block_end:
                i += 1
            else:
                break
        pos = block_end


# ---- the whole package -----------------------------------------------------------------------------------------------

def load_sign_key(path):
    with open(path, "rb") as fh:
        return serialization.load_pem_private_key(fh.read(), password=None)


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


def check_update_title(base_title_id, edition, log=print):
    """An update for the edition `edition` (None: no check) against a base of title base_title_id. A base of another
    known edition is refused (the program of one edition over the game data of the other); an unknown title ID (a base
    packed with an explicit --title-id) only gives a warning."""
    if not edition or base_title_id == EDITIONS[edition]["title_id"]:
        return
    other = next((name for name, e in EDITIONS.items() if e["title_id"] == base_title_id), None)
    if other:
        raise PackError(f"the base is the {other} edition (title {base_title_id:016x}), but this update is for the "
                        f"{edition} edition (title {EDITIONS[edition]['title_id']:016x}); choose the base of the same "
                        f"edition")
    log(f"warning: the base has title {base_title_id:016x}, not the {edition} edition's "
        f"{EDITIONS[edition]['title_id']:016x}; the update keeps the base's title ID")


def build(args, log=print):
    keys = load_keys(args.keys)
    program_only = bool(getattr(args, "program_only", False))
    if program_only and not getattr(args, "update", False):
        raise PackError("--program-only needs --update")
    if program_only and not args.nro:
        raise PackError("--program-only needs --nro (the new program)")
    if not program_only and not args.input:
        raise PackError("--input is required (except for --update --program-only)")
    input_dir = os.path.abspath(args.input) if args.input else None
    nro_path = args.nro or os.path.join(input_dir, NRO_NAME)
    try:
        with open(nro_path, "rb") as fh:
            nro = Nro(fh.read())
    except OSError as e:
        raise PackError(f"cannot read the NRO {nro_path}: {e.strerror}") from None
    if "nacp" not in nro.assets or "icon" not in nro.assets:
        raise PackError("the NRO has no NACP or icon (build it with tools/build_nro.sh)")

    update = bool(getattr(args, "update", False))
    edition = getattr(args, "edition", None)
    ed = EDITIONS[edition] if edition else None
    explicit_data_dir = getattr(args, "data_dir", None)
    data_dir = explicit_data_dir or (ed["data_dir"] if ed else DEFAULT_DATA_DIR)
    base_meta = None
    if update:
        if not args.base:
            raise PackError("--update needs --base (the base NSP, or its .basemeta.json)")
        if args.version < 1:
            raise PackError("--update needs --version N (N >= 1; the title version becomes N * 0x10000)")
        base_meta = (load_base_metadata(args.base) if args.base.endswith(".json")
                     else metadata_from_nsp(args.base, keys, log))
        # The base's own title ID always wins (an update must patch the installed title).
        title_id = int(base_meta["title_id"], 16)
        check_update_title(title_id, edition, log)
        if not explicit_data_dir and base_meta.get("data_dir"):
            data_dir = base_meta["data_dir"]  # keep the installed game's folder unless --data-dir says otherwise
        if not program_only and base_meta.get("data_dir") and base_meta["data_dir"] != data_dir:
            log(f"warning: the base was packed with data_dir={base_meta['data_dir']}, this update uses "
                f"{data_dir} (saves and caches move to the new folder)")
        version = args.version * 0x10000
    else:
        title_id = getattr(args, "title_id", None)
        if title_id is None:
            title_id = ed["title_id"] if ed else DEFAULT_TITLE_ID
        version = args.version
    if title_id & 0xFFF or not 0x0100000000000000 <= title_id <= 0x0FFFFFFFFFFFF000:
        raise PackError("the title ID must be 01xxxxxxxxxxx000 .. 0fffffffffffffff with the last three digits 000")
    patch_id = title_id + 0x800

    # Program NCA, ExeFS
    sign_key = rsa.generate_private_key(public_exponent=65537, key_size=2048) if args.sign_key is None else \
        load_sign_key(args.sign_key)
    modulus = sign_key.public_key().public_numbers().n.to_bytes(0x100, "big")
    npdm_cfg = npdm_config(title_id)
    if getattr(args, "emulator_compatible", False):
        npdm_cfg = npdm_emulator_compatible(npdm_cfg)
        log("NPDM: emulator-compatible (pre-19.0.0 DebugFlags layout; for yuzu/Eden, not for the console)")
    npdm = build_npdm(npdm_cfg, modulus)
    nso = nso_from_nro(nro)
    exefs = pfs0_section(pfs0_bytes([("main", nso), ("main.npdm", npdm)]), EXEFS_HASH_BLOCK)

    clock = {"start": time.time(), "last": 0.0}

    def progress(label):
        def report(done, total):
            now = time.time()
            if now - clock["last"] >= 5 or done == total:
                clock["last"] = now
                rate = done / max(now - clock["start"], 1e-6)
                log(f"  {label}: {done * 100 // max(total, 1)}% ({human(done)} of {human(total)}, {human(rate)}/s)")
        return report

    # Program NCA, RomFS
    romfs, has_dlc = None, False
    if program_only:
        romfs_sec = program_only_romfs_section(base_meta, generation=args.version)
        log(f"Patch RomFS: the base's RomFS section unchanged ({human(romfs_sec.stats['virtual'])}); "
            "only the program is new")
        log(f"note: the settings and {PREWARM_LIST_NAME} stay the base's; if this program changed the pipeline list "
            f"version or the shader package, put the new list into the SD data folder or make a full update")
    else:
        created = getattr(args, "created_utc", None) or \
            datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        marker = (f"format=1\ndata_dir={data_dir}\ntitle_id={title_id:016x}\nversion={version}\n"
                  f"created_utc={created}\nnro_sha256={hashlib.sha256(nro.data).hexdigest()}\n")
        romfs, has_dlc = build_romfs(input_dir, marker, include_dlc=not args.no_dlc)
        if not os.path.isfile(os.path.join(input_dir, PREWARM_LIST_NAME)):
            log(f"warning: no {PREWARM_LIST_NAME} in {input_dir}: packed without the shipped prewarm list (a cold "
                "start compiles more pipelines during play)")
        log(f"RomFS: {romfs.count} files, {human(romfs.data_bytes)}{' (with DLC)' if has_dlc else ''}")
        log("Pass 1/2: hashing the RomFS")
        romfs_sec = romfs_section(romfs, progress("hash"))
        if update:
            romfs_sec = patch_romfs_section(romfs, base_meta, generation=args.version)
            st = romfs_sec.stats
            log(f"Patch RomFS: {st['runs']} ranges; {human(st['base_bytes'])} of files read from the base, "
                f"{human(st['patch_bytes'])} stored in the update (IVFC levels, RomFS tables and changed chunks)")
    program = NcaBuilder(keys, title_id, CONTENT_PROGRAM, [exefs, romfs_sec], sign_key=sign_key)

    # Control NCA
    control_romfs = Romfs()
    nacp = patch_nacp(nro.assets["nacp"], args.name, getattr(args, "display_version", None), title_id)
    control_romfs.add("control.nacp", RomfsSource(len(nacp), data=nacp))
    control_romfs.add("icon_AmericanEnglish.dat", RomfsSource(len(nro.assets["icon"]), data=nro.assets["icon"]))
    control = NcaBuilder(keys, title_id, CONTENT_CONTROL, [romfs_section(control_romfs)]).to_bytes()

    # Meta NCA (Application or Patch content meta)
    if update:
        meta_id, cnmt_name, meta_type = patch_id, f"Patch_{patch_id:016x}.cnmt", CNMT_PATCH
    else:
        meta_id, cnmt_name, meta_type = title_id, f"Application_{title_id:016x}.cnmt", CNMT_APPLICATION

    def make_meta(program_hash, program_size, control_hash):
        cnmt = build_cnmt(meta_id, version, [(program_hash, program_size, CNMT_PROGRAM),
                                             (control_hash, len(control), CNMT_CONTROL)],
                          meta_type=meta_type, application_id=title_id)
        return NcaBuilder(keys, meta_id, CONTENT_META,
                          [pfs0_section(pfs0_bytes([(cnmt_name, cnmt)]), META_HASH_BLOCK)]).to_bytes()

    final, total = write_nsp(args, program, control, make_meta, progress, clock, log)
    log(f"Done in {time.time() - clock['start']:.0f} s: {'update' if update else 'title'} "
        f"{(patch_id if update else title_id):016x} version {version}, program {final[0][0]}, "
        f"control {final[1][0]}, meta {final[2][0]}")
    if not update:
        meta_out = metadata_path(args.output)
        with open(meta_out, "w", encoding="utf-8") as fh:
            json.dump(base_metadata(title_id, romfs, final[0][0][:-4], data_dir), fh)
        log(f"Base metadata for updates: {meta_out} (offsets and hashes only; keep it with the NSP)")
    if args.split:
        log("FAT32 split folder: set its archive bit before installing (Hekate: Tools > Arch bit; Windows: attrib +a)")
    return {"title_id": title_id, "patch_id": patch_id if update else None, "version": version, "files": final,
            "size": total, "dlc": has_dlc, "stats": getattr(romfs_sec, "stats", None)}


def write_nsp(args, program, control, make_meta, progress, clock, log):
    """Streams the Program NCA into the NSP after a placeholder header, then the Control and Meta NCAs, then the real
    header (the NCA names are their SHA-256, known only at the end; the header length does not change)."""
    meta_size = len(make_meta(bytes(32), program.size, bytes(32)))
    names = [("0" * 32 + ".nca", program.size), ("0" * 32 + ".nca", len(control)),
             ("0" * 32 + ".cnmt.nca", meta_size)]
    header_size = len(pfs0_header(names))
    total = header_size + sum(size for _, size in names)
    out_dir = os.path.dirname(os.path.abspath(args.output)) or "."
    free = shutil.disk_usage(out_dir).free
    if free < total + (64 << 20):
        raise PackError(f"not enough free space in {out_dir}: the NSP needs {human(total)}, {human(free)} free")
    log(f"NSP: {human(total)} -> {args.output}{' (FAT32 split folder)' if args.split else ''}")

    out = Output(args.output, args.split)
    ok = False
    try:
        out.seek(header_size)
        log("Pass 2/2: encrypting and writing the Program NCA")
        digest = hashlib.sha256()
        written = 0
        report = progress("write")
        clock["start"] = time.time()
        for block in program.chunks():
            out.write(block)
            digest.update(block)
            written += len(block)
            report(written, program.size)
        if written != program.size:
            raise PackError("Program NCA size mismatch")
        program_hash = digest.digest()
        control_hash = sha256(control)
        out.write(control)
        meta = make_meta(program_hash, program.size, control_hash)
        if len(meta) != meta_size:
            raise PackError("Meta NCA size changed")
        meta_hash = sha256(meta)
        out.write(meta)
        final = [(program_hash[:16].hex() + ".nca", program.size), (control_hash[:16].hex() + ".nca", len(control)),
                 (meta_hash[:16].hex() + ".cnmt.nca", len(meta))]
        header = pfs0_header(final)
        if len(header) != header_size:
            raise PackError("NSP header size changed")
        out.seek(0)
        out.write(header)
        ok = True
    finally:
        out.close()
        if not ok:
            if args.split:
                shutil.rmtree(args.output, ignore_errors=True)
            else:
                try:
                    os.remove(args.output)
                except OSError:
                    pass
    return final, total


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split("\n\n", 1)[1])
    p.add_argument("--input", help="installer output folder (masseffect-nx/ of the zip) or SD install folder "
                                    "(not used by --update --program-only)")
    p.add_argument("--keys", default=os.environ.get("SWITCH_PROD_KEYS", os.path.expanduser("~/.switch/prod.keys")),
                   help="your prod.keys (default: $SWITCH_PROD_KEYS or ~/.switch/prod.keys)")
    p.add_argument("--output", required=True, help="output .nsp (with --split: a folder)")
    p.add_argument("--nro", help=f"program to pack (default: <input>/{NRO_NAME})")
    p.add_argument("--edition", choices=sorted(EDITIONS),
                   help="game edition: sets the defaults of --title-id and --data-dir ("
                        + "; ".join(f"{n}: {e['title_id']:016x}, {e['data_dir']}" for n, e in sorted(EDITIONS.items()))
                        + "); with --update, a base of the other edition is refused")
    p.add_argument("--title-id", type=lambda s: int(s, 16), default=None,
                   help=f"default: the --edition's, else {DEFAULT_TITLE_ID:016x}; {FORWARDER_TITLE_ID:016x} replaces "
                        f"the forwarder; ignored by --update (the base's title ID is used)")
    p.add_argument("--data-dir", default=None,
                   help=f"writable SD folder of the installed game (saves, cache, logs); default: with --update the "
                        f"base's, else the --edition's, else {DEFAULT_DATA_DIR}")
    p.add_argument("--name", help="title name shown on the HOME menu (default: the NRO's)")
    p.add_argument("--version", type=int, default=0,
                   help="full: title version (CNMT), default 0; --update: update number N (title version N*0x10000)")
    p.add_argument("--no-dlc", action="store_true", help="leave masseffect/0000000000000000/ out")
    p.add_argument("--split", action="store_true", help="FAT32: write a folder of 4 GiB parts (00, 01, ...)")
    p.add_argument("--emulator-compatible", action="store_true",
                   help="NPDM that yuzu-based emulators (Eden v0.2.1 and older) accept: force_debug in the "
                        "pre-19.0.0 DebugFlags bit; for emulator testing only, not for the console")
    p.add_argument("--update", action="store_true",
                   help="make an update (patch title = base + 0x800) that stores only what changed against --base")
    p.add_argument("--base", help="with --update: the base NSP (decrypted with --keys) or its .basemeta.json")
    p.add_argument("--program-only", action="store_true",
                   help="with --update: only a new program (--nro); the RomFS (game data, shaders, settings, prewarm "
                        "list) stays the base's, so no --input is needed")
    p.add_argument("--display-version", help="version string shown by the HOME menu (NACP), e.g. 1.1")
    p.add_argument("--sign-key", help=argparse.SUPPRESS)  # PEM RSA-2048 key for reproducible tests
    p.add_argument("--created-utc", help=argparse.SUPPRESS)  # fixed marker timestamp for reproducible tests
    args = p.parse_args(argv)
    if args.data_dir is not None and not valid_data_dir(args.data_dir):
        p.error("--data-dir must look like sdmc:/switch/<folder>")
    try:
        build(args)
    except PackError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
