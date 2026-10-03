#!/usr/bin/env python3
"""Counts what the installed .tex files use, read like wallpaper64.exe 2.8.42's loader (sub_14015E580).

Usage:
    tex_scan.py [workshop dir] [more dirs...]

Defaults to the Steam workshop folder of Wallpaper Engine plus WE's own assets. Looks at every .tex inside
scene.pkg/general.pkg files and loose .tex files. Prints the container/section versions (TEXV, TEXI, TEXB, TEXS),
texture formats, flag bits and FreeImage types, each with up to five items that have it. Use it before porting
a texture feature to see whether the library has it at all.
"""
import os
import struct
import sys
from collections import defaultdict

DEFAULT_DIRS = [
    '/workspace/SteamLibrary/steamapps/workshop/content/431960',
    '/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets',
]

FORMATS = {
    0: 'ARGB8888', 1: 'RGB888', 2: 'RGB565', 4: 'DXT5', 6: 'DXT3', 7: 'DXT1', 8: 'RG88', 9: 'R8',
    10: 'RG1616f', 11: 'R16f', 12: 'BC7', 13: 'RGBa1010102', 14: 'RGBA16161616f', 15: 'RGB161616f',
}


def cstring(data, pos):
    end = data.find(b'\0', pos)
    if end < 0:
        return None, len(data)
    return data[pos:end].decode('latin-1'), end + 1


def u32(data, pos):
    if pos + 4 > len(data):
        return 0, pos
    return struct.unpack_from('<I', data, pos)[0], pos + 4


def scan_tex(data, found, item):
    def note(key):
        if len(found[key]) < 5:
            found[key].add(item)
        counts[key] += 1

    magic, pos = cstring(data, 0)
    note(f'container {magic}')

    if magic not in ('TEXV0005', 'TEXV0004'):
        return

    # TEXV0004: version 0 TEXI fields and version 0 TEXB body, no tags
    if magic == 'TEXV0004':
        sections = [('TEXI', 0), ('TEXB', 0)]
    else:
        sections = None

    def read_texi(pos, version):
        fmt, pos = u32(data, pos)
        flags, pos = u32(data, pos)
        pos += 16
        if flags & 0x40:
            pos += 4
        if version >= 1:
            pos += 4
        note(f'format {fmt} {FORMATS.get(fmt, "?")}')
        for bit in range(32):
            if flags & (1 << bit):
                note(f'flag 0x{1 << bit:x}')
        return pos, flags

    flags = 0

    if sections:
        pos, flags = read_texi(pos, 0)
        note('TEXB version 0 (TEXV0004)')
        return

    while pos < len(data):
        tag, pos = cstring(data, pos)
        if not tag or len(tag) < 4:
            break
        version = int(tag[4:]) if tag[4:].isdigit() else -1
        if tag.startswith('TEXI'):
            note(f'section {tag}')
            pos, flags = read_texi(pos, version)
        elif tag.startswith('TEXB'):
            note(f'section {tag}')
            if version >= 1:
                images, pos = u32(data, pos)
                if images > 1:
                    note('several images')
            if version >= 3:
                fif, pos = u32(data, pos)
                note(f'freeimage {fif:#x}' if fif != 0xFFFFFFFF else 'freeimage none')
            if version >= 4:
                conditions, pos = u32(data, pos)
                if conditions:
                    note(f'TEXB0004 conditions={min(conditions, 2)}{"+" if conditions > 2 else ""}')
            # the mip data follows, TEXS comes after it but its offset isn't stored: look for the tag
            texs = data.find(b'TEXS', pos)
            if texs < 0:
                break
            pos = texs
        elif tag.startswith('TEXS'):
            note(f'section {tag}')
            break
        else:
            note(f'unknown section {tag}')
            break


counts = defaultdict(int)


def scan_pkg(path, found, item):
    with open(path, 'rb') as f:
        data = f.read()
    n, pos = u32(data, 0)
    header = data[4:4 + n].decode('latin-1', 'replace')
    if not header.startswith('PKGV'):
        return
    pos = 4 + n
    count, pos = u32(data, pos)
    entries = []
    for _ in range(count):
        n, pos = u32(data, pos)
        name = data[pos:pos + n].decode('utf-8', 'replace')
        pos += n
        offset, pos = u32(data, pos)
        length, pos = u32(data, pos)
        entries.append((name, offset, length))
    base = pos
    for name, offset, length in entries:
        if name.lower().endswith('.tex'):
            scan_tex(data[base + offset:base + offset + length], found, item)


def main():
    dirs = sys.argv[1:] or DEFAULT_DIRS
    found = defaultdict(set)

    for root_dir in dirs:
        for root, _, files in os.walk(root_dir):
            rel = os.path.relpath(root, root_dir)
            item = rel.split(os.sep)[0] if rel != '.' else os.path.basename(root_dir)
            for name in files:
                path = os.path.join(root, name)
                if name.endswith('.pkg'):
                    scan_pkg(path, found, item)
                elif name.lower().endswith('.tex'):
                    with open(path, 'rb') as f:
                        scan_tex(f.read(), found, item)

    for key in sorted(counts):
        print(f'{counts[key]:7d}  {key:32s} {" ".join(sorted(found[key]))}')


if __name__ == '__main__':
    main()
