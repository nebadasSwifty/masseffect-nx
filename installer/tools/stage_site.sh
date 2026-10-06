#!/usr/bin/env bash
# Assembles the static site (what GitHub Pages serves) from the installer sources and the build artifacts.
#
#   installer/tools/stage_site.sh <out dir> [--wasm <dir>] [--releases <dir>]
#
#   --wasm <dir>      the Emscripten outputs (scan/hlsl/pack/dxc_web .mjs + .wasm). Default: installer/wasm
#   --releases <dir>  the release assets (*.nro) to publish next to the page; a manifest.json with their size and
#                     SHA-256 is written. Default: none (the page then reports that no build is published).
#
# The page itself needs no build step: this only copies files and generates releases/manifest.json.
set -eu
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
inst=$(cd "$here/.." && pwd)
root=$(cd "$inst/.." && pwd)
out=${1:?usage: stage_site.sh <out dir> [--wasm <dir>] [--releases <dir>]}
shift
wasm=$inst/wasm
releases=
strict=0
while [ $# -gt 0 ]; do
  case $1 in
    --require-complete) strict=1; shift;;
    --wasm) wasm=$2; shift 2;;
    --releases) releases=$2; shift 2;;
    *) echo "unknown argument $1" >&2; exit 1;;
  esac
done

rm -rf "$out"
mkdir -p "$out/wasm" "$out/releases"
cp "$inst/index.html" "$inst/style.css" "$inst/config.js" "$out/"
cp -R "$inst/js" "$inst/assets" "$out/"
touch "$out/.nojekyll"
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$out/"

# masseffect.toml: the settings file of the build (the page downloads it and puts it into the zip).
cp "$root/app/masseffect.toml" "$out/masseffect.toml"
# The translator's shared header, pasted into every generated HLSL by the translator.
cp "$root/shaders/XenosRecomp/shader_common.h" "$out/wasm/shader_common.h"
# Supplemental runtime containers for D3D immediate mode and Scaleform UI shaders.
if [ -f "$inst/wasm/runtime_containers.json" ]; then
  cp "$inst/wasm/runtime_containers.json" "$out/wasm/runtime_containers.json"
elif [ -d "$root/shaders/runtime_containers" ]; then
  python3 -c "
import base64, json, os, sys
src, dst = sys.argv[1], sys.argv[2]
data = {f: base64.b64encode(open(os.path.join(src, f), 'rb').read()).decode('ascii')
        for f in sorted(os.listdir(src)) if f.endswith('.bin')}
json.dump(data, open(dst, 'w'), indent=0)
" "$root/shaders/runtime_containers" "$out/wasm/runtime_containers.json"
fi

n=0
for f in scan hlsl pack dxc_web; do
  for ext in mjs wasm; do
    if [ -f "$wasm/$f.$ext" ]; then cp "$wasm/$f.$ext" "$out/wasm/"; n=$((n + 1)); fi
  done
done
echo "staged $n wasm files from $wasm"
[ "$n" -eq 8 ] || echo "WARNING: expected 8 wasm files (scan, hlsl, pack, dxc_web: .mjs + .wasm); the page will report the missing ones" >&2

if [ -n "$releases" ]; then
  for asset in "$releases"/*.nro "$releases"/*.nsp; do
    [ ! -f "$asset" ] || cp "$asset" "$out/releases/"
  done
  # Settings must belong to the same release as the executable.
  [ ! -f "$releases/masseffect.toml" ] || cp "$releases/masseffect.toml" "$out/masseffect.toml"
fi
if [ "$strict" -eq 1 ]; then
  [ "$n" -eq 8 ] || { echo 'error: incomplete WebAssembly toolchain' >&2; exit 1; }
  [ -s "$out/wasm/runtime_containers.json" ] || { echo 'error: missing runtime_containers.json' >&2; exit 1; }
  # Every edition's NRO named in installer/config.js (nro: '...'), plus the forwarder.
  edition_nros=$(sed -nE "s/^[[:space:]]*nro: '([^']+\.nro)',.*/\1/p" "$(dirname "$0")/../config.js" | sort -u)
  [ -n "$edition_nros" ] || { echo 'error: no edition NRO names found in installer/config.js' >&2; exit 1; }
  for asset in $edition_nros masseffect-nx-forwarder.nsp; do
    [ -s "$out/releases/$asset" ] || { echo "error: missing release asset $asset" >&2; exit 1; }
  done
  [ -s "$releases/masseffect.toml" ] || { echo 'error: missing release settings' >&2; exit 1; }
fi
cp "$out/masseffect.toml" "$out/releases/.toml-for-manifest"
python3 - "$out" "${RELEASE_TAG:-}" <<'PY'
import hashlib, json, os, sys
out, tag = sys.argv[1], sys.argv[2]
assets = {}
rel = os.path.join(out, "releases")
for name in sorted(os.listdir(rel)):
    path = os.path.join(rel, name)
    if name == ".toml-for-manifest":
        data = open(path, "rb").read()
        assets["masseffect.toml"] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        os.remove(path)
    elif name.endswith((".nro", ".nsp")):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        assets[name] = {"size": os.path.getsize(path), "sha256": h.hexdigest()}
json.dump({"tag": tag or None, "assets": assets}, open(os.path.join(rel, "manifest.json"), "w"), indent=1)
print("manifest.json:", ", ".join(assets) or "(no assets)")
PY
echo "site staged in $out"
