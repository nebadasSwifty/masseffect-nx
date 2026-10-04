#!/usr/bin/env bash
# Build a 39-bit full-application forwarder for the NRO installed on the SD card.
# Usage: SWITCH_PROD_KEYS=/path/prod.keys tools/build_nsp.sh [file.nro] [output-directory]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NRO="${1:-$ROOT/out/nx/masseffect-nx.nro}"
OUT="${2:-$ROOT/out/release}"
KEYS="${SWITCH_PROD_KEYS:-$HOME/.switch/prod.keys}"
[[ -f "$KEYS" ]] || { echo 'error: set SWITCH_PROD_KEYS to your own prod.keys' >&2; exit 1; }
[[ -f "$NRO" ]] || { echo "error: NRO not found: $NRO" >&2; exit 1; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$NRO" "$WORK/app.nro"
# Pin the loader and packer. Download sources, not opaque precompiled forwarders.
git clone -q https://github.com/Skywalker25/Forwarder-Mod.git "$WORK/loader"
git -C "$WORK/loader" checkout -q 9853bd9167d5a7f0c5fdf935864245c1fbcf8282
git clone -q https://github.com/rlaphoenix/hacBrewPack.git "$WORK/packer"
git -C "$WORK/packer" checkout -q 745b16ecfc9ce055743067d200572204cb2aac6c
# Mount the key file read-only. It never enters the build tree or release artifacts.
KEYS="$(cd "$(dirname "$KEYS")" && pwd)/$(basename "$KEYS")"
docker run --rm -v "$WORK:/work" -v "$OUT:/output" -v "$KEYS:/run/keys/prod.keys:ro" \
  -w /work "${DEVKITA64_IMAGE:-devkitpro/devkita64:latest}" bash -euo pipefail -c '
    python3 - <<"PY"
import json, pathlib, struct
root = pathlib.Path("/work")
p = root / "loader/hbl.json"
j = json.loads(p.read_text())
j["name"] = "Mass Effect"
for key in ("title_id", "title_id_range_min", "title_id_range_max"):
    j[key] = "0x01a5eec700000000"
for old, new in (("title_id", "program_id"), ("title_id_range_min", "program_id_range_min"), ("title_id_range_max", "program_id_range_max")):
    j[new] = j[old]
for capability in j["kernel_capabilities"]:
    if capability["type"] == "debug_flags":
        capability["value"] = {"allow_debug": False, "force_debug": True, "force_debug_prod": False}
source = root / "loader/source/main.c"
source.write_text(source.read_text().replace("void NORETURN ", "void __attribute__((noreturn)) "))
j["address_space_type"] = 3  # AddressSpace64Bit: 39 bits; type 1 is only 36 bits.
p.write_text(json.dumps(j, indent=2))
nro = (root / "app.nro").read_bytes()
if nro[0x10:0x14] != b"NRO0":
    raise SystemExit("Invalid NRO header")
base = struct.unpack_from("<I", nro, 0x18)[0]
if nro[base:base+4] != b"ASET":
    raise SystemExit("NRO must contain an icon and NACP")
control = root / "control"
control.mkdir()
for name, pos in (("icon_AmericanEnglish.dat", 8), ("control.nacp", 24)):
    offset, size = struct.unpack_from("<QQ", nro, base+pos)
    if not size or base+offset+size > len(nro):
        raise SystemExit("Invalid NRO assets")
    (control / name).write_bytes(nro[base+offset:base+offset+size])
nacp = bytearray((control / "control.nacp").read_bytes())
if len(nacp) != 0x4000:
    raise SystemExit("Invalid NACP size")
# The homebrew uses files on SD, not Horizon account saves.
nacp[0x3025] = 0  # StartupUserAccount: None
nacp[0x3034] = 0  # screenshots enabled
nacp[0x3035] = 2  # video capture enabled
nacp[0x3080:0x3090] = bytes(16)
(control / "control.nacp").write_bytes(nacp)
romfs = root / "romfs"
romfs.mkdir()
for name in ("nextNroPath", "nextArgv"):
    (romfs / name).write_text("sdmc:/switch/masseffect-nx/masseffect-nx.nro")
PY
    make -C loader -j2 RELEASE=1 ROMFS=
    mkdir -p exefs
    elf2nso loader/hbl.elf exefs/main
    npdmtool loader/hbl.json exefs/main.npdm
    cp packer/config.mk.template packer/config.mk
    make -C packer -j2
    # Keep packer diagnostics out of CI logs (it reads console keys).
    if ! packer/hacbrewpack --titleid 01a5eec700000000 -k /run/keys/prod.keys \
        --nologo --keygeneration 1 --nspdir /output > /work/pack.log 2>&1; then
      echo "error: hacBrewPack failed; check that header_key and key_area_key_application_00 are present" >&2
      exit 1
    fi
    test -s /output/01a5eec700000000.nsp
    mv /output/01a5eec700000000.nsp /output/masseffect-nx-forwarder.nsp
    chmod a+r /output/masseffect-nx-forwarder.nsp
  '
echo "Built $OUT/masseffect-nx-forwarder.nsp (39-bit, title ID 01a5eec700000000)"
