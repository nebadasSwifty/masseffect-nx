#!/usr/bin/env python3
# Direct calls to game functions that have no hook (post-codegen step, run after every codegen).
#
# With GCC every recompiled function sub_X is a weak alias of __imp__sub_X (DEFINE_REX_FUNC, masseffect_pch.h),
# so that one of our REX_HOOK_RAW(sub_X) replaces it at link time. The codegen always calls sub_X, and the
# compiler cannot inline a weak function into another (a hook could replace it): not within the same file
# and not with LTO. This step changes `sub_X(ctx, base);` into `__imp__sub_X(ctx, base);` when sub_X has no
# hook, which is exactly the same function, but can now be inlined.
#
# Any 82xxxxxx address that appears in the app or SDK sources or in the toml files counts as hooked (the
# D3D trace builds its hooks with sub_##addr). That is slightly more than needed and safe.
#
# It does not touch masseffect_init.cpp or masseffect_register.cpp: the dispatch table for indirect calls still
# points to sub_X and sees the hooks. It has to be run again after every codegen (tools/codegen.sh does).
#
# Usage: python3 tools/direct_calls.py [--undo] [--dry-run] [--check] [--gen <generated folder>] [--hooked-list <file>]
#   --dry-run: count only, write nothing. --check: exit 1 if unhooked sub_X calls are still not direct, or if the
#   tree has no direct calls at all (a codegen that lost the step). Exit 1 also if no recomp files are found.
#   --undo: turn every __imp__sub_X call back into sub_X.
#   --hooked-list: a file with the sub_X names to treat as hooked, instead of scanning the sources.
#   Environment: REXSDK_DIR = SDK folder to scan for hooks (default <repo>/sdk).
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # repository root
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated', 'default')
SDK = os.environ.get('REXSDK_DIR') or os.path.join(ROOT, 'sdk')
SOURCES = [os.path.join(APP, 'src'), os.path.join(SDK, 'src'), os.path.join(SDK, 'include'),
           os.path.join(APP, 'overrides.toml'), os.path.join(APP, 'perf_overrides.toml'),
           os.path.join(APP, 'masseffect_manifest.toml'), os.path.join(APP, 'CMakeLists.txt')]


def hooked():
    names = set()
    for root in SOURCES:
        if os.path.isfile(root):
            paths = [root]
        else:
            paths = [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs
                     if not f.endswith(('.a', '.obj', '.o', '.png', '.jpg', '.bin', '_all.inc', '_all.inc'))]
        for path in paths:
            try:
                text = open(path, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for m in re.findall(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])', text):
                names.add('sub_' + m.upper())
    return names


def main():
    undo = '--undo' in sys.argv
    dry = '--dry-run' in sys.argv or '--check' in sys.argv
    check = '--check' in sys.argv
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    if '--hooked-list' in sys.argv:
        with_hook = set(open(sys.argv[sys.argv.index('--hooked-list') + 1], encoding='utf-8').read().split())
    else:
        with_hook = hooked()
    call = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    direct = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    changed = 0
    files = 0
    seen = 0
    already_direct = 0
    for f in sorted(os.listdir(gen)):
        if not (f.startswith('masseffect_recomp.') and f.endswith('.cpp')):
            continue
        path = os.path.join(gen, f)
        text = open(path, encoding='utf-8').read()
        seen += 1
        already_direct += len(direct.findall(text))
        if undo:
            new_value, n = direct.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), text)
        else:
            def change(m):
                return m.group(0) if 'sub_' + m.group(1) in with_hook else '__imp__sub_%s(ctx, base);' % m.group(1)
            new_value = call.sub(change, text)
            n = len(call.findall(text)) - len(call.findall(new_value))
        if new_value != text:
            files += 1
            if not dry:
                open(path, 'w', encoding='utf-8', newline='\n').write(new_value)
        changed += n
    if seen == 0:
        sys.exit('ERROR (post-codegen patch): direct_calls: no masseffect_recomp.*.cpp in %s' % gen)
    if not undo and changed == 0 and already_direct == 0:
        sys.exit('ERROR (post-codegen patch): direct_calls: neither sub_X calls to convert nor __imp__ direct '
                 'calls in %s; the generated call shape changed' % gen)
    if check and not undo:
        print('direct_calls --check: %d direct calls present, %d still convertible' % (already_direct, changed))
        if changed or already_direct == 0:
            sys.exit(1)
        return
    print('%s: %d calls in %d files; %d hooked addresses respected' %
          ('undone' if undo else 'direct' + (' (dry-run)' if dry else ''), changed, files, len(with_hook)))


if __name__ == '__main__':
    main()
