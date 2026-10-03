#!/usr/bin/env python3
"""Regenerate app/function_order.ld (hot functions first in .text) from a console run's stack profile.

  usage: python3 tools/function_order.py PROFILE_LOG ELF [OUTPUT.ld]

PROFILE_LOG is the profiler log of a console run (rex_profile.log, with stack samples enabled) and ELF the unstripped
executable of the SAME build (out/nx/masseffect.elf, next to the NRO). The sampled program counters are symbolized with
the devkitA64 addr2line inside the devkitpro/devkita64 Docker image (the mangled names are the section names), the 2000
hottest functions are kept, and crt0 stays first (the NRO does not boot otherwise). The default output is
app/function_order.ld, which app/CMakeLists.txt passes to the linker.
"""
import collections, os, re, subprocess, sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
profile, elf = sys.argv[1], os.path.abspath(sys.argv[2])
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
output = sys.argv[3] if len(sys.argv) > 3 else os.path.join(root, 'app', 'function_order.ld')
image = os.environ.get('DEVKITA64_IMAGE', 'devkitpro/devkita64:latest')
agg = collections.Counter()
for line in open(profile):
    m = re.match(r'\s+([0-9.]+)%\s+(?:pc|lr) image\+(0x[0-9a-f]+)', line)   # "image+0x..." lines of the profiler
    if m: agg[m.group(2)] += float(m.group(1))
pcs = [a for a, _ in agg.most_common(20000)]
out = subprocess.run(['docker', 'run', '--rm', '-v', f'{os.path.dirname(elf)}:/w', image,
                      '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line', '-f', '-e', f'/w/{os.path.basename(elf)}'] + pcs,
                     capture_output=True, text=True).stdout.split('\n')[0::2]
names, seen = [], set()
for n in out:
    n = n.strip()
    if n and n != '??' and n not in seen and re.match(r'^[A-Za-z0-9_.$]+$', n):
        seen.add(n); names.append(n)
if not names:
    sys.exit('no symbols resolved: is the ELF the one that produced the profile, and is Docker running?')
lines = ['/*', f' * Hot functions first in .text, from {os.path.basename(profile)} (tools/function_order.py). crt0 stays first.', ' */',
         '.text : {', '  KEEP (*(.crt0))', '  *libc.a:libc_a-memcpy.o(.text .text.*)', '  *libc.a:libc_a-memmove.o(.text .text.*)',
         '  *libc.a:libc_a-memset.o(.text .text.*)']
for n in names[:2000]:
    if n.startswith('sub_'): lines.append(f'  *(.text.__imp__{n})')
    lines.append(f'  *(.text.{n})')
lines.append('}')
open(output, 'w').write('\n'.join(lines) + '\n')
print(len(names[:2000]), 'functions ->', output)
