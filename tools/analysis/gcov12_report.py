#!/usr/bin/env python3
"""Line counts of a coverage build made with GCC 12 (the portable one that ran in the VMs) as a gcovr style JSON report.
gcovr drops some of those files and the host's gcov cannot read the data, so the Debian 12 rootfs's own gcov runs over every
.gcda of the folder (tools/analysis/gcov_deb12.sh, text output with preserved paths) and the .gcov files are read here.

Usage: gcov12_report.py <folder with the .gcno and .gcda files> <out.json> [source prefix, default src/WallpaperEngine/]"""
import glob, json, os, re, subprocess, sys, tempfile

folder, out = sys.argv[1:3]
prefix = sys.argv[3] if len(sys.argv) > 3 else 'src/WallpaperEngine/'
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.abspath(os.path.join(here, '..', '..'))
wanted = repo.replace('/', '#') + '#' + prefix.replace('/', '#')
counts = {}

with tempfile.TemporaryDirectory(dir=folder) as work:
    script = os.path.join(work, 'run.sh')
    open(script, 'w').write('#!/bin/bash\ncd "$1"\nfind "$2" -name "*.gcda" | while read -r f; do gcov -p -o "$(dirname "$f")" "$f" > /dev/null 2>&1; done\nexit 0\n')
    os.chmod(script, 0o755)
    env = dict(os.environ, GCOV_BIND=os.path.abspath(folder), PROOT=os.environ.get('PROOT', os.path.expanduser('~/.local/share/arch-proot/proot')))
    env['BINDS'] = '-b %s:%s -b %s:%s' % (repo, repo, os.path.abspath(folder), os.path.abspath(folder))
    subprocess.run([os.path.join(here, '..', 'distro_rootfs.sh'), 'run',
                    os.environ.get('GCOV_ROOTFS', os.path.expanduser('~/.local/share/distro-rootfs/debian-12')), script,
                    work, os.path.abspath(folder)], env=env, check=True)

    for path in glob.glob(os.path.join(work, wanted + '*.gcov')):
        source = None
        for line in open(path, errors='replace'):
            fields = line.rstrip('\n').split(':', 2)
            if len(fields) < 3:
                continue
            count, number = fields[0].strip(), fields[1].strip()
            if number == '0':
                if fields[2].startswith('Source:'):
                    source = os.path.relpath(fields[2][7:], repo)
                continue
            if count == '-' or source is None:
                continue
            value = 0 if count[0] in '#=' else int(count.rstrip('*'))
            lines = counts.setdefault(source, {})
            lines[int(number)] = lines.get(int(number), 0) + value

json.dump({'files': [{'file': name, 'lines': [{'line_number': n, 'count': c} for n, c in sorted(lines.items())]}
                     for name, lines in sorted(counts.items())]}, open(out, 'w'))
print(len(counts), 'files')
