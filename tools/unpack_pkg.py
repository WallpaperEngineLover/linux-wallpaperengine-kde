#!/usr/bin/env python3
"""Extracts files out of a Wallpaper Engine .pkg container (scene.pkg, general.pkg, etc).

Usage:
    unpack_pkg.py <path-to.pkg> <output-dir> [name-filter ...]

With no filters, everything is extracted. Filters are substring matches against
the stored filename (case-insensitive), e.g.:

    unpack_pkg.py scene.pkg out scene.json
    unpack_pkg.py scene.pkg out models/spiritblossomahribase_puppet.mdl
"""
import struct
import sys
import os


def read_u32(f):
    return struct.unpack('<I', f.read(4))[0]


def read_sized_string(f):
    n = read_u32(f)
    return f.read(n).decode('utf-8', errors='replace')


def extract(pkg_path, out_dir, name_filters=None):
    with open(pkg_path, 'rb') as f:
        header = read_sized_string(f)
        if not header.startswith('PKGV'):
            raise ValueError(f"expected PKGV header, got {header!r}")

        count = read_u32(f)
        entries = []
        for _ in range(count):
            name = read_sized_string(f)
            offset = read_u32(f)
            length = read_u32(f)
            entries.append((name, offset, length))

        base = f.tell()
        os.makedirs(out_dir, exist_ok=True)

        for name, offset, length in entries:
            if name_filters and not any(want.lower() in name.lower() for want in name_filters):
                continue
            f.seek(base + offset)
            data = f.read(length)
            dest = os.path.join(out_dir, name.replace('\\', '/'))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, 'wb') as out:
                out.write(data)
            print(f"extracted {name} ({length} bytes)")

        return entries


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    pkg_path = sys.argv[1]
    out_dir = sys.argv[2]
    filters = sys.argv[3:] or None

    all_entries = extract(pkg_path, out_dir, filters)

    if filters is None:
        for entry_name, _, entry_length in all_entries:
            print(entry_name, entry_length)
