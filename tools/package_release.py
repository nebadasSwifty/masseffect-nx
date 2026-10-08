#!/usr/bin/env python3
"""Assemble a starter ZIP without game files or generated shaders."""
import hashlib
from pathlib import Path
import shutil
import sys
import zipfile

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / 'out/release')
for name in ('masseffect-nx.nro', 'masseffect-nx-forwarder.nsp'):
    if not (out / name).is_file():
        raise SystemExit(f'Missing release artifact: {name}')
shutil.copyfile(root / 'app/masseffect.toml', out / 'masseffect.toml')
with zipfile.ZipFile(out / 'masseffect-nx-starter.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for name in ('masseffect-nx.nro', 'masseffect.toml'):
        z.write(out / name, f'switch/masseffect-nx/{name}')
    # The English edition's shipped pipeline prewarm list, under the name masseffect.toml uses (optional).
    prewarm = root / 'app/prewarm/masseffect_prewarm_list-en.bin'
    if prewarm.is_file():
        z.write(prewarm, 'switch/masseffect-nx/masseffect_prewarm_list.bin')
    z.write(out / 'masseffect-nx-forwarder.nsp', 'masseffect-nx-forwarder.nsp')
    z.writestr('INSTALL.txt',
        'Extract this starter ZIP into the ROOT of the SD card.\n'
        'Use https://nebadasSwifty.github.io/masseffect-nx/ to build the game/shader package from your own disc.\n'
        'Extract the installer ZIP into /switch/ on the SD card.\n'
        'Install masseffect-nx-forwarder.nsp with your CFW title installer.\n'
        'The HOME tile launches /switch/masseffect-nx/masseffect-nx.nro in 39-bit application mode.\n'
        'The NSP contains a launcher only; the NRO, game_root and shaders must remain on SD.\n')
with (out / 'SHA256SUMS').open('w') as f:
    for p in sorted(out.iterdir()):
        if p.suffix in ('.nro', '.nsp', '.zip', '.toml') or p.name.startswith('masseffect_prewarm_list-'):
            h = hashlib.sha256()
            with p.open('rb') as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b''):
                    h.update(chunk)
            f.write(f'{h.hexdigest()}  {p.name}\n')
