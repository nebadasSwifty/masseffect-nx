#!/usr/bin/env python3
"""Checks that the fragment conversion shaders (me_edram_color_to_color.frag, me_edram_r64_to_r64.frag) address the
same texels as the compute ones (masseffect_edram_*_a_*.comp, me_edram_raw64_to_raw64.comp): for random tile runs, pitches,
MSAA shifts and start tiles, destination texel -> source texel must be the same map. Pure Python, no GPU."""
import random

random.seed(1)


def compute_map(c, bpp64):
    out = {}
    tw = 40 if bpp64 else 80          # pixels per tile row of the view (before MSAA)
    step = 2 if bpp64 else 1          # words per pixel
    for qy in range(16):
        for qx in range(c['cant'] * 80):
            if (qx & ((1 << (bpp64 + c['dmx'])) - 1)) != 0 or (qy & ((1 << c['dmy']) - 1)) != 0:
                continue
            rel, local = qx // 80, qx % 80
            td, to = c['sd'] + rel, c['so'] + rel
            px = ((td % c['pd']) * tw + local // step) >> c['dmx']
            py = ((td // c['pd']) * 16 + qy) >> c['dmy']
            if px >= c['wd'] or py >= c['hd']:
                continue
            out[(px, py)] = (((to % c['po']) * tw + local // step) >> c['omx'],
                             ((to // c['po']) * 16 + qy) >> c['omy'])
    return out


def frag_map(c, bpp64):
    tw = 40 if bpp64 else 80
    wt = tw >> c['dmx']
    ht = 16 >> c['dmy']
    pitch = c['pd']
    first, last = c['sd'] // pitch, (c['sd'] + c['cant'] - 1) // pitch
    sx = (c['sd'] % pitch) * wt if first == last else 0
    sy = first * ht
    right = min(c['wd'], ((c['sd'] + c['cant'] - 1) % pitch + 1) * wt) if first == last else c['wd']
    bottom = min(c['hd'], (last + 1) * ht)
    out = {}
    if sx >= c['wd'] or sy >= c['hd'] or right <= sx or bottom <= sy:
        return out
    for py in range(sy, bottom):
        for px in range(sx, right):
            fx, fy = px << c['dmx'], py << c['dmy']
            td = (fy // 16) * pitch + fx // tw
            if td < c['sd'] or td - c['sd'] >= c['cant']:
                continue
            to = c['so'] + td - c['sd']
            out[(px, py)] = ((((to % c['po']) * tw) + fx % tw) >> c['omx'],
                             ((to // c['po']) * 16 + fy % 16) >> c['omy'])
    return out


bad = 0
for bpp64 in (0, 1):
    for _ in range(3000):
        dmx, dmy = random.choice([0, 0, 1]), random.choice([0, 0, 1])
        pd = random.randint(1, 12)
        rows = random.randint(1, 8)
        tw = 40 if bpp64 else 80
        c = dict(dmx=dmx, dmy=dmy, omx=random.choice([0, dmx]), omy=random.choice([0, dmy]), pd=pd,
                 po=random.choice([pd, pd, random.randint(1, 12)]), wd=random.randint(1, pd * (tw >> dmx)),
                 hd=random.randint(1, rows * (16 >> dmy)), sd=random.randint(0, pd * rows - 1),
                 so=random.randint(0, 100), cant=1)
        c['cant'] = random.randint(1, max(1, pd * rows - c['sd']))
        if compute_map(c, bpp64) != frag_map(c, bpp64):
            bad += 1
            if bad < 5:
                print('MISMATCH', bpp64, c)
print('mismatches:', bad)
raise SystemExit(1 if bad else 0)
