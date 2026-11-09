#!/usr/bin/env python3
"""Merges gcovr JSON reports of different builds (the host's coverage build and the portable one that ran in the VMs) by
line: a line counts when any report ran it, only lines the first report knows are kept (other compilers split inlined code
differently). Prints the per file difference and the totals.

Usage: merge_coverage.py <base.json> <more.json>... [--top N]"""
import json, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
if '--top' in sys.argv:
    args.remove(sys.argv[sys.argv.index('--top') + 1])


def load(path):
    return {f['file']: {l['line_number']: l['count'] for l in f['lines']} for f in json.load(open(path))['files']}


base = load(args[0])
merged = {f: dict(lines) for f, lines in base.items()}
for path in args[1:]:
    for f, lines in load(path).items():
        if f in merged:
            for number, count in lines.items():
                if number in merged[f]:
                    merged[f][number] += count

rows = []
total = [0, 0, 0]
for f, lines in base.items():
    before = sum(1 for c in lines.values() if c == 0)
    after = sum(1 for c in merged[f].values() if c == 0)
    total[0] += len(lines)
    total[1] += before
    total[2] += after
    if before != after:
        rows.append((before - after, f.rsplit('/', 1)[-1], before, after))
for row in sorted(rows, reverse=True)[:top]:
    print(*row)
print('lines', total[0], 'missed before', total[1], 'after', total[2])
