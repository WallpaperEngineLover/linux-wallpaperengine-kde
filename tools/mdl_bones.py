#!/usr/bin/env python3
"""Dumps a puppet .mdl file's MDLS bone array: parent index, bind-pose matrix determinant
(to spot a mirrored/negative-scale bone), and translation - without needing a debug build.

Mirrors the parsing in CImage.cpp's parsePuppetBones()/readPuppetMeshData(): MDLS header is
9 bytes, then nextSectionOffset(u32) + boneCount(u32), then per bone: null-terminated name
+ type(u32) + parent(i32) + matrixBytes(u32) + [16 floats if matrixBytes==64] + null-terminated
extra string (empty for most rigs, jiggle/physics JSON for some). The 16 floats are the file's
row-major matrix - reshaping them row-major (not glm's column-major convention) gives the actual authored matrix back, which is what this script does.

With MDLS version 2+ and its flag byte set, a second matrix per bone follows: the rest pose the animation starts
from (the records above are the bind pose the vertices and inverse bind matrices are in, wallpaper64.exe 2.8.42
sub_1401FBAE0/sub_1401FDF90). Those are printed next to the record's translation. The MDLV header (version,
format, materials, vertex/index byte counts per mesh) is printed first.

Usage:
    mdl_bones.py <path-to-puppet.mdl>
    mdl_bones.py --scan [workshop dir]   every .mdl inside the installed scene.pkg files whose rest pose differs
                                         from the bind pose (default dir: the sandbox workshop library)

Needs: pip install numpy
"""
import glob
import os
import struct
import sys

import numpy as np


def read_cstr(data, offset):
    end = data.index(b'\x00', offset)
    return data[offset:end].decode('latin1', errors='replace'), end + 1


def print_meshes(data):
    tag, offset = read_cstr(data, 0)
    if not tag.startswith('MDLV'):
        return
    version = int(tag[4:])
    default_format, materials_per_mesh, mesh_count = struct.unpack_from('<III', data, offset)
    offset += 12
    print(tag, 'defaultFormat', hex(default_format), 'meshes', mesh_count)
    for mesh in range(mesh_count):
        materials = []
        for _ in range(materials_per_mesh):
            material, offset = read_cstr(data, offset)
            materials.append(material)
        flags = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        if flags & 4:
            offset += 4
        if version >= 17:
            offset += 24
        vertex_format = default_format
        if version >= 15:
            vertex_format = struct.unpack_from('<I', data, offset)[0]
            offset += 4
        vertex_bytes = struct.unpack_from('<I', data, offset)[0]
        offset += 4 + vertex_bytes
        index_bytes = struct.unpack_from('<I', data, offset)[0]
        offset += 4 + index_bytes
        print(f'  mesh {mesh} {materials} flags={hex(flags)} format={hex(vertex_format)} '
              f'vertexBytes={vertex_bytes} indexBytes={index_bytes}')
        if version >= 21:
            print('  (version 21+ trailing per-mesh records not walked, only the first mesh is reliable)')
            break


def read_bones(data, mdls_offset):
    """Returns (version, [(name, parent, matrix or None, extra)], [rest matrices] or None)."""
    version = int(data[mdls_offset + 4:mdls_offset + 8])
    offset = mdls_offset + 13
    bone_count = struct.unpack_from('<I', data, offset)[0]
    offset += 4
    bones = []
    for i in range(bone_count):
        name, offset = read_cstr(data, offset)
        offset += 4
        parent, matrix_bytes = struct.unpack_from('<iI', data, offset)
        offset += 8
        matrix = None
        if matrix_bytes == 64:
            matrix = np.array(struct.unpack_from('<16f', data, offset), dtype=float).reshape(4, 4)
        elif matrix_bytes > 4096:
            raise ValueError(f"bone {i} has an implausible matrix byte count ({matrix_bytes}), layout desynced")
        offset += matrix_bytes
        extra, offset = read_cstr(data, offset)
        bones.append((name, parent, matrix, extra))
    if version < 2:
        return version, bones, None
    extra_count = struct.unpack_from('<H', data, offset)[0]
    offset += 2
    for _ in range(extra_count):
        _, offset = read_cstr(data, offset)
        offset += 8 + 64
    if not data[offset]:
        return version, bones, None
    offset += 1
    rest = [np.array(struct.unpack_from('<16f', data, offset + 64 * i), dtype=float).reshape(4, 4)
            for i in range(bone_count)]
    return version, bones, rest


def scan(workshop):
    for pkg in sorted(glob.glob(os.path.join(workshop, '*', 'scene.pkg'))):
        with open(pkg, 'rb') as f:
            f.read(struct.unpack('<I', f.read(4))[0])
            entries = []
            for _ in range(struct.unpack('<I', f.read(4))[0]):
                name = f.read(struct.unpack('<I', f.read(4))[0]).decode('utf-8', errors='replace')
                entries.append((name, *struct.unpack('<II', f.read(8))))
            base = f.tell()
            for name, offset, length in entries:
                if not name.endswith('.mdl'):
                    continue
                f.seek(base + offset)
                data = f.read(length)
                mdls = data.find(b'MDLS')
                if mdls < 0:
                    continue
                try:
                    _, bones, rest = read_bones(data, mdls)
                except (ValueError, struct.error) as ex:
                    print(pkg.split(os.sep)[-2], name, 'ERROR', ex)
                    continue
                if rest is None:
                    continue
                difference = max(np.abs(r - b[2]).max() for r, b in zip(rest, bones) if b[2] is not None)
                if difference > 0.01:
                    print(pkg.split(os.sep)[-2], name, f'rest pose differs from bind pose by up to {difference:.1f}')


def main(path):
    data = open(path, 'rb').read()
    print_meshes(data)
    marker_size = 9
    mdls_offset = data.find(b'MDLS', marker_size)
    if mdls_offset < 0:
        raise SystemExit("no MDLS section found - not a puppet mesh, or a layout this script doesn't handle")
    print('MDLS at', mdls_offset, 'file size', len(data))

    offset = mdls_offset + 9  # skip the MDLS0001-style 9-byte magic
    next_section_offset = struct.unpack_from('<I', data, offset)[0]
    offset += 4
    bone_count = struct.unpack_from('<I', data, offset)[0]
    offset += 4
    print('nextSectionOffset', next_section_offset, 'boneCount', bone_count)

    _, bones, rest = read_bones(data, mdls_offset)
    for i, (name, parent, matrix, extra) in enumerate(bones):
        extra_note = f" extra={extra}" if extra else ""
        if matrix is None:
            print(f"{i} parent={parent} name={name!r} NO MATRIX{extra_note}")
            continue
        det = np.linalg.det(matrix[:3, :3])
        translation = matrix[3, :3]  # row-vector convention: translation lives in the last row
        flag = "  <-- MIRRORED/NEGATIVE SCALE" if det < 0 else ""
        rest_note = f" rest={rest[i][3, :3]}" if rest is not None else ""
        print(f"{i} parent={parent} name={name!r} det3x3={det:.5f} translation={translation}{rest_note}{flag}"
              f"{extra_note}")


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == '--scan':
        scan(sys.argv[2] if len(sys.argv) > 2 else '/workspace/SteamLibrary/steamapps/workshop/content/431960')
    elif len(sys.argv) == 2:
        main(sys.argv[1])
    else:
        print(__doc__)
        raise SystemExit(1)
