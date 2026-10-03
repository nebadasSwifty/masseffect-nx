#!/usr/bin/env python3
"""Generate explicitly labelled negative artifacts, never modify production shader."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output_directory', type=Path)
args = parser.parse_args()
source = args.source.read_text()
needle = 'int source_sample = int((source_physical.y & 1u) ^ 1u);'
assert source.count(needle) == 1, 'production sample expression changed'
pixel = '  uvec2 physical = pixel;'
assert source.count(pixel) == 1, 'production pixel expression changed'
mutants = {
    'swap': source.replace(needle, 'int source_sample = int(source_physical.y & 1u);'),
    'collapse': source.replace(needle, 'int source_sample = 1;'),
    'skip': source.replace(pixel, '  if (pixel.x >= 20u && pixel.x < 24u && pixel.y == 17u) discard;\n' + pixel),
}
for name, text in mutants.items():
    with (args.output_directory / f'negative-{name}.frag').open('x') as output:
        output.write('// NEGATIVE TEST MUTANT: ' + name + ', NOT production shader\n' + text)
