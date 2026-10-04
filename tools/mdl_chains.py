#!/usr/bin/env python3
"""Lists the MDLS IK chains of puppet .mdl files, read like wallpaper64.exe 2.8.42 (sub_140261880).

Usage:
    mdl_chains.py <item id>... [--dir workshop dir]

Per puppet: MDLS version, bone count, which bone JSON keys are set (and how many bones have ik/se/re on), the
extra/constraint/chain counts and each chain entry. Entries with flags & 4 are physics (kinematic rope) chains.
"""
import struct, sys, glob, json, collections

def pkg_entries(pkg):
    with open(pkg, 'rb') as f:
        n = struct.unpack('<I', f.read(4))[0]; f.read(n)
        c = struct.unpack('<I', f.read(4))[0]; ents = []
        for _ in range(c):
            n = struct.unpack('<I', f.read(4))[0]; name = f.read(n).decode('utf-8', 'replace')
            o, l = struct.unpack('<II', f.read(8)); ents.append((name, o, l))
        base = f.tell()
        for name, o, l in ents:
            if name.endswith('.mdl'):
                f.seek(base + o); yield name, f.read(l)

class R:
    def __init__(self, d, o): self.d = d; self.o = o
    # `r.o += r.f()` would load r.o before f() advances it
    def f(self, fmt):
        v = struct.unpack_from('<' + fmt, self.d, self.o); self.o += struct.calcsize(fmt)
        return v if len(v) > 1 else v[0]
    def s(self):
        e = self.d.index(b'\0', self.o); v = self.d[self.o:e].decode('utf-8', 'replace'); self.o = e + 1; return v

def scan(name, d):
    o = d.find(b'MDLS')
    if o < 0:
        return
    r = R(d, o + 9); ver = int(d[o + 4:o + 8]); end = r.f('I'); nb = r.f('I')
    keys = collections.Counter(); on = collections.Counter()
    for _ in range(nb):
        r.s(); r.f('I'); r.f('i'); mb = r.f('I'); r.o += mb; js = r.s()
        if js:
            j = json.loads(js)
            keys.update(j.keys())
            on.update(k for k in ('ik', 'ikce', 'se', 're') if j.get(k) is True)
    print(f'  {name} MDLS v{ver} bones={nb} on={dict(on)} keys={sorted(keys)}')
    if ver < 2:
        return
    extras = r.f('H')
    for _ in range(extras):
        r.s(); r.o += 8 + 64
    if r.f('B'):
        r.o += 64 * (nb + extras)
    constraints = r.f('I')
    for _ in range(constraints):
        r.o += 12
        if (r.f('I') if ver >= 4 else 0) & 2:
            r.o += 8
    groups = r.f('H'); r.o += 4 * groups
    for _ in range(groups):
        children = r.f('H'); r.o += 16 * children
    chains = r.f('H'); entries = []
    for _ in range(chains):
        root = r.f('I'); n = r.f('I'); r.o += 4 * n
        for _ in range(r.f('H')):
            r.f('I')
            for _ in range(r.f('H')):
                endBone, flags, _, _ = r.f('IIff')
                bones = [r.f('i') for _ in range(r.f('H'))]
                entries.append((root, endBone, flags, bones))
    physics = sum(1 for e in entries if e[2] & 4)
    print(f'    extras={extras} constraints={constraints} chains={chains} physics entries={physics} ({r.o}/{end})')
    for root, endBone, flags, bones in entries:
        print(f'      root {root} end {endBone} flags {flags:#x} bones {bones}')

args = sys.argv[1:]
root = '/workspace/SteamLibrary/steamapps/workshop/content/431960'
if '--dir' in args:
    i = args.index('--dir'); root = args[i + 1]; del args[i:i + 2]
for item in args:
    print('=====', item)
    for pkg in glob.glob(f'{root}/{item}/*.pkg'):
        for name, data in pkg_entries(pkg):
            scan(name, data)
    for p in glob.glob(f'{root}/{item}/**/*.mdl', recursive=True):
        scan(p, open(p, 'rb').read())
