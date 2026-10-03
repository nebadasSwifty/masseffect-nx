#!/usr/bin/env python3
"""Create fresh labelled reverse-transport negatives; never modify production."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output_directory', type=Path)
args = parser.parse_args()
source = args.source.read_text()
needle = 'uvec2 physical = uvec2(pixel.x, pixel.y * 2u + (sample_host ^ 1u));'
if source.count(needle) != 1:
    raise ValueError('production physical-address expression changed')
mutants = {
    'swap': source.replace(needle, 'uvec2 physical = uvec2(pixel.x, pixel.y * 2u + sample_host);'),
    'collapse': source.replace(needle, 'uvec2 physical = uvec2(pixel.x, pixel.y * 2u);'),
    'skip': source.replace(needle, 'if (pixel.x >= 20u && pixel.x < 24u && pixel.y == 9u) discard;\n  ' + needle),
}
for name, text in mutants.items():
    with (args.output_directory / f'negative-{name}.frag').open('x') as output:
        output.write('// NEGATIVE TEST MUTANT: ' + name + ', NOT production shader\n' + text)
