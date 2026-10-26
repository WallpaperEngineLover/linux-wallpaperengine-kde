#!/usr/bin/env python3
"""Static analysis over our own sources (src/, not src/External) with PVS-Studio, the Clang Static Analyzer and clang-tidy.

Usage:
    static_analysis.py [-b build dir] [-o out dir] [-j jobs] [--full] pvs|csa|tidy|all

Needs a build made with CMAKE_EXPORT_COMPILE_COMMANDS=ON (default build dir: build). The compile database comes from
GCC, so the clang tools get -Wno-unknown-warning-option for the GCC-only warning flags in it. Every tool works on a
copy of the database that only holds our own translation units, one entry per file.

Results go to <out dir> (default build/static-analysis, git ignored), one folder per tool, each with a report.txt of
unique findings in `file:line:col: [check] message` form that only lists files under src/ outside src/External, and a
count per check printed at the end:
    pvs/     PVS-Studio (needs a license in ~/.config/PVS-Studio), pvs.log is the raw log, report.txt levels 1-3 of
             GA, 64 and OP, html/ the browsable version
    csa/     analyze-build in <jobs> shards, plist files per shard, failures/ for files clang could not parse
    tidy/    run-clang-tidy with the repo's .clang-tidy, tidy.log is its raw output

--full widens the clang tools: the analyzer also runs the alpha, optin, security and cplusplus checkers (noisier, some
report things that cannot happen), and clang-tidy gets a second run in tidy-full/ with the bugprone, cert, concurrency,
misc, performance and portability groups besides the repo's own list.

Run one tool at a time on a machine others are using, each one starts <jobs> processes.
"""
import argparse
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_SUFFIXES = {'.c', '.cc', '.cpp'}
CLANG_EXTRA_ARG = '-Wno-unknown-warning-option'


def define_count(entry):
    command = entry.get('command') or ' '.join(entry['arguments'])
    return command.count(' -D')


def own_database(build, out):
    entries = json.loads((build / 'compile_commands.json').read_text())
    src = ROOT / 'src'
    external = src / 'External'
    seen = {}
    for entry in entries:
        path = Path(entry['directory'], entry['file']).resolve()
        if path.suffix not in SOURCE_SUFFIXES or src not in path.parents or external in path.parents:
            continue
        # a file built by two targets keeps the entry of the one with the feature defines (ENABLE_WAYLAND, ...)
        if path not in seen or define_count(entry) > define_count(seen[path]):
            seen[path] = entry
    out.mkdir(parents=True, exist_ok=True)
    database = out / 'compile_commands.json'
    database.write_text(json.dumps(list(seen.values()), indent=1))
    return database, list(seen.values())


def ours(path):
    path = Path(path)
    src = ROOT / 'src'
    return src in path.parents and (src / 'External') not in path.parents


def write_report(out, findings):
    unique = sorted(set(findings))
    (out / 'report.txt').write_text(''.join(line + '\n' for _, line in unique))
    counts = Counter(check for check, _ in unique)
    print(f'{len(unique)} unique findings in {out / "report.txt"}')
    for check, count in counts.most_common():
        print(f'{count:6d}  {check}')


def run_pvs(build, out, jobs, full=False):
    folder = out / 'pvs'
    shutil.rmtree(folder, ignore_errors=True)
    database, entries = own_database(build, folder)
    log = folder / 'pvs.log'
    config = folder / 'PVS-Studio.cfg'
    config.write_text(f"rules-config={ROOT / 'tools/pvs-rules.pvsconfig'}\n")
    subprocess.run(['pvs-studio-analyzer', 'analyze', '-f', str(database), '-o', str(log), '-j', str(jobs),
                    '-e', str(ROOT / 'src/External'), '-c', str(config)], check=True)
    levels = 'GA:1,2,3;64:1,2,3;OP:1,2,3'
    text = folder / 'raw.txt'
    subprocess.run(['plog-converter', '-a', levels, '-t', 'errorfile', '-o', str(text), str(log)],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['plog-converter', '-a', levels, '-t', 'fullhtml', '-o', str(folder / 'html'), str(log)],
                   check=True, stdout=subprocess.DEVNULL)
    pattern = re.compile(r'^(.+?):(\d+):(\d+): (?:warning|error|note): (V\d+) (.*)$')
    findings = []
    for line in text.read_text(errors='replace').splitlines():
        match = pattern.match(line)
        if match and ours(match.group(1)):
            path, row, col, code, message = match.groups()
            findings.append((code, f'{path}:{row}:{col}: [{code}] {message}'))
    print(f'{len(entries)} files analyzed')
    write_report(folder, findings)


CSA_FULL_CHECKERS = ['alpha.core', 'alpha.cplusplus', 'alpha.security', 'alpha.unix', 'alpha.deadcode',
                     'optin.cplusplus', 'optin.performance', 'optin.portability', 'security', 'cplusplus', 'nullability']
TIDY_FULL_CHECKS = ('-*,bugprone-*,cert-*,clang-analyzer-*,concurrency-*,misc-*,performance-*,portability-*,'
                    '-misc-non-private-member-variables-in-classes,-misc-include-cleaner')


def run_csa(build, out, jobs, full=False):
    folder = out / 'csa'
    shutil.rmtree(folder, ignore_errors=True)
    database, entries = own_database(build, folder)
    shards = []
    for index in range(jobs):
        part = entries[index::jobs]
        if not part:
            continue
        shard = folder / f'shard{index}'
        shard.mkdir()
        (shard / 'compile_commands.json').write_text(json.dumps(part))
        wrapper = shard / 'clang++'
        wrapper.write_text(f'#!/bin/sh\ncase "$1" in -cc1*) exec clang++ "$@" ;; esac\n'
                           f'exec clang++ {CLANG_EXTRA_ARG} "$@"\n')
        wrapper.chmod(0o755)
        command = ['analyze-build', '--cdb', str(shard / 'compile_commands.json'),
                   '--output', str(shard / 'reports'), '--plist', '--use-analyzer', str(wrapper)]
        if full:
            command += ['-analyzer-config', 'aggressive-binary-operation-simplification=true']
            for checker in CSA_FULL_CHECKERS:
                command += ['--enable-checker', checker]
        shards.append(subprocess.Popen(command))
    for process in shards:
        process.wait()
    findings = []
    failures = 0
    for plist in folder.glob('shard*/reports/*/*.plist'):
        data = plistlib.loads(plist.read_bytes())
        files = data.get('files', [])
        for diag in data.get('diagnostics', []):
            location = diag['location']
            path = files[location['file']]
            if ours(path):
                check = diag.get('check_name', diag.get('category', 'csa'))
                findings.append((check, f"{path}:{location['line']}:{location['col']}: [{check}] "
                                        f"{diag['description']}"))
    for failure in folder.glob('shard*/reports/*/failures/*'):
        failures += 1
    print(f'{len(entries)} files analyzed, {failures} failure reports (files clang could not parse)')
    write_report(folder, findings)


def run_tidy(build, out, jobs, full=False):
    run_tidy_with(build, out / 'tidy', jobs, None)
    if full:
        run_tidy_with(build, out / 'tidy-full', jobs, TIDY_FULL_CHECKS)


def run_tidy_with(build, folder, jobs, checks):
    shutil.rmtree(folder, ignore_errors=True)
    database, entries = own_database(build, folder)
    log = folder / 'tidy.log'
    header_filter = f'{ROOT}/src/(WallpaperEngine|Steam)/.*|{ROOT}/src/[^/]*'
    with open(log, 'w') as handle:
        subprocess.run(['run-clang-tidy', '-p', str(folder), '-j', str(jobs), '-quiet',
                        f'-extra-arg={CLANG_EXTRA_ARG}', f'-header-filter={header_filter}',
                        f'-config-file={ROOT / ".clang-tidy"}', *([f'-checks={checks}'] if checks else [])],
                       stdout=handle, stderr=subprocess.STDOUT, check=False)
    pattern = re.compile(r'^(.+?):(\d+):(\d+): (warning|error): (.*) \[([\w.,-]+)\]$')
    findings = []
    for line in log.read_text(errors='replace').splitlines():
        match = pattern.match(line)
        if match and ours(match.group(1)):
            path, row, col, _, message, check = match.groups()
            findings.append((check, f'{path}:{row}:{col}: [{check}] {message}'))
    print(f'{len(entries)} files analyzed')
    write_report(folder, findings)


TOOLS = {'pvs': run_pvs, 'csa': run_csa, 'tidy': run_tidy}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('tool', choices=[*TOOLS, 'all'])
    parser.add_argument('-b', '--build', type=Path, default=ROOT / 'build')
    parser.add_argument('-o', '--out', type=Path, default=ROOT / 'build' / 'static-analysis')
    parser.add_argument('--full', action='store_true', help='wider clang analyzer and clang-tidy checks')
    parser.add_argument('-j', '--jobs', type=int, default=8)
    args = parser.parse_args()
    if not (args.build / 'compile_commands.json').exists():
        sys.exit(f'{args.build}/compile_commands.json is missing, configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
    for name in (TOOLS if args.tool == 'all' else [args.tool]):
        print(f'== {name} ==', flush=True)
        TOOLS[name](args.build.resolve(), args.out.resolve(), args.jobs, args.full)


if __name__ == '__main__':
    main()
