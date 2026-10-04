#!/usr/bin/env bash
# Reuse the known-working DXC v2025.1 WASM build of the reference installer.
# Source commit and both artifact checksums are pinned; no floating latest downloads.
set -euo pipefail
OUT=${1:?usage: fetch_dxc.sh output-directory}
mkdir -p "$OUT"
REF=e7ec1b651321d752ea34b05bc61cc210543f44d6
BASE="https://raw.githubusercontent.com/StevensND/nfsmw-nx-installer/$REF/wasm"
for ext in mjs wasm; do
  curl --fail --location --silent --show-error --retry 3 "$BASE/dxc_web.$ext" -o "$OUT/dxc_web.$ext"
done
python3 - "$OUT" <<'PY'
import hashlib, pathlib, sys
out = pathlib.Path(sys.argv[1])
expected = {
    'dxc_web.mjs': '6fc3ebec16bbde923dde2bdc92f263dbae115a3f59c57197af84be9065529239',
    'dxc_web.wasm': '86b2fcd344e6a76c6524cbcc9d3830342c60a7ca231b31f585b22c8c36d821f2',
}
for name, sha in expected.items():
    if hashlib.sha256((out / name).read_bytes()).hexdigest() != sha:
        raise SystemExit('DXC checksum mismatch: ' + name)
print('Pinned reference DXC artifacts verified')
PY
