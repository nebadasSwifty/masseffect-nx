#!/usr/bin/env python3
"""Report overlay files of an edition that differ from the English file in more than guest addresses.

  tools/edition_drift.py <id>     e.g. tools/edition_drift.py ru

An overlay file replaces the English one in the edition build. When the English file gains a change (a new hook
call, a cvar), the overlay copy silently keeps the old code. This masks every 8-digit 82xxxxxx address (and
sub_82xxxxxx names) and prints the remaining differing lines, so such drift is seen before a build.
"""
import difflib, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDRESS = re.compile(r'(?i)(sub_|0x)?82[0-9a-f]{6}')

def masked(path):
    return [ADDRESS.sub('<addr>', line.rstrip('\n')) for line in open(path, encoding='utf-8', errors='replace')]

def main():
    edition = sys.argv[1] if len(sys.argv) > 1 else 'ru'
    overlay = os.path.join(ROOT, 'editions', edition, 'overlay')
    drift = 0
    for folder, _, files in os.walk(overlay):
        for name in sorted(files):
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, overlay)
            english = os.path.join(ROOT, relative)
            if not os.path.exists(english) or name.endswith(('.ld', '.toml')):
                continue
            lines = [l for l in difflib.unified_diff(masked(english), masked(path), 'english', edition, n=0, lineterm='')
                     if l[:1] in '+-' and not l.startswith(('+++', '---'))]
            if lines:
                drift += 1
                print(f'== {relative}: {len(lines)} lines differ beyond addresses')
                for l in lines[:12]:
                    print('   ' + l[:160])
    print(f'{drift} overlay file(s) with differences beyond addresses')

if __name__ == '__main__':
    main()
