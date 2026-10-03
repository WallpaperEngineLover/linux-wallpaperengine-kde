#!/usr/bin/env python3
"""Lists vertex/fragment shader pairs whose varyings differ in type between the two stages (vec2 in the vertex
shader, vec4 in the fragment one, ...), over WE's assets and every installed workshop item (scene.pkg and loose
shaders/ dirs). Varyings declared more than once (one per #if branch) are marked, ShaderUnit's varying
compatibility passes leave those alone.

Usage:
    shader_varying_scan.py [assets dir] [workshop dir]
"""
import collections
import glob
import os
import re
import struct
import sys

ASSETS = sys.argv[1] if len(sys.argv) > 1 else '/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets'
WORKSHOP = sys.argv[2] if len(sys.argv) > 2 else '/workspace/SteamLibrary/steamapps/workshop/content/431960'
DECLARATION = re.compile(r'\bvarying\s+(\w+)\s+(\w+)\s*;')


def pkg_shaders(path):
    with open(path, 'rb') as f:
        f.read(struct.unpack('<I', f.read(4))[0])
        entries = []
        for _ in range(struct.unpack('<I', f.read(4))[0]):
            name = f.read(struct.unpack('<I', f.read(4))[0]).decode('utf-8', errors='replace')
            entries.append((name, *struct.unpack('<II', f.read(8))))
        base = f.tell()
        for name, offset, length in entries:
            if name.startswith('shaders/') and name.endswith(('.vert', '.frag')):
                f.seek(base + offset)
                yield name[len('shaders/'):], f.read(length).decode('utf-8', errors='replace')


def main():
    files = {}
    shaders = os.path.join(ASSETS, 'shaders')
    for path in glob.glob(shaders + '/**/*.vert', recursive=True) + glob.glob(shaders + '/**/*.frag', recursive=True):
        files[os.path.relpath(path, shaders)] = open(path, errors='replace').read()
    for pkg in glob.glob(os.path.join(WORKSHOP, '*', 'scene.pkg')):
        for name, source in pkg_shaders(pkg):
            files.setdefault(name, source)
    for root in glob.glob(os.path.join(WORKSHOP, '*', 'shaders')):
        for path in glob.glob(root + '/**/*.vert', recursive=True) + glob.glob(root + '/**/*.frag', recursive=True):
            files.setdefault(os.path.relpath(path, root), open(path, errors='replace').read())

    for name, vertex in sorted(files.items()):
        if not name.endswith('.vert'):
            continue
        fragment = files.get(name[:-5] + '.frag')
        if fragment is None:
            continue
        vertex_types = collections.defaultdict(list)
        fragment_types = collections.defaultdict(list)
        for kind, varying in DECLARATION.findall(vertex):
            vertex_types[varying].append(kind)
        for kind, varying in DECLARATION.findall(fragment):
            fragment_types[varying].append(kind)
        for varying, kinds in vertex_types.items():
            other = fragment_types.get(varying)
            if other and set(kinds) != set(other):
                note = '  (declared per #if branch)' if len(kinds) > 1 or len(other) > 1 else ''
                print(f'{name} {varying}: vertex {kinds}, fragment {other}{note}')


if __name__ == '__main__':
    main()
