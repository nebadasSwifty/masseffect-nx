#!/usr/bin/env python3
"""Guest loads/stores without `volatile` in the generated Switch pch (run after every codegen,
like tools/direct_calls.py). The generated REX_LOAD_*/REX_STORE_* read guest memory through volatile
pointers, which stops the compiler from keeping values in registers or pairing accesses. t153: 16.4-22.3 fps
vs 14.5-18 (t150), image correct, no hang. MMIO macros keep volatile. Idempotent.

  usage: python3 tools/pch_no_volatile.py [--gen DIR] [--dry-run|--check] [pch path]
  (default app/generated/default/masseffect_pch.h; exit 1 if the pattern is missing; see tools/pch_common.py)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pch_common as c

NAME = 'pch_no_volatile'
args = c.Args()
path = args.file or os.path.join(args.gen, 'masseffect_pch.h')
if not os.path.isfile(path):
    c.failure('%s: %s not found' % (NAME, path))
s = c.read(path)
if 'REX_GUEST_VOLATILE' in s:
    c.finish(args, path, s, True, NAME)
out = []
changed = 0
for line in s.split('\n'):
    if line.startswith(('#define REX_LOAD_U', '#define REX_STORE_U', '#define REX_LOAD_D_U', '#define REX_STORE_D_U')):
        n = line.replace('(volatile u', '(REX_GUEST_VOLATILE u')
        changed += n != line
        line = n
    out.append(line)
if changed == 0 or '#define REX_LOAD_U8(x)' not in s:
    c.failure('%s: no REX_LOAD_*/REX_STORE_* "(volatile u" macros found in %s; the generated pch changed' % (NAME, path))
s = '\n'.join(out)
s = s.replace('#define REX_LOAD_U8(x)', '''// Mass Effect: guest loads/stores without volatile (tools/pch_no_volatile.py); define REX_GUEST_VOLATILE as
// volatile to restore the generated behaviour.
#ifndef REX_GUEST_VOLATILE
#define REX_GUEST_VOLATILE
#endif
#define REX_LOAD_U8(x)''', 1)
c.finish(args, path, s, False, NAME)
