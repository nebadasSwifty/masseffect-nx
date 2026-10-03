#!/usr/bin/env python3
"""Scaleform GFx viewport of sub_82238FE8 at the internal resolution (run after every codegen).

The function builds {1280,720,0,0,1280,720,0} for movie->vtable[+88] (SetViewport). With the scene rendered at
960x544 that leaves the UI stage laid out for 1280x720 (stretched loading screen, prompts too high). The two
constants are replaced by g_me_ui_width/alto (0 -> 1280/720), set by src/native/me_resolution.cpp.

The file is found by searching the generated tree for the function definition (it moves between codegens).
Idempotent; exits 1 if the pattern is missing. Flags: see tools/pch_common.py (--gen, --dry-run, --check).
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pch_common as c

NAME = 'pch_ui_viewport'
args = c.Args()
path = args.file or c.search_definition(args.gen, 'sub_82238FE8')
s = c.read(path)
if 'DEFINE_REX_FUNC(sub_82238FE8) {' not in s:
    c.failure('%s: no sub_82238FE8 definition in %s' % (NAME, path))
i = s.index('DEFINE_REX_FUNC(sub_82238FE8) {')
j = s.find('\n}\n', i)
body = s[i:j]
if 'g_me_ui_height' in body or 'g_me_ui_height' in s[max(0, i - 200):i]:
    c.finish(args, path, s, True, NAME)
pat = re.compile(r'(?P<ind>[ \t]*)// li r11,720\n[ \t]*r11\.s64 = 720;\n[ \t]*// li r10,1280\n[ \t]*ctx\.r10\.s64 = 1280;\n')
if len(pat.findall(body)) != 1:
    c.failure('%s: "li r11,720 / li r10,1280" pattern not found exactly once in sub_82238FE8 of %s; the generated '
            'code changed, update tools/pch_ui_viewport.py' % (NAME, path))
rep = lambda m: ('%s// li r11,720 (Mass Effect: tools/pch_ui_viewport.py)\n%sr11.s64 = g_me_ui_height;\n'
                 '%sctx.r10.s64 = g_me_ui_width;\n' % ((m.group('ind'),) * 3))
body2 = pat.sub(rep, body)
s = s[:i] + 'extern "C" volatile int64_t g_me_ui_width, g_me_ui_height;\n' + body2 + s[j:]
c.finish(args, path, s, False, NAME)
