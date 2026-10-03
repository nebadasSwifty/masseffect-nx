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
while [ $# -gt 0 ]; do
  case $1 in
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

# masseffect.toml: the settings file of the build (the page downloads it and puts it into the zip).
cp "$root/app/masseffect.toml" "$out/masseffect.toml"
# The translator's shared header, pasted into every generated HLSL by the translator.
cp "$root/shaders/XenosRecomp/shader_common.h" "$out/wasm/shader_common.h"

n=0
for f in scan hlsl pack dxc_web; do
  for ext in mjs wasm; do
    if [ -f "$wasm/$f.$ext" ]; then cp "$wasm/$f.$ext" "$out/wasm/"; n=$((n + 1)); fi
  done
done
echo "staged $n wasm files from $wasm"
[ "$n" -eq 8 ] || echo "WARNING: expected 8 wasm files (scan, hlsl, pack, dxc_web: .mjs + .wasm); the page will report the missing ones" >&2

if [ -n "$releases" ] && ls "$releases"/*.nro >/dev/null 2>&1; then
  cp "$releases"/*.nro "$out/releases/"
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
    elif name.endswith(".nro"):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        assets[name] = {"size": os.path.getsize(path), "sha256": h.hexdigest()}
json.dump({"tag": tag or None, "assets": assets}, open(os.path.join(rel, "manifest.json"), "w"), indent=1)
print("manifest.json:", ", ".join(assets) or "(no assets)")
PY
echo "site staged in $out"
