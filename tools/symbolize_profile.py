#!/usr/bin/env python3
"""Symbolize a console stack profile (rex_profile.log / profile.log) with the ELF of the SAME build.

  usage: python3 tools/symbolize_profile.py PROFILE_LOG ELF [OUTPUT]      (default output: PROFILE_LOG.sym)

Every "pc image+0x..." / "lr image+0x..." line gets the function name (demangled) and, with --lines, file:line.
It then prints, per thread section ("-- thread ..."), the samples summed per function, so a thread's cost is read
per function and not per instruction. The ELF must be the unstripped executable that was packed into the NRO of
that run (out/edition-ru/out/nx/masseffect for the Russian edition): a rebuild moves functions and the names come
out wrong (spdlog where a wait is, guest functions where native code is). Keep a copy of it next to each NRO
(ru_NAME.nro + ru_NAME.elf) and symbolize that run's profile with that copy.

addr2line runs in the devkitpro/devkita64 Docker image (as tools/function_order.py), or natively when
AARCH64_ADDR2LINE points to an aarch64 addr2line / llvm-addr2line on this machine.
"""
import collections, os, re, subprocess, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
with_lines = '--lines' in sys.argv
if len(args) < 2:
    sys.exit(__doc__)
profile, elf = args[0], os.path.abspath(args[1])
output = args[2] if len(args) > 2 else profile + '.sym'
pattern = re.compile(r'^(\s+)([0-9.]+)%\s+(pc|lr) image\+(0x[0-9a-f]+)(.*)$')

lines = open(profile, errors='replace').read().split('\n')
pcs = sorted({m.group(4) for m in map(pattern.match, lines) if m})
if not pcs:
    sys.exit('no "pc image+0x..." lines in ' + profile)


def addr2line(addresses):
    flags = ['-f', '-C'] + ([] if with_lines else ['-s'])
    tool = os.environ.get('AARCH64_ADDR2LINE')
    names = []
    for i in range(0, len(addresses), 4000):
        chunk = addresses[i:i + 4000]
        if tool:
            cmd = [tool] + flags + ['-e', elf] + chunk
        else:
            image = os.environ.get('DEVKITA64_IMAGE', 'devkitpro/devkita64:latest')
            cmd = ['docker', 'run', '--rm', '-v', f'{os.path.dirname(elf)}:/w', image,
                   '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line'] + flags + ['-e', f'/w/{os.path.basename(elf)}'] + chunk
        out = subprocess.run(cmd, capture_output=True, text=True).stdout.split('\n')
        for k in range(len(chunk)):
            function = out[2 * k].strip() if 2 * k < len(out) else '??'
            where = out[2 * k + 1].strip() if 2 * k + 1 < len(out) else ''
            names.append(function if not with_lines else f'{function} ({where})')
    return names


symbol = dict(zip(pcs, addr2line(pcs)))
if sum(1 for v in symbol.values() if not v.startswith('??')) == 0:
    sys.exit('no symbols resolved: is the ELF the one that produced the profile, and is Docker running?')

result = []
section, per_function = None, collections.Counter()


def flush():
    if section and per_function:
        result.append(f'   == per function in {section.strip()[:80]}:')
        for name, share in per_function.most_common(40):
            result.append(f'     {share:5.1f}%  {name}')
    per_function.clear()


for line in lines:
    if line.startswith('-- thread') or line.startswith('===='):
        flush()
        section = line if line.startswith('-- thread') else None
    m = pattern.match(line)
    if m:
        name = symbol.get(m.group(4), '??')
        if m.group(3) == 'pc':
            per_function[name] += float(m.group(2))
        line = f'{m.group(1)}{m.group(2)}%  {m.group(3)} image+{m.group(4)}  {name}{m.group(5)}'
    result.append(line)
flush()
open(output, 'w').write('\n'.join(result) + '\n')
print(f'{len(pcs)} addresses, {sum(1 for v in symbol.values() if not v.startswith("??"))} resolved -> {output}')
