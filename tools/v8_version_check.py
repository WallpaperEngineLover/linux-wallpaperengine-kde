#!/usr/bin/env python3
"""Compiles the scripting sources against the V8 headers of several Node.js releases (-fsyntax-only), since
distributions ship different V8 versions in libnode (Ubuntu 24.04: 10.2, Fedora 44: 12.4/13.6, current Node: 14.x).

    tools/v8_version_check.py build v18.19.1 v22.23.1 v24.18.0 v26.10.0

Takes the compile commands of an already configured build (CMAKE_EXPORT_COMPILE_COMMANDS is on), swaps the V8
include directory for each release's headers (downloaded from nodejs.org into ~/.cache/lwe-v8-headers) and prints
every error or warning. Exits 1 when anything fails.
"""

import argparse
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

CACHE = Path.home () / '.cache/lwe-v8-headers'
# the files that include V8 headers
PATTERNS = (
    '/Scripting/', 'Objects/CImage.cpp', 'Objects/CMesh.cpp', 'Testing/Cases/ModuleNamespaceTiming.cpp',
    'Testing/Cases/ScriptWatchdog.cpp', 'Testing/Cases/ScriptLimits.cpp'
)


def headers (version):
    target = CACHE / version

    if not (target / 'include/node/v8.h').exists ():
        target.mkdir (parents = True, exist_ok = True)
        url = f'https://nodejs.org/dist/{version}/node-{version}-headers.tar.gz'
        archive = target / 'headers.tar.gz'
        urllib.request.urlretrieve (url, archive)

        with tarfile.open (archive) as tar:
            for member in tar.getmembers ():
                member.name = member.name.split ('/', 1)[-1]
                if member.name:
                    tar.extract (member, target, filter = 'data')

        archive.unlink ()

    return target / 'include/node'


def v8_include (command):
    for argument in command:
        if argument.startswith ('-I') or argument.startswith ('-isystem'):
            path = argument[2:] if argument.startswith ('-I') else argument[8:]
            if (Path (path) / 'v8.h').exists ():
                return path

    return None


def check (entry, version, include):
    command = shlex.split (entry['command'])
    original = v8_include (command)
    swapped = []
    skip = False

    for argument in command:
        if skip:
            skip = False
            continue
        if argument == '-o':
            skip = True
            continue
        swapped.append (argument.replace (original, str (include)) if original else argument)

    # the file that derives from a V8 class is compiled for real: libnode has no RTTI, so a reference to the typeinfo of
    # one of V8's classes is a link error on the distributions whose headers need it (undefined reference to `typeinfo
    # for v8::ArrayBuffer::Allocator'), which -fsyntax-only never shows
    derives = entry['file'].endswith ('LimitedAllocator.cpp')

    if derives:
        with tempfile.TemporaryDirectory () as scratch:
            object_file = Path (scratch) / 'check.o'
            result = subprocess.run (
                swapped + ['-c', '-o', str (object_file)], cwd = entry['directory'], capture_output = True, text = True
            )
            symbols = subprocess.run (['nm', '-C', object_file], capture_output = True, text = True).stdout \
                if result.returncode == 0 else ''
    else:
        result = subprocess.run (swapped + ['-fsyntax-only'], cwd = entry['directory'], capture_output = True, text = True)
        symbols = ''

    problems = [line for line in result.stderr.splitlines () if 'error' in line or 'warning' in line]
    problems += [f'references {line.strip ()}' for line in symbols.splitlines () if 'typeinfo for v8::' in line]

    return version, entry['file'], result.returncode, problems


def main ():
    parser = argparse.ArgumentParser (description = __doc__, formatter_class = argparse.RawDescriptionHelpFormatter)
    parser.add_argument ('build', type = Path, help = 'configured build directory')
    parser.add_argument ('versions', nargs = '+', help = 'Node.js releases, e.g. v22.23.1')
    args = parser.parse_args ()

    commands = json.loads ((args.build / 'compile_commands.json').read_text ())
    entries = {entry['file']: entry for entry in commands if any (pattern in entry['file'] for pattern in PATTERNS)}
    jobs = [(entry, version, headers (version)) for version in args.versions for entry in entries.values ()]
    failed = False

    with concurrent.futures.ThreadPoolExecutor (os.cpu_count ()) as pool:
        for version, file, code, problems in pool.map (lambda job: check (*job), jobs):
            if code != 0 or problems:
                failed = True
                print (f'{version} {file}')
                print ('\n'.join ('  ' + line for line in problems[:20]))

    print (f'{len (jobs)} compiles, {"failures" if failed else "all clean"}')
    sys.exit (1 if failed else 0)


if __name__ == '__main__':
    main ()
