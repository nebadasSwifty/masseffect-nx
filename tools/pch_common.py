"""Shared helpers of the post-codegen patch scripts (pch_*.py): locate generated files, common flags, errors.

Common flags of every pch_*.py:
  --gen DIR     generated tree (default app/generated/default)
  --dry-run     do everything except writing; exit 1 if a pattern is missing
  --check       report only: exit 0 if the patch is already applied, 1 otherwise (used by tools/verify_pch.sh)
  <path>        (positional) explicit file, skips the search
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN_DEFAULT = os.path.join(ROOT, 'app', 'generated', 'default')


class Args:
    def __init__(self, argv=None):
        a = list(sys.argv[1:] if argv is None else argv)
        self.dry = '--dry-run' in a
        self.check = '--check' in a
        self.gen = GEN_DEFAULT
        self.file = None
        i = 0
        while i < len(a):
            if a[i] == '--gen':
                self.gen = a[i + 1]; i += 1
            elif not a[i].startswith('--'):
                self.file = a[i]
            i += 1


def failure(msg):
    sys.stderr.write('ERROR (post-codegen patch): %s\n' % msg)
    sys.exit(1)


def cpps(gen):
    r = sorted(glob.glob(os.path.join(gen, 'masseffect_recomp.*.cpp')))
    if not r:
        failure('no masseffect_recomp.*.cpp in %s' % gen)
    return r


def read(path):
    with open(path, encoding='utf-8', errors='surrogateescape', newline='') as fh:
        return fh.read()


def write(path, s):
    with open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='') as fh:
        fh.write(s)


def search_definition(gen, name):
    """File of the generated tree that contains `DEFINE_REX_FUNC(name) {` (exactly one, else error)."""
    key = 'DEFINE_REX_FUNC(%s) {' % name
    hits = [f for f in cpps(gen) if key in read(f)]
    if len(hits) != 1:
        failure('%d definitions of %s in %s (expected exactly 1)' % (len(hits), name, gen))
    return hits[0]


def finish(args, path, s_new, already_applied, name):
    """Common ending: already applied / --check / --dry-run / write."""
    if already_applied:
        print('%s: already applied (%s)' % (name, os.path.basename(path))); sys.exit(0)
    if args.check:
        sys.stderr.write('%s: NOT applied in %s\n' % (name, path)); sys.exit(1)
    if args.dry:
        print('%s: dry-run OK, patch applicable to %s' % (name, os.path.basename(path))); sys.exit(0)
    write(path, s_new)
    print('%s: applied to %s' % (name, os.path.basename(path)))
