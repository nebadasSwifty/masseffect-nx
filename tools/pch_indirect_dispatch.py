#!/usr/bin/env python3
"""Switchable indirect-call dispatch in the generated pch (run after every codegen, like tools/pch_no_volatile.py).

Replaces the "Indirect Call Dispatch" block of masseffect_pch.h with the current SDK template
sdk/resources/templates/codegen/_indirect_call.inja, which selects the dispatch at compile time with
REX_INDIRECT_DISPATCH (0 legacy = unchanged code, 1 compact table, 2 per-site inline cache; CMake option
MASSEFFECT_INDIRECT_DISPATCH, docs/cpu-cost-analysis.md "Indirect call dispatch"). A generator built from an SDK
that already has the new template emits the same block; the script then reports "already applied". This keeps
older generator binaries (sdk/out/host/rexglue not rebuilt) usable. Idempotent.

  usage: python3 tools/pch_indirect_dispatch.py [--gen DIR] [--dry-run|--check] [pch path]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pch_common as c

NAME = 'pch_indirect_dispatch'
args = c.Args()
path = args.file or os.path.join(args.gen, 'masseffect_pch.h')
if not os.path.isfile(path):
    c.failure('%s: %s not found' % (NAME, path))
sdk = os.environ.get('REXSDK_DIR') or os.path.join(c.ROOT, 'sdk')
tpl_path = os.path.join(sdk, 'resources', 'templates', 'codegen', '_indirect_call.inja')
if not os.path.isfile(tpl_path):
    c.failure('%s: template %s not found' % (NAME, tpl_path))
tpl = c.read(tpl_path)
if '{%' in tpl or '{{' in tpl:
    c.failure('%s: %s contains template syntax, cannot copy it verbatim' % (NAME, tpl_path))
s = c.read(path)
if '#if REX_INDIRECT_DISPATCH == 0' in s:
    c.finish(args, path, s, True, NAME)

bar = '//' + '=' * 77 + '\n'
start = s.find(bar + '// Indirect Call Dispatch\n')
end = s.find(bar + '// Flush Mode\n')
if start < 0 or end < 0 or end < start:
    c.failure('%s: Indirect Call Dispatch / Flush Mode blocks not found in %s' % (NAME, path))
old = s[start:end]
if '#define REX_CALL_INDIRECT_FUNC(x)' not in old or old.count('REX_CALL_INDIRECT_FUNC(x)') != 1:
    c.failure('%s: unexpected Indirect Call Dispatch block in %s' % (NAME, path))
s = s[:start] + tpl.rstrip('\n') + '\n\n\n' + s[end:]
c.finish(args, path, s, False, NAME)
