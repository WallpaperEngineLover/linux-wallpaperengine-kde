#!/usr/bin/env python3
"""Runs a program the way Steam's `proton run` does, minus steam.exe: the prefix setup and the per-run environment
(DXVK/vkd3d-proton dll overrides, fonts, protonfixes) all come from Proton's own `run` path, only its last step,
starting the program through steam.exe, becomes `wine <args>`. `runinprefix` is no substitute, it skips the setup
and with it the d3d11/dxgi=n overrides, so WE ends up on wined3d. Valve's steam.exe also asserts without a running
Steam client (GE's only warns), the dialog blocks the run.

    proton_run.py <proton script> <program> [args...]     with STEAM_COMPAT_DATA_PATH etc. set like for proton
"""
import os, sys, textwrap

path = sys.argv[1]
args = sys.argv[2:]
src = open(path).read()
head, sep, body = src.partition('\nif __name__ == "__main__":\n')
if not sep:
    sys.exit('%s: no main block' % path)
# the script imports filelock, protonfixes etc. from its own folder
sys.path.insert(0, os.path.dirname(os.path.abspath(path)))
g = {'__name__': 'proton', '__file__': path}
sys.argv = [path, 'run'] + args
exec(compile(head, path, 'exec'), g)


def run(self):
    return self.run_proc([g['g_proton'].wine_bin] + args)


g['Session'].run = run
# the block ends at the first unindented line (the file ends with pylint/vim comments)
lines = []
for line in body.splitlines(True):
    if line.strip() and not line[0].isspace():
        break
    lines.append(line)
exec(compile(textwrap.dedent(''.join(lines)), path, 'exec'), g)
