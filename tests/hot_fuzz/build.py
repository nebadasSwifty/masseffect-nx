#!/usr/bin/env python3
"""Build and run the hot-hook differential fuzzer on macOS arm64.

  python3 tests/hot_fuzz/build.py [--iters N] [--seed S] [--out DIR] [case-substring ...]

Extracts the originals named in `// ORIG: sub_X ...` lines of tests/hot_fuzz/cases/case_*.inc from the generated code
(app/generated/default, produced by tools/codegen.sh and not committed), builds them with the native headers
(app/src/native/hot) and runs both on random inputs. Nothing generated is committed: the build directory is
out/hot_fuzz (or --out). Needs the SDK headers and third-party sources (tools/fetch_thirdparty.py); macOS arm64 or
Linux aarch64 with clang++ (the native code uses the same ISA as the console).
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GEN = os.environ.get('GEN') or os.path.join(ROOT, 'app', 'generated', 'default')
def find_sdk():
    cands = [os.environ.get('REXSDK', ''), os.path.join(ROOT, 'sdk')]
    for c in cands:
        if c and os.path.isdir(os.path.join(c, 'include', 'rex')):
            return os.path.abspath(c)
    sys.exit('SDK not found (set REXSDK=<folder with include/rex>, default <repo>/sdk)')


def main():
    args = sys.argv[1:]
    iters, seed, run_args, out = '20000', '1', [], None
    i = 0
    filt = []
    while i < len(args):
        if args[i] == '--iters':
            iters = args[i + 1]; i += 2
        elif args[i] == '--seed':
            seed = args[i + 1]; i += 2
        elif args[i] == '--bench':
            run_args.append('--bench'); i += 1
        elif args[i] == '--out':
            out = args[i + 1]; i += 2
        else:
            filt.append(args[i]); i += 1
    out = out or os.path.join(ROOT, 'out', 'hot_fuzz', 'build')
    os.makedirs(out, exist_ok=True)
    sdk = find_sdk()
    cases = sorted(glob.glob(os.path.join(HERE, 'cases', 'case_*.inc')))
    if filt:
        cases = [c for c in cases if any(f in os.path.basename(c) for f in filt)]
    names = []
    natives = []
    for c in cases:
        txt = open(c).read()
        for m in re.finditer(r'^// ORIG:(.*)$', txt, re.M):
            names += re.findall(r'sub_[0-9A-F]{8}', m.group(1))
        natives += re.findall(r'^// NATIVE:\s*(\S+)', txt, re.M)
    with open(os.path.join(out, 'natives_all.inc'), 'w') as f:
        for n in natives:
            f.write('#include "%s"\n' % os.path.join(ROOT, 'app', 'src', 'native', 'hot', n))
    # shim pch
    pch = open(os.path.join(GEN, 'masseffect_pch.h')).read()
    for inc in ['rex/ppc.h', 'rex/image_info.h', 'rex/logging.h', 'rex/chrono/clock.h', 'rex/perf/counter.h',
                'rex/thread/mutex.h', 'rex/system/mmio_handler.h']:
        pch = pch.replace('#include <%s>' % inc, '')
    pch = pch.replace('extern const rex::PPCImageInfo PPCImageConfig;', '').replace('extern PPCFuncMapping PPCFuncMappings[];', '')
    pch = pch.replace('#pragma once', '#pragma once\n#include <rex/ppc/context.h>\n#include <rex/ppc/intrinsics.h>\n#include <cmath>\n#include <climits>\n#define REXCPU_DEBUG(...) ((void)0)\n#define REXCPU_WARN(...) ((void)0)\n')
    open(os.path.join(out, 'masseffect_pch.h'), 'w').write(pch)
    # extract originals
    found = {}
    hdrs = []
    for f in sorted(glob.glob(os.path.join(GEN, 'masseffect_recomp.*.cpp'))):
        txt = open(f, encoding='utf-8').read()
        for n in names:
            if n in found:
                continue
            m = re.search(r'^DEFINE_REX_FUNC\(%s\) \{\n.*?^\}\n' % n, txt, re.S | re.M)
            if m:
                found[n] = m.group(0)
                idx = f.split('.')[-2]
                if idx not in hdrs:
                    hdrs.append(idx)
    missing = [n for n in names if n not in found]
    if missing:
        print('missing in generated code:', missing)
        return 2
    body = ''
    for h in hdrs:
        src = os.path.join(GEN, 'masseffect_funcs.%s.h' % h)
        dst = os.path.join(out, 'masseffect_funcs.%s.h' % h)
        open(dst, 'w').write(open(src).read())
        body += '#include "masseffect_funcs.%s.h"\n' % h
    for n in names:
        body += '\n' + found[n]
    open(os.path.join(out, 'orig.cpp'), 'w').write(body)
    with open(os.path.join(out, 'cases_all.inc'), 'w') as f:
        for c in cases:
            f.write('#include "%s"\n' % c)
    inc = ['-I' + out, '-I' + HERE, '-I' + os.path.join(sdk, 'include'),
           '-I' + os.path.join(sdk, 'thirdparty', 'simde'), '-I' + os.path.join(sdk, 'thirdparty', 'fmt', 'include')]
    cmd = ['clang++', '-std=c++23', '-O2', '-g0', '-ffp-contract=off', '-fno-strict-aliasing', '-w',
           ] + inc + [os.path.join(out, 'orig.cpp'), os.path.join(HERE, 'main.cpp'),
                                              '-o', os.path.join(out, 'fuzz')]
    print(' '.join(cmd[:6]), '...')
    r = subprocess.run(cmd)
    if r.returncode:
        return r.returncode
    return subprocess.run([os.path.join(out, 'fuzz'), '--iters', iters, '--seed', seed] + run_args + filt).returncode


if __name__ == '__main__':
    sys.exit(main())
