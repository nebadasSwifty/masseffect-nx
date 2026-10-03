#!/usr/bin/env python3
# Extracts the recompiled body of guest functions from app/generated/default into ref_<addr>.inc
# (DEFINE_REX_FUNC(sub_X) { ... } verbatim) for the host differential test.
#   usage: extract.py <generated dir> <out dir> <ADDR> [<ADDR> ...]
import glob
import os
import re
import sys

gen, out = sys.argv[1], sys.argv[2]
for addr in sys.argv[3:]:
    name = 'sub_' + addr.upper()
    found = False
    for f in sorted(glob.glob(os.path.join(gen, 'masseffect_recomp.*.cpp'))):
        txt = open(f).read()
        k = txt.find('DEFINE_REX_FUNC(%s)' % name)
        if k < 0:
            continue
        e = txt.find('\n}\n', k)
        body = txt[k:e + 3]
        # the extracted copy must call the originals, never a hook
        # (also for every callee: the test stubs the callees as __imp__ functions, and a callee that has a hook in the app
        # keeps the sub_X spelling in the generated code)
        body = re.sub(r'\bsub_([0-9A-F]{8})\(ctx, base\);', r'__imp__sub_\1(ctx, base);', body)
        open(os.path.join(out, 'ref_%s.inc' % addr.upper()), 'w').write(body)
        print('extracted', name, 'from', os.path.basename(f), len(body), 'bytes')
        found = True
        break
    if not found:
        sys.exit('function %s not found' % name)
