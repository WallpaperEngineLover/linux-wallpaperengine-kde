#!/usr/bin/env python3
"""GCC's -fanalyzer over the engine's own sources (src/WallpaperEngine, no External), one translation unit at a time
from build/compile_commands.json with the object going to /dev/null. A TU takes up to 6 GB, so the default is two at
once; the machine is shared with the user's desktop.

    tools/analysis/gcc_analyzer.py <out dir> [-j 2] [--timeout 600] [path substring...]

Writes <out dir>/<file>.txt per source with the analyzer's warnings and <out dir>/summary.txt.
"""

import argparse
import concurrent.futures
import json
import resource
import shlex
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def analyze(entry, out, timeout):
    words = shlex.split(entry['command'])
    command = []
    skip = False
    for word in words:
        if skip:
            skip = False
            continue
        if word == '-o':
            skip = True
            continue
        command.append(word)
    command += ['-o', '/dev/null', '-fanalyzer', '-Wno-error', '-Wno-all', '-Wno-extra', '-Wanalyzer-too-complex']
    command += ['-Wanalyzer-null-dereference', '-Wanalyzer-use-after-free', '-Wanalyzer-double-free',
                '-Wanalyzer-malloc-leak', '-Wanalyzer-file-leak', '-Wanalyzer-possible-null-argument',
                '-Wanalyzer-possible-null-dereference', '-Wanalyzer-null-argument', '-Wanalyzer-use-of-uninitialized-value',
                '-Wanalyzer-out-of-bounds', '-Wanalyzer-deref-before-check', '-Wanalyzer-fd-leak']
    name = str(Path(entry['file']).relative_to(ROOT)).replace('/', '_')
    started = time.time()
    try:
        done = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True, timeout=timeout,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_AS, (10 << 30, 10 << 30)))
        text, code = done.stderr, done.returncode
    except subprocess.TimeoutExpired:
        text, code = '', 'timeout'
    (out / f'{name}.txt').write_text(text)
    warnings = text.count('[-Wanalyzer-')
    return f'{entry["file"].replace(str(ROOT) + "/", "")}: exit={code} warnings={warnings} {time.time() - started:.0f}s'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out', type=Path)
    parser.add_argument('-j', '--jobs', type=int, default=2)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('filters', nargs='*')
    options = parser.parse_args()
    options.out.mkdir(parents=True, exist_ok=True)
    entries = [e for e in json.loads((ROOT / 'build' / 'compile_commands.json').read_text())
               if '/src/WallpaperEngine/' in e['file'] and e['file'].endswith('.cpp') and '/Testing/' not in e['file']
               and (not options.filters or any(f in e['file'] for f in options.filters))]
    entries = sorted({e['file']: e for e in entries}.values(), key=lambda e: e['file'])
    lines = []
    with concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
        for line in pool.map(lambda e: analyze(e, options.out, options.timeout), entries):
            print(line, flush=True)
            lines.append(line)
    (options.out / 'summary.txt').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
