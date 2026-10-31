#!/usr/bin/env python3
"""Runs clang's -Wthread-safety over the engine sources and prints only those diagnostics (and real errors).

    tools/analysis/thread_safety.py [-b build-clang] [-j 8] [file substring ...]

Needs a clang build directory with compile_commands.json (cmake -DCMAKE_CXX_COMPILER=clang++
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON). The locks are the ones of Debugging/ThreadSafety.h, std::mutex has no attributes.
"""
import argparse
import concurrent.futures as cf
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEEP = re.compile(r'(warning: .*\[-Wthread-safety[^\]]*\]|error:)')


def check(entry):
    args = shlex.split(entry['command'])
    args[0] = 'clang++'

    if '-o' in args:
        i = args.index('-o')
        del args[i:i + 2]

    args = [a for a in args if a not in ('-c', '-ftime-trace') and not (a.startswith ('-W') and not a.startswith ('-Wno-'))]
    args += ['-fsyntax-only', '-Wthread-safety', '-Wthread-safety-beta']
    result = subprocess.run(args, cwd=entry['directory'], capture_output=True, text=True)
    lines = [line for line in result.stderr.splitlines() if KEEP.search(line)]
    return [(entry['file'], line) for line in lines]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--build', default='build-clang')
    parser.add_argument('-j', '--jobs', type=int, default=8)
    parser.add_argument('filters', nargs='*')
    options = parser.parse_args()

    entries = json.load(open(ROOT / options.build / 'compile_commands.json'))
    entries = [e for e in entries if '/src/WallpaperEngine/' in e['file'] or e['file'].endswith('/src/main.cpp')]

    if options.filters:
        entries = [e for e in entries if any(f in e['file'] for f in options.filters)]

    seen = set()
    with cf.ThreadPoolExecutor(options.jobs) as pool:
        for found in pool.map(check, entries):
            for _, line in found:
                line = line.replace(str(ROOT) + '/src/WallpaperEngine/', '')

                if line not in seen:
                    seen.add(line)
                    print(line)

    print(f'{len(seen)} diagnostics', file=sys.stderr)
    return 1 if seen else 0


if __name__ == '__main__':
    sys.exit(main())
