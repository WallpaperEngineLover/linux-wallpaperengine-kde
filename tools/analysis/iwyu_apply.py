#!/usr/bin/env python3
"""Removes the includes include-what-you-use calls unused from the engine's .cpp files, and keeps only the removals that
still compile.

    iwyu_tool.py -p build-clang -j 6 src/WallpaperEngine > iwyu.txt
    tools/analysis/iwyu_apply.py iwyu.txt [-j 8] [--dry-run]

Only "remove" lines of .cpp files count. Left alone on purpose: headers, includes inside #if blocks (IWYU only sees the
active configuration), IWYU pragmas and, without --std, standard library headers (the transitive includes of the
libstdc++ here are not those of Debian 12's, a removal that builds here could break the portable build). Every changed
file is syntax checked with g++ (build/) and clang++ (build-clang/) from their compile_commands.json; a file that fails
is tried again one include at a time. Run clang-format on the result.

--std takes the standard library headers instead of everything else, and also checks each file with Debian 12's g++ 12
(the portable release's compiler, the oldest libstdc++ we build with) in its rootfs under proot: `build-deb12` is a CMake
build dir of the repo made there (/src = the repo, /build), PROOT and the rootfs from tools/distro_rootfs.sh
(~/.local/share/distro-rootfs/debian-12 and build-deb12, proot at ~/.local/share/arch-proot/proot unless PROOT is set).
"""
import argparse
import collections
import concurrent.futures as cf
import json
import os
import re
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STD_HEADER = re.compile(r'^#include <[^.>]+>$')
BUILDS = ('build', 'build-clang')
ROOTFS = Path.home() / '.local/share/distro-rootfs'
PROOT = os.environ.get('PROOT', str(Path.home() / '.local/share/arch-proot/proot'))


def parse(report):
    removals = collections.defaultdict(dict)
    current = None
    for line in report.read_text(errors='replace').split('\n'):
        m = re.match(r'^(/\S+) should remove these lines:$', line)
        if m:
            current = m.group(1)
            continue
        if current and line.startswith('- #include'):
            m = re.match(r'^- (#include [<"][^>"]+[>"])\s+// lines (\d+)-(\d+)', line)
            if m:
                removals[current][int(m.group(2)) - 1] = m.group(1)
            continue
        if current and not line.startswith('- ') and line.strip():
            current = None
    return removals


def candidates(removals, std):
    plan = {}
    for path, items in removals.items():
        if not path.startswith(str(ROOT / 'src/WallpaperEngine') + '/') or not path.endswith('.cpp'):
            continue
        lines = Path(path).read_text().split('\n')
        depth, conditional = 0, []
        for line in lines:
            text = line.strip()
            if re.match(r'#\s*(if|ifdef|ifndef)\b', text):
                depth += 1
            conditional.append(depth > 0)
            if re.match(r'#\s*endif\b', text):
                depth -= 1
        keep = [i for i, include in sorted(items.items())
                if i < len(lines) and lines[i].split('//')[0].strip() == include and 'IWYU pragma' not in lines[i]
                and not conditional[i] and bool(STD_HEADER.match(include)) == std]
        if keep:
            plan[path] = keep
    return plan


def load(build):
    return {entry['file']: entry for entry in json.loads((ROOT / build / 'compile_commands.json').read_text())}


def compiles_deb12(entry, path):
    args = shlex.split(entry['command'])
    args = [a for a in args if not (a.startswith('-W') and not a.startswith('-Wno-'))]
    if '-o' in args:
        i = args.index('-o')
        del args[i:i + 2]
    args = [a for a in args if a != '-c']
    args.insert(1, '-fsyntax-only')
    command = ['bash', str(ROOT / 'tools/distro_rootfs.sh'), 'run', str(ROOTFS / 'debian-12'), '-u', 'sh', '-c',
               'cd /build && ' + shlex.join(args)]
    env = dict(os.environ, PROOT=PROOT, BINDS=f'-b {ROOT}:/src -b {ROOTFS / "build-deb12"}:/build')
    return subprocess.run(command, env=env, capture_output=True).returncode == 0


def compiles(databases, path, deb12=None):
    if deb12 is not None:
        entry = deb12.get(path.replace(str(ROOT), '/src', 1))
        if entry is None or not compiles_deb12(entry, path):
            return False
    checked = False
    for build in BUILDS:
        entry = databases[build].get(path)
        if entry is None:
            continue
        args = shlex.split(entry['command'])
        args[0] = 'g++' if build == 'build' else 'clang++'
        args = [a for a in args if not (a.startswith('-W') and not a.startswith('-Wno-'))]
        if '-o' in args:
            i = args.index('-o')
            del args[i:i + 2]
        args = [a for a in args if a not in ('-c', '-ftime-trace')]
        args.insert(1, '-fsyntax-only')
        checked = True
        if subprocess.run(args, cwd=entry['directory'], capture_output=True).returncode != 0:
            return False
    return checked


def apply(databases, path, indices, deb12):
    file = Path(path)
    original = file.read_text()
    lines = original.split('\n')

    def write(dropped):
        file.write_text('\n'.join(line for i, line in enumerate(lines) if i not in dropped))

    try:
        write(set(indices))
        if compiles(databases, path, deb12):
            return len(indices)
        dropped = set()
        for i in indices:
            write(dropped | {i})
            if compiles(databases, path, deb12):
                dropped.add(i)
        write(dropped)
        return len(dropped)
    except BaseException:
        file.write_text(original)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('report', type=Path)
    parser.add_argument('-j', '--jobs', type=int, default=8)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--std', action='store_true', help='standard library headers, also checked with Debian 12')
    options = parser.parse_args()
    plan = candidates(parse(options.report), options.std)
    print(f'{len(plan)} files, {sum(len(v) for v in plan.values())} includes to try')
    if options.dry_run:
        return
    databases = {build: load(build) for build in BUILDS}
    deb12 = {entry['file']: entry for entry in json.loads((ROOTFS / 'build-deb12/compile_commands.json').read_text())} if options.std else None
    with cf.ThreadPoolExecutor(options.jobs) as pool:
        futures = {pool.submit(apply, databases, path, indices, deb12): (path, len(indices)) for path, indices in plan.items()}
        removed = 0
        for future in cf.as_completed(futures):
            path, wanted = futures[future]
            count = future.result()
            removed += count
            print(f'{count}/{wanted} {Path(path).relative_to(ROOT)}')
    print(f'{removed} includes removed')


if __name__ == '__main__':
    main()
