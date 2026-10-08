#!/usr/bin/env python3
"""tools/build_full_nsp.py on a synthetic installer output with throwaway random keys (no game data, no real keys).

The NSP is taken apart again by an independent reader written from the format descriptions (switchbrew, hactool):
header decryption, key area, AES-CTR sections, PFS0 hash tables, IVFC levels, a RomFS lookup that follows the hash
chains the way libnx's romfs device does, the CNMT and the NCA header signature against the NPDM's ACID key.

With HACTOOL=/path/to/hactool set, hactool also verifies every NCA (-y).
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest

try:
    import cryptography  # noqa: F401
except ImportError:
    print("needs the Python package 'cryptography' (python3 -m pip install cryptography)")
    sys.exit(77)  # tests/run_all.sh: skipped

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import build_full_nsp as b  # noqa: E402

from cryptography.exceptions import InvalidSignature  # noqa: E402
from cryptography.hazmat.primitives import hashes  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import padding, rsa  # noqa: E402
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # noqa: E402

TITLE = 0x01A5EEC700010000


# ---- synthetic inputs ------------------------------------------------------------------------------------------------

def make_nro(name="Mass Effect"):
    text = bytearray(os.urandom(0x3000))
    text[0:0x10] = b"\x20\x00\x00\x14" + b"\0\0\0\0" + b"HOMEBREW"
    ro = os.urandom(0x2000)
    data = os.urandom(0x1000)
    segs = [(0, len(text)), (0x3000, len(ro)), (0x5000, len(data))]
    size = 0x6000
    header = struct.pack("<4sIII", b"NRO0", 0, size, 0)
    for off, n in segs:
        header += struct.pack("<II", off, n)
    header += struct.pack("<II", 0x4000, 0) + bytes(range(32)) + bytes(0x20)
    text[0x10:0x80] = header
    body = bytes(text) + ro + data
    icon = b"\xff\xd8\xff\xe0" + os.urandom(1000)
    nacp = bytearray(0x4000)
    nacp[0:len(name)] = name.encode()
    nacp[0x3025] = 1
    nacp[0x3080:0x3090] = b"\x11" * 16
    aset = struct.pack("<4sI", b"ASET", 0)
    off = 0x38
    aset += struct.pack("<QQ", off, len(icon)) + struct.pack("<QQ", off + len(icon), len(nacp)) + struct.pack("<QQ", 0, 0)
    return body + aset + icon + bytes(nacp)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


def make_input(root):
    files = {
        "masseffect.toml": b"dlc_enable = true\n",
        "masseffect_shaders.mesp": os.urandom(70000),
        "masseffect_shaders.mesp.idx": os.urandom(333),
        "masseffect_prewarm_list.bin": b"NFPL" + struct.pack("<III", 5, 4, 3) + os.urandom(12),
        "game_root/default.xex": b"XEX2" + os.urandom(5000),
        "game_root/Layer0/BIOGame/CookedXenon/Startup_INT.xxx": os.urandom(b.IVFC_BLOCK * 3 + 17),
        "game_root/Layer0/BIOGame/CookedXenon/BIOA_PRO10.xxx": os.urandom(40000),
        "game_root/Layer0/BIOGame/Config/abcd": b"four",
        "game_root/Layer1/Movies/empty.bik": b"",
        "game_root/Layer1/a": b"x",
        "masseffect/0000000000000000/4D5307E8/00000002/BDTS/AutoLoad.ini": b"[Packages]\n",
        "masseffect/0000000000000000/4D5307E8/Headers/00000002/BDTS.header": os.urandom(0x971A),
    }
    junk = {
        "masseffect/E000000000000001/4D5307E8/00000001/save.sav": b"save",
        "logs/rex/x.log": b"log",
        "cache/vfs_index_game_root.bin": b"idx",
        "game_root/._default.xex": b"apple double",
        "game_root/.DS_Store": b"ds",
    }
    for path, data in {**files, **junk}.items():
        write(os.path.join(root, path), data)
    write(os.path.join(root, "masseffect-nx.nro"), make_nro())
    return files, junk


def make_keys(path):
    keys = {"header_key": os.urandom(32), "key_area_key_application_00": os.urandom(16)}
    with open(path, "w") as fh:
        fh.write("master_key_00 = " + os.urandom(16).hex() + "\n")
        for name, value in keys.items():
            fh.write(f"{name} = {value.hex()}\n")
    return keys


# ---- independent reader ----------------------------------------------------------------------------------------------

def read_pfs0(data):
    magic, count, strtab, _ = struct.unpack_from("<4sIII", data, 0)
    assert magic == b"PFS0", magic
    entries = []
    names_off = 0x10 + 0x18 * count
    data_off = names_off + strtab
    for i in range(count):
        off, size, name_off, _ = struct.unpack_from("<QQII", data, 0x10 + 0x18 * i)
        end = data.index(b"\0", names_off + name_off)
        entries.append((data[names_off + name_off:end].decode(), data_off + off, size))
    return entries


class Nca:
    def __init__(self, raw, keys):
        self.raw = raw
        h = bytearray()
        for sector in range(6):
            dec = Cipher(algorithms.AES(keys["header_key"]), modes.XTS(sector.to_bytes(16, "big"))).decryptor()
            h += dec.update(raw[sector * 0x200:(sector + 1) * 0x200]) + dec.finalize()
        self.h = bytes(h)
        assert self.h[0x200:0x204] == b"NCA3"
        self.content_type = self.h[0x205]
        self.size, self.title_id = struct.unpack_from("<QQ", self.h, 0x208)
        crypto = max(self.h[0x206], self.h[0x220])
        assert crypto <= 1 and self.h[0x207] == 0
        ecb = Cipher(algorithms.AES(keys["key_area_key_application_00"]), modes.ECB()).decryptor()
        area = ecb.update(self.h[0x300:0x340]) + ecb.finalize()
        self.ctr_key = area[0x20:0x30]
        self.sections = []
        for i in range(4):
            start, end, present, _ = struct.unpack_from("<IIII", self.h, 0x240 + 0x10 * i)
            if not start and not end:
                continue
            fs = self.h[0x400 + 0x200 * i:0x600 + 0x200 * i]
            assert hashlib.sha256(fs).digest() == self.h[0x280 + 0x20 * i:0x2A0 + 0x20 * i], "FS header hash"
            assert present == 1
            self.sections.append((start * 0x200, end * 0x200, fs))

    def section_plain(self, index):
        start, end, fs = self.sections[index]
        assert fs[4] == 3, "AES-CTR expected"
        ctr = struct.unpack_from("<Q", fs, 0x140)[0]
        iv = ctr.to_bytes(8, "big") + (start >> 4).to_bytes(8, "big")
        dec = Cipher(algorithms.AES(self.ctr_key), modes.CTR(iv)).decryptor()
        return dec.update(self.raw[start:end]) + dec.finalize()

    def pfs0(self, index):
        start, end, fs = self.sections[index]
        assert fs[2] == 1 and fs[3] == 2
        plain = self.section_plain(index)
        block, layers, t_off, t_size, p_off, p_size = struct.unpack_from("<IIQQQQ", fs, 0x28)
        assert layers == 2
        table = plain[t_off:t_off + t_size]
        assert hashlib.sha256(table).digest() == fs[0x08:0x28], "PFS0 master hash"
        pfs0 = plain[p_off:p_off + p_size]
        for i in range(0, len(pfs0), block):
            assert hashlib.sha256(pfs0[i:i + block]).digest() == table[i // block * 32:i // block * 32 + 32]
        assert len(table) == (len(pfs0) + block - 1) // block * 32
        return pfs0

    def romfs(self, index):
        start, end, fs = self.sections[index]
        assert fs[2] == 0 and fs[3] == 3
        plain = self.section_plain(index)
        magic, ident, mh_size, nlevels = struct.unpack_from("<4sIII", fs, 0x08)
        assert (magic, ident, mh_size, nlevels) == (b"IVFC", 0x20000, 0x20, 7)
        levels = [struct.unpack_from("<QQII", fs, 0x18 + 0x18 * i) for i in range(6)]
        hashes_of = None
        for i, (off, size, log2, _) in enumerate(levels):
            bs = 1 << log2
            data = plain[off:off + size]
            assert len(data) == size
            expected = fs[0xC8:0xE8] if i == 0 else plain[levels[i - 1][0]:levels[i - 1][0] + levels[i - 1][1]]
            got = b""
            for j in range(0, size, bs):
                blk = data[j:j + bs]
                got += hashlib.sha256(blk + bytes(bs - len(blk))).digest()  # hactool: full blocks
            assert got == expected[:len(got)] and len(got) == len(expected), f"IVFC level {i + 1}"
            hashes_of = data
        assert end - start >= levels[-1][0] + levels[-1][1]
        return hashes_of


class RomfsReader:
    """Looks paths up through the hash tables, like libnx's romfs device."""

    def __init__(self, image):
        self.img = image
        (hs, self.dh_off, self.dh_size, self.dt_off, self.dt_size, self.fh_off, self.fh_size, self.ft_off,
         self.ft_size, self.data_off) = struct.unpack_from("<10Q", image, 0)
        assert hs == 0x50

    @staticmethod
    def calc_hash(parent, name):
        h = parent ^ 123456789
        for c in name:
            h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
            h ^= c
        return h

    def dir_entry(self, off):
        parent, sibling, child, file, nxt, nlen = struct.unpack_from("<6I", self.img, self.dt_off + off)
        name = self.img[self.dt_off + off + 0x18:self.dt_off + off + 0x18 + nlen]
        return parent, sibling, child, file, nxt, name

    def file_entry(self, off):
        parent, sibling, doff, size, nxt, nlen = struct.unpack_from("<IIQQII", self.img, self.ft_off + off)
        name = self.img[self.ft_off + off + 0x20:self.ft_off + off + 0x20 + nlen]
        return parent, sibling, doff, size, nxt, name

    def find_dir(self, parent, name):
        count = self.dh_size // 4
        off = struct.unpack_from("<I", self.img, self.dh_off + 4 * (self.calc_hash(parent, name) % count))[0]
        while off != 0xFFFFFFFF:
            p, _, _, _, nxt, n = self.dir_entry(off)
            if p == parent and n == name:
                return off
            off = nxt
        return None

    def read(self, path):
        parts = [p.encode() for p in path.split("/")]
        cur = 0
        for part in parts[:-1]:
            cur = self.find_dir(cur, part)
            if cur is None:
                return None
        count = self.fh_size // 4
        off = struct.unpack_from("<I", self.img, self.fh_off + 4 * (self.calc_hash(cur, parts[-1]) % count))[0]
        while off != 0xFFFFFFFF:
            p, _, doff, size, nxt, n = self.file_entry(off)
            if p == cur and n == parts[-1]:
                return self.img[self.data_off + doff:self.data_off + doff + size]
            off = nxt
        return None

    def walk(self, off=0, prefix=""):
        """Every file path, through the child/sibling links (what readdir does)."""
        _, _, child, file, _, _ = self.dir_entry(off)
        out = []
        while file != 0xFFFFFFFF:
            _, sibling, _, _, _, name = self.file_entry(file)
            out.append(prefix + name.decode())
            file = sibling
        while child != 0xFFFFFFFF:
            _, sibling, _, _, _, name = self.dir_entry(child)
            out += self.walk(child, prefix + name.decode() + "/")
            child = sibling
        return out


def run_packer(input_dir, keys_path, output, **extra):
    argv = ["--input", input_dir, "--keys", keys_path, "--output", output]
    for k, v in extra.items():
        argv += [f"--{k.replace('_', '-')}"] + ([] if v is True else [str(v)])
    # Same parsing as the command line, without exiting the test process.
    import io
    import contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        rc = b.main(argv)
    return rc


class FullNspTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = cls.tmp.name
        cls.input = os.path.join(cls.dir, "masseffect-nx")
        cls.files, cls.junk = make_input(cls.input)
        cls.keys_path = os.path.join(cls.dir, "prod.keys")
        cls.keys = make_keys(cls.keys_path)
        cls.nsp = os.path.join(cls.dir, "out.nsp")
        assert run_packer(cls.input, cls.keys_path, cls.nsp, data_dir="sdmc:/switch/me-test") == 0
        with open(cls.nsp, "rb") as fh:
            cls.raw = fh.read()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def ncas(self, raw=None):
        raw = raw or self.raw
        out = {}
        for name, off, size in read_pfs0(raw):
            blob = raw[off:off + size]
            self.assertTrue(name.startswith(hashlib.sha256(blob).hexdigest()[:32]), name)
            out[name] = Nca(blob, self.keys)
        return out

    def by_type(self, ncas, ctype):
        (nca,) = [n for n in ncas.values() if n.content_type == ctype]
        return nca

    def test_container(self):
        entries = read_pfs0(self.raw)
        self.assertEqual(len(entries), 3)
        self.assertEqual(sum(1 for n, _, _ in entries if n.endswith(".cnmt.nca")), 1)
        self.assertEqual(entries[-1][1] + entries[-1][2], len(self.raw))
        for nca in self.ncas().values():
            self.assertEqual(nca.title_id, TITLE)
            self.assertEqual(nca.size, len(nca.raw))

    def test_program_exefs(self):
        program = self.by_type(self.ncas(), b.CONTENT_PROGRAM)
        exefs = program.pfs0(0)
        files = {n: exefs[o:o + s] for n, o, s in read_pfs0(exefs)}
        self.assertEqual(sorted(files), ["main", "main.npdm"])
        nso = files["main"]
        self.assertEqual(nso[:4], b"NSO0")
        with open(os.path.join(self.input, "masseffect-nx.nro"), "rb") as fh:
            nro = b.Nro(fh.read())
        for i in range(3):
            foff, moff, size, _ = struct.unpack_from("<IIII", nso, 0x10 + 0x10 * i)
            seg = nso[foff:foff + size]
            self.assertEqual(hashlib.sha256(seg).digest(), nso[0xA0 + 0x20 * i:0xC0 + 0x20 * i])
            self.assertEqual(moff, nro.segments[i][0])
            if i == 0:
                self.assertEqual(seg[0x10:0x80], bytes(0x70))
                self.assertEqual(seg[0x80:], nro.data[0x80:0x3000])
        npdm = files["main.npdm"]
        self.assertEqual(npdm[:4], b"META")
        self.assertEqual((npdm[0x0C] >> 1) & 3, 3, "39-bit address space")
        aci0_off, _, acid_off, _ = struct.unpack_from("<IIII", npdm, 0x70)
        self.assertEqual(struct.unpack_from("<Q", npdm, aci0_off + 0x10)[0], TITLE)
        self.assertEqual(struct.unpack_from("<QQ", npdm, acid_off + 0x210), (TITLE, TITLE))
        # NCA header signature 2 verifies with the ACID public key
        modulus = int.from_bytes(npdm[acid_off + 0x100:acid_off + 0x200], "big")
        pub = rsa.RSAPublicNumbers(65537, modulus).public_key()
        try:
            pub.verify(program.h[0x100:0x200], program.h[0x200:0x400],
                       padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        except InvalidSignature:
            self.fail("NCA header signature 2 does not verify with the ACID key")

    def test_program_romfs(self):
        program = self.by_type(self.ncas(), b.CONTENT_PROGRAM)
        image = program.romfs(1)
        fs = RomfsReader(image)
        for path, data in self.files.items():
            self.assertEqual(fs.read(path), data, path)
        for path in self.junk:
            self.assertIsNone(fs.read(path), path)
        walked = set(fs.walk())
        self.assertEqual(walked, set(self.files) | {b.MARKER_NAME})
        marker = fs.read(b.MARKER_NAME).decode()
        self.assertIn("data_dir=sdmc:/switch/me-test\n", marker)
        self.assertIn(f"title_id={TITLE:016x}\n", marker)
        self.assertIsNone(fs.read("masseffect-nx.nro"))

    def test_control(self):
        control = self.by_type(self.ncas(), b.CONTENT_CONTROL)
        fs = RomfsReader(control.romfs(0))
        nacp = fs.read("control.nacp")
        self.assertEqual(len(nacp), 0x4000)
        self.assertEqual(nacp[:11], b"Mass Effect")
        self.assertEqual(nacp[0x3025], 0)
        self.assertEqual(nacp[0x3035], 2)
        self.assertEqual(nacp[0x3080:0x3090], bytes(16))
        self.assertEqual(fs.read("icon_AmericanEnglish.dat")[:4], b"\xff\xd8\xff\xe0")

    def test_cnmt(self):
        ncas = self.ncas()
        meta = self.by_type(ncas, b.CONTENT_META)
        pfs = meta.pfs0(0)
        ((name, off, size),) = read_pfs0(pfs)
        self.assertEqual(name, f"Application_{TITLE:016x}.cnmt")
        cnmt = pfs[off:off + size]
        tid, ver, mtype, _, ext, count, metas = struct.unpack_from("<QIBBHHH", cnmt, 0)
        self.assertEqual((tid, mtype, ext, count, metas), (TITLE, 0x80, 0x10, 2, 0))
        self.assertEqual(struct.unpack_from("<Q", cnmt, 0x20)[0], TITLE + 0x800)
        want = {n.content_type: (hashlib.sha256(n.raw).digest(), len(n.raw)) for n in ncas.values()}
        types = {b.CNMT_PROGRAM: b.CONTENT_PROGRAM, b.CNMT_CONTROL: b.CONTENT_CONTROL}
        for i in range(count):
            rec = cnmt[0x30 + 0x38 * i:0x30 + 0x38 * (i + 1)]
            digest, ncaid, size, ctype = rec[:32], rec[32:48], int.from_bytes(rec[48:54], "little"), rec[54]
            self.assertEqual((digest, size), want[types[ctype]])
            self.assertEqual(ncaid, digest[:16])

    def test_split_output(self):
        old = b.FAT32_PART
        b.FAT32_PART = 0x10000
        try:
            out = os.path.join(self.dir, "split.nsp")
            self.assertEqual(run_packer(self.input, self.keys_path, out, split=True, no_dlc=True), 0)
            parts = sorted(os.listdir(out))
            self.assertGreater(len(parts), 3)
            self.assertEqual(parts, [f"{i:02d}" for i in range(len(parts))])
            sizes = [os.path.getsize(os.path.join(out, p)) for p in parts]
            self.assertTrue(all(s == 0x10000 for s in sizes[:-1]) and 0 < sizes[-1] <= 0x10000)
            raw = b""
            for part in parts:
                with open(os.path.join(out, part), "rb") as fh:
                    raw += fh.read()
            program = self.by_type(self.ncas(raw), b.CONTENT_PROGRAM)
            fs = RomfsReader(program.romfs(1))
            self.assertIsNone(fs.read("masseffect/0000000000000000/4D5307E8/00000002/BDTS/AutoLoad.ini"))
            self.assertEqual(fs.read("game_root/default.xex"), self.files["game_root/default.xex"])
        finally:
            b.FAT32_PART = old

    def test_bad_inputs(self):
        bad_keys = os.path.join(self.dir, "bad.keys")
        with open(bad_keys, "w") as fh:
            fh.write("header_key = 00\n")
        self.assertEqual(run_packer(self.input, bad_keys, os.path.join(self.dir, "x.nsp")), 1)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "x.nsp")))
        empty = os.path.join(self.dir, "empty")
        os.makedirs(empty, exist_ok=True)
        self.assertEqual(run_packer(empty, self.keys_path, os.path.join(self.dir, "y.nsp")), 1)

    def test_invalid_toml_refused(self):
        import shutil
        bad = os.path.join(self.dir, "badtoml", "masseffect-nx")
        shutil.copytree(self.input, bad)
        with open(os.path.join(bad, "masseffect.toml"), "ab") as fh:
            fh.write(b"\ndlc_enable = false\n")  # the same key twice: the game would ignore the whole file
        self.assertEqual(run_packer(bad, self.keys_path, os.path.join(self.dir, "bad.nsp")), 1)

    def test_prewarm_list_optional(self):
        import contextlib
        import io
        import shutil
        bare = os.path.join(self.dir, "nolist", "masseffect-nx")
        shutil.copytree(self.input, bare)
        os.remove(os.path.join(bare, b.PREWARM_LIST_NAME))
        out = os.path.join(self.dir, "nolist.nsp")
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            self.assertEqual(b.main(["--input", bare, "--keys", self.keys_path, "--output", out]), 0)
        self.assertIn(f"no {b.PREWARM_LIST_NAME}", log.getvalue())
        with open(out, "rb") as fh:
            program = self.by_type(self.ncas(fh.read()), b.CONTENT_PROGRAM)
        fs = RomfsReader(program.romfs(1))
        self.assertIsNone(fs.read(b.PREWARM_LIST_NAME))
        self.assertEqual(fs.read("masseffect.toml"), self.files["masseffect.toml"])

    def test_romfs_table_count(self):
        self.assertEqual([b.romfs_table_count(n) for n in (0, 2, 3, 18, 19, 20, 100)], [3, 3, 3, 19, 19, 23, 101])

    @unittest.skipUnless(os.environ.get("HACTOOL"), "set HACTOOL=/path/to/hactool for the external check")
    def test_hactool_verify(self):
        hactool = os.environ["HACTOOL"]
        checked_program = False
        for name, off, size in read_pfs0(self.raw):
            path = os.path.join(self.dir, name)
            with open(path, "wb") as fh:
                fh.write(self.raw[off:off + size])
            r = subprocess.run([hactool, "-k", self.keys_path, "-y", path], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            # Expected failures: the Nintendo fixed-key NCA signature and the ACID signature of the NPDM (both need
            # Nintendo's private keys; the console accepts them with the same signature patches as the forwarder).
            fails = [line for line in r.stdout.splitlines()
                     if "FAIL" in line and "Fixed-Key Signature" not in line and line.strip().split()[0] != "Signature"]
            self.assertEqual(fails, [], f"{name}:\n{r.stdout}")
            if "Content Type:                       Program" in r.stdout:
                self.assertIn("NPDM Signature (GOOD)", r.stdout)  # checked against the ExeFS main.npdm ACID key
                checked_program = True
        self.assertTrue(checked_program)


class NpdmCapabilityTest(unittest.TestCase):
    @staticmethod
    def kernel_caps(npdm):
        aci0_off = struct.unpack_from("<I", npdm, 0x70)[0]
        kac_off, kac_size = struct.unpack_from("<II", npdm, aci0_off + 0x30)
        return struct.unpack_from(f"<{kac_size // 4}I", npdm, aci0_off + kac_off)

    @staticmethod
    def debug_flags(caps):
        return [c for c in caps if (~c & (c + 1)) - 1 == 0xFFFF]  # DebugFlags capability id 16

    def test_console_debug_flags(self):
        caps = self.kernel_caps(b.build_npdm(b.npdm_config(TITLE)))
        self.assertEqual(self.debug_flags(caps), [0xFFFF | 1 << 19])  # 19.0.0 layout: force_debug = bit 19

    def test_emulator_compatible(self):
        console = self.kernel_caps(b.build_npdm(b.npdm_config(TITLE)))
        emu_npdm = b.build_npdm(b.npdm_emulator_compatible(b.npdm_config(TITLE)))
        emu = self.kernel_caps(emu_npdm)
        # yuzu/Eden <= v0.2.1 reject DebugFlags with any bit 19..31 set; force_debug moves to the old bit 18.
        self.assertEqual(self.debug_flags(emu), [0xFFFF | 1 << 18])
        self.assertEqual([c for c in emu if c not in self.debug_flags(emu)],
                         [c for c in console if c not in self.debug_flags(console)])
        self.assertEqual((emu_npdm[0x0C] >> 1) & 3, 3, "39-bit address space")



# ---- updates (patch NSPs with a BKTR RomFS) ----------------------------------------------------------------------------

def read_bucket_tree(table, entry_size):
    """[(offset, entry bytes)] of a bucket tree (16 KiB nodes): L1 node, then the entry sets."""
    index, sets, end = struct.unpack_from("<iiq", table, 0)
    assert index == 0 and sets >= 1
    starts = struct.unpack_from(f"<{sets}q", table, 0x10)
    entries = []
    for i in range(sets):
        node = table[0x4000 * (i + 1):0x4000 * (i + 2)]
        n_index, count, n_end = struct.unpack_from("<iiq", node, 0)
        assert n_index == i and count >= 1
        assert n_end == (starts[i + 1] if i + 1 < sets else end)
        for k in range(count):
            e = node[0x10 + entry_size * k:0x10 + entry_size * (k + 1)]
            entries.append((struct.unpack_from("<q", e, 0)[0], e))
        assert entries[-count][0] == starts[i]
    return entries, end


def patch_virtual_section(patch, base):
    """Rebuilds the virtual RomFS section of a patch Program NCA from its BKTR tables and the base Program NCA."""
    start, end, fs = patch.sections[1]
    assert fs[2:5] == bytes([0, 3, 4]), "RomFS, IVFC, AesCtrEx"
    ind_off, ind_size, ind_hdr = struct.unpack_from("<qq16s", fs, 0x100)
    aes_off, aes_size, aes_hdr = struct.unpack_from("<qq16s", fs, 0x120)
    assert ind_hdr[:4] == aes_hdr[:4] == b"BKTR"
    assert ind_off % 0x200 == 0 and ind_off + ind_size <= aes_off and aes_off + aes_size == end - start
    generation, secure = struct.unpack_from("<II", fs, 0x140)
    raw = patch.raw[start:end]

    def ctr(upper, data, offset):
        iv = struct.pack(">QQ", upper, (start + offset) >> 4)
        dec = Cipher(algorithms.AES(patch.ctr_key), modes.CTR(iv)).decryptor()
        return dec.update(data) + dec.finalize()

    aes_table = ctr((secure << 32) | generation, raw[aes_off:aes_off + aes_size], aes_off)
    aes_entries, aes_end = read_bucket_tree(aes_table, 0x10)
    assert aes_end == aes_off and aes_entries[0][0] == 0
    assert struct.unpack_from("<I", aes_hdr, 8)[0] == len(aes_entries)
    physical = bytearray()
    for i, (off, e) in enumerate(aes_entries):
        nxt = aes_entries[i + 1][0] if i + 1 < len(aes_entries) else aes_off
        _, enc, gen = struct.unpack("<qB3xi", e)
        assert enc == 0 and off % 16 == 0
        physical += ctr((secure << 32) | gen, raw[off:nxt], off)
    ind_entries, virtual_size = read_bucket_tree(bytes(physical[ind_off:ind_off + ind_size]), 0x14)
    assert struct.unpack_from("<I", ind_hdr, 8)[0] == len(ind_entries)
    base_plain = base.section_plain(1)
    virtual = bytearray()
    counts = {0: 0, 1: 0}
    for i, (virt, e) in enumerate(ind_entries):
        nxt = ind_entries[i + 1][0] if i + 1 < len(ind_entries) else virtual_size
        v, ph, storage = struct.unpack("<qqi", e)
        assert v == len(virtual) and v % 16 == 0 and ph % 16 == 0
        src = base_plain if storage == 0 else physical
        virtual += src[ph:ph + nxt - v]
        counts[storage] += nxt - v
    assert len(virtual) == virtual_size
    return bytes(virtual), counts


def ivfc_check(fs, image):
    levels = [struct.unpack_from("<QQII", fs, 0x18 + 0x18 * i) for i in range(6)]
    for i, (off, size, log2, _) in enumerate(levels):
        bs = 1 << log2
        data = image[off:off + size]
        expected = fs[0xC8:0xE8] if i == 0 else image[levels[i - 1][0]:levels[i - 1][0] + levels[i - 1][1]]
        got = b"".join(hashlib.sha256(data[j:j + bs] + bytes(bs - len(data[j:j + bs]))).digest()
                       for j in range(0, size, bs))
        assert got == expected, f"IVFC level {i + 1}"
    return image[levels[-1][0]:levels[-1][0] + levels[-1][1]]


class UpdateNspTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import shutil
        cls.tmp = tempfile.TemporaryDirectory()
        d = cls.dir = cls.tmp.name
        cls.keys_path = os.path.join(d, "prod.keys")
        cls.keys = make_keys(cls.keys_path)
        cls.base_in = os.path.join(d, "base", "masseffect-nx")
        make_input(cls.base_in)
        big = os.urandom(5 * b.PATCH_CHUNK + 1234)
        write(os.path.join(cls.base_in, "game_root/Layer1/big.bin"), big)
        write(os.path.join(cls.base_in, "game_root/Layer1/removed.bin"), os.urandom(3000))
        cls.base_nsp = os.path.join(d, "base.nsp")
        assert run_packer(cls.base_in, cls.keys_path, cls.base_nsp, data_dir="sdmc:/switch/me-test") == 0
        # Version 1: one chunk of big.bin changed, toml changed, a file added and one removed, a new program.
        cls.new_in = os.path.join(d, "new", "masseffect-nx")
        shutil.copytree(cls.base_in, cls.new_in)
        changed = bytearray(big)
        changed[2 * b.PATCH_CHUNK + 100:2 * b.PATCH_CHUNK + 200] = os.urandom(100)
        write(os.path.join(cls.new_in, "game_root/Layer1/big.bin"), bytes(changed))
        write(os.path.join(cls.new_in, "masseffect.toml"), b"dlc_enable = true\nnew_setting = 1\n")
        write(os.path.join(cls.new_in, "game_root/Layer1/added.bin"), os.urandom(777))
        cls.new_list = b"NFPL" + struct.pack("<III", 5, 4, 4) + os.urandom(16)
        write(os.path.join(cls.new_in, b.PREWARM_LIST_NAME), cls.new_list)
        os.remove(os.path.join(cls.new_in, "game_root/Layer1/removed.bin"))
        write(os.path.join(cls.new_in, "masseffect-nx.nro"), make_nro())
        common = dict(update=True, version=1, data_dir="sdmc:/switch/me-test", created_utc="2026-10-08T00:00:00Z")
        cls.update_nsp = os.path.join(d, "update.nsp")
        assert run_packer(cls.new_in, cls.keys_path, cls.update_nsp, base=cls.base_nsp + ".basemeta.json",
                          **common) == 0
        cls.update_from_nsp = os.path.join(d, "update2.nsp")
        assert run_packer(cls.new_in, cls.keys_path, cls.update_from_nsp, base=cls.base_nsp, **common) == 0
        # The same input packed in full, for a byte comparison of the virtual RomFS section.
        cls.full_new = os.path.join(d, "full_new.nsp")
        assert run_packer(cls.new_in, cls.keys_path, cls.full_new, version=0x10000, data_dir="sdmc:/switch/me-test",
                          created_utc="2026-10-08T00:00:00Z") == 0
        # Program only: a new NRO, no --input; the RomFS stays the base's.
        cls.new_nro = os.path.join(d, "new.nro")
        write(cls.new_nro, make_nro())
        cls.program_only = os.path.join(d, "update_program_only.nsp")
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            assert b.main(["--update", "--program-only", "--version", "2", "--nro", cls.new_nro, "--keys",
                           cls.keys_path, "--base", cls.base_nsp + ".basemeta.json", "--output", cls.program_only]) == 0
        cls.program_only_from_nsp = os.path.join(d, "update_program_only2.nsp")
        with contextlib.redirect_stdout(io.StringIO()):
            assert b.main(["--update", "--program-only", "--version", "2", "--nro", cls.new_nro, "--keys",
                           cls.keys_path, "--base", cls.base_nsp, "--output", cls.program_only_from_nsp]) == 0
        cls.raw = {}
        for name in ("base_nsp", "update_nsp", "update_from_nsp", "full_new", "program_only", "program_only_from_nsp"):
            with open(getattr(cls, name), "rb") as fh:
                cls.raw[name] = fh.read()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def ncas(self, name):
        raw = self.raw[name]
        out = {}
        for n, off, size in read_pfs0(raw):
            blob = raw[off:off + size]
            self.assertTrue(n.startswith(hashlib.sha256(blob).hexdigest()[:32]))
            nca = Nca(blob, self.keys)
            out[nca.content_type] = (n, nca)
        return out

    def test_patch_cnmt(self):
        ncas = self.ncas("update_nsp")
        meta = ncas[b.CONTENT_META][1]
        self.assertEqual(meta.title_id, TITLE + 0x800)
        pfs = meta.pfs0(0)
        ((name, off, size),) = read_pfs0(pfs)
        self.assertEqual(name, f"Patch_{TITLE + 0x800:016x}.cnmt")
        cnmt = pfs[off:off + size]
        tid, ver, mtype, _, ext, count, metas = struct.unpack_from("<QIBBHHH", cnmt, 0)
        self.assertEqual((tid, ver, mtype, ext, count, metas), (TITLE + 0x800, 0x10000, 0x81, 0x18, 2, 0))
        app_id, sysver, ext_data = struct.unpack_from("<QII", cnmt, 0x20)
        self.assertEqual((app_id, ext_data), (TITLE, 0))
        for i in range(count):
            rec = cnmt[0x38 + 0x38 * i:0x38 + 0x38 * (i + 1)]
            ctype = {1: b.CONTENT_PROGRAM, 3: b.CONTENT_CONTROL}[rec[54]]
            self.assertEqual(rec[:32], hashlib.sha256(ncas[ctype][1].raw).digest())
        self.assertEqual(ncas[b.CONTENT_PROGRAM][1].title_id, TITLE)
        self.assertEqual(ncas[b.CONTENT_CONTROL][1].title_id, TITLE)

    def test_virtual_romfs_equals_full_pack(self):
        base = self.ncas("base_nsp")[b.CONTENT_PROGRAM][1]
        patch = self.ncas("update_nsp")[b.CONTENT_PROGRAM][1]
        full = self.ncas("full_new")[b.CONTENT_PROGRAM][1]
        virtual, counts = patch_virtual_section(patch, base)
        self.assertEqual(virtual, full.section_plain(1))  # byte for byte the RomFS section a full pack would have
        image = ivfc_check(patch.sections[1][2], virtual)
        fs = RomfsReader(image)
        with open(os.path.join(self.new_in, "game_root/Layer1/big.bin"), "rb") as fh:
            self.assertEqual(fs.read("game_root/Layer1/big.bin"), fh.read())
        self.assertEqual(fs.read("game_root/Layer1/added.bin") is not None, True)
        self.assertIsNone(fs.read("game_root/Layer1/removed.bin"))
        self.assertIn(b"new_setting", fs.read("masseffect.toml"))
        self.assertEqual(fs.read(b.PREWARM_LIST_NAME), self.new_list)  # a new release's list reaches the update
        # Most of the game data comes from the base: all of big.bin but one 64 KiB chunk, the unchanged files.
        self.assertGreater(counts[0], 5 * b.PATCH_CHUNK)
        self.assertLess(counts[1], len(virtual) - counts[0] + 1)

    def test_base_from_nsp_matches_metadata(self):
        a = self.ncas("update_nsp")[b.CONTENT_PROGRAM][1].sections[1][2]
        c = self.ncas("update_from_nsp")[b.CONTENT_PROGRAM][1].sections[1][2]
        self.assertEqual(a[0x100:0x140], c[0x100:0x140])  # same tables from either source of base metadata
        self.assertEqual(a[0x08:0x100], c[0x08:0x100])

    def test_program_only_update(self):
        base = self.ncas("base_nsp")[b.CONTENT_PROGRAM][1]
        ncas = self.ncas("program_only")
        patch = ncas[b.CONTENT_PROGRAM][1]
        virtual, counts = patch_virtual_section(patch, base)
        self.assertEqual(virtual, base.section_plain(1))  # the base's RomFS section, unchanged
        self.assertEqual(counts[1], 0)
        self.assertEqual(patch.sections[1][2][0x08:0x100], base.sections[1][2][0x08:0x100])  # same IVFC superblock
        start, end, _ = patch.sections[1]
        self.assertEqual(end - start, 0x10000)  # just the two tables (an L1 node and one entry set each)
        with open(self.new_nro, "rb") as fh:
            nso = b.nso_from_nro(b.Nro(fh.read()))
        self.assertEqual(dict((n, patch.pfs0(0)[o:o + sz]) for n, o, sz in read_pfs0(patch.pfs0(0)))["main"], nso)
        meta = ncas[b.CONTENT_META][1]
        ((name, off, size),) = read_pfs0(meta.pfs0(0))
        self.assertEqual(name, f"Patch_{TITLE + 0x800:016x}.cnmt")
        self.assertEqual(struct.unpack_from("<I", meta.pfs0(0), off + 8)[0], 0x20000)
        # From the NSP or from the metadata file: the same section header.
        other = self.ncas("program_only_from_nsp")[b.CONTENT_PROGRAM][1]
        self.assertEqual(other.sections[1][2][0x08:0x140], patch.sections[1][2][0x08:0x140])

    def test_update_needs_base(self):
        self.assertEqual(run_packer(self.new_in, self.keys_path, os.path.join(self.dir, "x.nsp"), update=True,
                                    version=1), 1)
        self.assertEqual(run_packer(self.new_in, self.keys_path, os.path.join(self.dir, "y.nsp"), update=True,
                                    base=self.base_nsp), 1)  # no --version

    @unittest.skipUnless(os.environ.get("HACTOOL"), "set HACTOOL=/path/to/hactool for the external check")
    def test_hactool_applies_patch(self):
        hactool = os.environ["HACTOOL"]
        paths = {}
        for which in ("base_nsp", "update_nsp"):
            n, nca = self.ncas(which)[b.CONTENT_PROGRAM]
            paths[which] = os.path.join(self.dir, which + ".nca")
            with open(paths[which], "wb") as fh:
                fh.write(nca.raw)
        out = os.path.join(self.dir, "hactool_section1.bin")  # hactool saves a patched RomFS section as its image
        r = subprocess.run([hactool, "-k", self.keys_path, "-y", f"--basenca={paths['base_nsp']}",
                            f"--section1={out}", paths["update_nsp"]], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        fails = [line for line in r.stdout.splitlines()
                 if "FAIL" in line and "Fixed-Key Signature" not in line and line.strip().split()[0] != "Signature"]
        self.assertEqual(fails, [], r.stdout)
        full = self.ncas("full_new")[b.CONTENT_PROGRAM][1]
        image = ivfc_check(full.sections[1][2], full.section_plain(1))
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), image)

class EditionTests(unittest.TestCase):
    """--edition: per-edition title ID and data folder, so the English and Russian NSPs install side by side."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = cls.dir = cls.tmp.name
        cls.keys_path = os.path.join(d, "prod.keys")
        cls.keys = make_keys(cls.keys_path)
        cls.input = os.path.join(d, "in", "masseffect-nx")
        make_input(cls.input)
        cls.out = {}
        for ed in ("en", "ru"):
            cls.out[ed] = os.path.join(d, f"{ed}.nsp")
            assert run_packer(cls.input, cls.keys_path, cls.out[ed], edition=ed,
                              created_utc="2026-10-08T00:00:00Z") == 0

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def ncas(self, path):
        with open(path, "rb") as fh:
            raw = fh.read()
        return {nca.content_type: (n, nca) for n, nca in
                ((n, Nca(raw[off:off + size], self.keys)) for n, off, size in read_pfs0(raw))}

    def test_ids_are_distinct(self):
        self.assertEqual(b.EDITIONS["ru"], {"title_id": 0x01A5EEC700010000, "data_dir": "sdmc:/switch/masseffect-nx"})
        self.assertEqual(b.EDITIONS["en"], {"title_id": 0x01A5EEC700020000, "data_dir": "sdmc:/switch/masseffect-nx-en"})
        apps = [b.FORWARDER_TITLE_ID] + [e["title_id"] for e in b.EDITIONS.values()]
        # Each application owns its ID, its patch (+0x800) and its add-on range (+0x1000 .. +0x1FFF).
        spans = sorted((a, a + 0x1FFF) for a in apps)
        for (_, hi), (lo, _) in zip(spans, spans[1:]):
            self.assertLess(hi, lo)
        dirs = [e["data_dir"] for e in b.EDITIONS.values()]
        self.assertEqual(len(set(dirs)), len(dirs))
        self.assertTrue(all(b.valid_data_dir(x) for x in dirs))

    def test_edition_title_and_folder(self):
        for ed, cfg in b.EDITIONS.items():
            ncas = self.ncas(self.out[ed])
            for _, nca in ncas.values():
                self.assertEqual(nca.title_id, cfg["title_id"], ed)
            meta = ncas[b.CONTENT_META][1]
            ((name, _, _),) = read_pfs0(meta.pfs0(0))
            self.assertEqual(name, f"Application_{cfg['title_id']:016x}.cnmt")
            marker = RomfsReader(ncas[b.CONTENT_PROGRAM][1].romfs(1)).read(b.MARKER_NAME).decode()
            self.assertIn(f"data_dir={cfg['data_dir']}\n", marker)
            self.assertIn(f"title_id={cfg['title_id']:016x}\n", marker)
            with open(self.out[ed] + ".basemeta.json", encoding="utf-8") as fh:
                import json
                bm = json.load(fh)
            self.assertEqual((bm["title_id"], bm["data_dir"]), (f"{cfg['title_id']:016x}", cfg["data_dir"]))
            # Read back from the NSP itself: the marker gives the data folder.
            got = b.metadata_from_nsp(self.out[ed], b.load_keys(self.keys_path), log=lambda *_: None)
            self.assertEqual((got["title_id"], got["data_dir"]), (bm["title_id"], cfg["data_dir"]))

    def test_explicit_flags_override_edition(self):
        out = os.path.join(self.dir, "override.nsp")
        self.assertEqual(run_packer(self.input, self.keys_path, out, edition="en", title_id="01a5eec700040000",
                                    data_dir="sdmc:/switch/me-x"), 0)
        ncas = self.ncas(out)
        self.assertEqual(ncas[b.CONTENT_PROGRAM][1].title_id, 0x01A5EEC700040000)
        marker = RomfsReader(ncas[b.CONTENT_PROGRAM][1].romfs(1)).read(b.MARKER_NAME).decode()
        self.assertIn("data_dir=sdmc:/switch/me-x\n", marker)

    def test_update_title_check(self):
        import contextlib
        import io
        nro = os.path.join(self.dir, "new.nro")
        write(nro, make_nro())

        def update(base, edition, out):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
                rc = b.main(["--update", "--program-only", "--version", "1", "--nro", nro, "--keys", self.keys_path,
                             "--base", base, "--output", os.path.join(self.dir, out)] +
                            (["--edition", edition] if edition else []))
            return rc, buf.getvalue()

        # Same edition: patch ID = base + 0x800.
        rc, _ = update(self.out["ru"] + ".basemeta.json", "ru", "u_ru.nsp")
        self.assertEqual(rc, 0)
        meta = self.ncas(os.path.join(self.dir, "u_ru.nsp"))[b.CONTENT_META][1]
        ((name, _, _),) = read_pfs0(meta.pfs0(0))
        self.assertEqual(name, f"Patch_{0x01A5EEC700010800:016x}.cnmt")
        # The other edition's base: refused, with both IDs named, nothing written.
        for base in (self.out["ru"] + ".basemeta.json", self.out["ru"]):
            rc, text = update(base, "en", "u_bad.nsp")
            self.assertEqual(rc, 1)
            self.assertIn("01a5eec700010000", text)
            self.assertIn("01a5eec700020000", text)
            self.assertFalse(os.path.exists(os.path.join(self.dir, "u_bad.nsp")))
        # Without --edition there is no check (legacy bases); a base of no known edition only warns, keeps its ID.
        rc, _ = update(self.out["ru"] + ".basemeta.json", None, "u_none.nsp")
        self.assertEqual(rc, 0)
        custom = os.path.join(self.dir, "custom.nsp")
        self.assertEqual(run_packer(self.input, self.keys_path, custom, title_id="01a5eec700030000"), 0)
        rc, text = update(custom + ".basemeta.json", "en", "u_custom.nsp")
        self.assertEqual(rc, 0)
        self.assertIn("warning: the base has title 01a5eec700030000", text)
        self.assertEqual(self.ncas(os.path.join(self.dir, "u_custom.nsp"))[b.CONTENT_PROGRAM][1].title_id,
                         0x01A5EEC700030000)

    def test_nacp_id_fields(self):
        nacp = bytearray(0x4000)
        for off in b.NACP_ID_FIELDS:
            struct.pack_into("<Q", nacp, off, 0x0100000000ABC000)
        struct.pack_into("<Q", nacp, b.NACP_AOC_BASE, 0x0100000000ABD000)
        out = b.patch_nacp(bytes(nacp), title_id=0x01A5EEC700020000)
        for off in b.NACP_ID_FIELDS:
            self.assertEqual(struct.unpack_from("<Q", out, off)[0], 0x01A5EEC700020000, hex(off))
        self.assertEqual(struct.unpack_from("<Q", out, b.NACP_AOC_BASE)[0], 0x01A5EEC700021000)
        # The port's NROs leave them 0: untouched.
        zero = b.patch_nacp(bytes(0x4000), title_id=0x01A5EEC700020000)
        for off in b.NACP_ID_FIELDS + (b.NACP_AOC_BASE,):
            self.assertEqual(zero[off:off + 8], bytes(8), hex(off))


if __name__ == "__main__":
    unittest.main()
