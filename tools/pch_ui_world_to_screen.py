#!/usr/bin/env python3
"""World-to-screen of the HUD markers at the internal resolution (run after every codegen).

sub_827C07F0 (r3 out {x,y,z}, r5 world position) returns Y in the units of the viewport height (544 at 960x544)
while the Flash stage is 720 high, so enemy markers / interaction ring sit too high. The stored Y is scaled by
g_me_ui_y_scale (720/H, set in src/native/me_resolution.cpp; 1.0 = off). Two generated layouts are handled:

  A) args_in_registers: `extern "C" <ret> __fast_sub_827C07F0(PPCContext&, uint8_t* base, uint64_t p_r3, ...) {`.
     Direct callers use the __fast_ body, which bypasses REX_HOOK_RAW hooks, so it is renamed __fast_base_ and a
     wrapper with the same signature scales the stored Y. The parameter list is parsed; p_r3 must be present.
  B) no __fast_ body (args_in_registers=false, or the function is excluded because a hook replaces it):
     the DEFINE_REX_FUNC(sub_827C07F0) body is renamed to a static function and a new DEFINE_REX_FUNC wrapper
     (r3 at entry = out pointer) scales Y after it. A REX_HOOK_RAW hook still wraps this wrapper.

The file is found by searching the generated tree. Idempotent; exit 1 if nothing matches.
Flags: see tools/pch_common.py (--gen, --dry-run, --check).
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pch_common as c

NAME = 'pch_ui_world_to_screen'
F = 'sub_827C07F0'
args = c.Args()
path = args.file or c.search_definition(args.gen, F)
s = c.read(path)
if 'DEFINE_REX_FUNC(%s) {' % F not in s:
    c.failure('%s: no %s definition in %s' % (NAME, F, path))
if 'g_me_ui_y_scale' in s:
    c.finish(args, path, s, True, NAME)

SCALE = '''	const double e = g_me_ui_y_scale;
	if (e != 1.0 && OUT) {
		uint32_t v; memcpy(&v, base + OUT + 4, 4); v = __builtin_bswap32(v);
		float f; memcpy(&f, &v, 4); f = float(f * e);
		memcpy(&v, &f, 4); v = __builtin_bswap32(v); memcpy(base + OUT + 4, &v, 4);
	}
'''
fast = re.search(r'extern "C" (?P<ret>\w+) __fast_%s\((?P<params>[^)]*)\) \{\n' % F, s)
if fast:
    params = [p.strip() for p in fast.group('params').split(',')]
    names = [re.findall(r'(\w+)$', p)[0] for p in params]
    if 'p_r3' not in names or len(params) < 3:
        c.failure('%s: __fast_%s has no p_r3 parameter (%s)' % (NAME, F, fast.group('params')))
    j = fast.start()
    k = s.find('\n}\n', j)
    if k < 0:
        c.failure('%s: end of __fast_%s not found' % (NAME, F))
    k += 3
    ret = fast.group('ret')
    body = s[j:k].replace('__fast_' + F, '__fast_base_' + F, 1)
    plist = ', '.join(params)
    call = ', '.join(names)
    decl = 'extern "C" %s __fast_base_%s(%s);\n' % (ret, F, plist)
    wrapper = '\nextern "C" volatile double g_me_ui_y_scale;\nextern "C" %s __fast_%s(%s) {\n' % (ret, F, plist)
    wrapper += ('\t__fast_base_%s(%s);\n' if ret == 'void' else '\t%s r = __fast_base_%s(%s);\n') % (
        (F, call) if ret == 'void' else (ret, F, call))
    wrapper += SCALE.replace('OUT', 'uint32_t(p_r3)')
    if ret != 'void':
        wrapper += '\treturn r;\n'
    wrapper += '}\n'
    s2 = s[:j] + decl + body + wrapper + s[k:]
    layout = 'A (__fast_ body)'
else:
    i = s.index('DEFINE_REX_FUNC(%s) {' % F)
    k = s.find('\n}\n', i)
    if k < 0:
        c.failure('%s: end of %s body not found' % (NAME, F))
    k += 3
    body = s[i:k].replace('DEFINE_REX_FUNC(%s) {' % F,
                          'static void __me_base_%s(PPCContext& __restrict ctx, uint8_t* base) {' % F, 1)
    if 'REX_FUNC_PROLOGUE' not in body:
        c.failure('%s: unexpected body of %s' % (NAME, F))
    wrapper = ('\nextern "C" volatile double g_me_ui_y_scale;\nDEFINE_REX_FUNC(%s) {\n'
               '\tconst uint32_t out_ = ctx.r3.u32;\n\t__me_base_%s(ctx, base);\n' % (F, F)
               + SCALE.replace('OUT', 'out_') + '}\n')
    s2 = s[:i] + body + wrapper + s[k:]
    layout = 'B (DEFINE_REX_FUNC body)'
print('%s: layout %s' % (NAME, layout))
c.finish(args, path, s2, False, NAME)
