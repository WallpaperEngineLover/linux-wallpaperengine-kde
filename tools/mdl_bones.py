#!/usr/bin/env python3
"""Dumps a puppet .mdl file's MDLS bone array: parent index, bind-pose matrix determinant
(to spot a mirrored/negative-scale bone), and translation - without needing a debug build.

Mirrors the parsing in CImage.cpp's parsePuppetBones()/readPuppetMeshData(): MDLS header is
9 bytes, then nextSectionOffset(u32) + boneCount(u32), then per bone: 1 padding byte + type(u32)
+ parent(i32) + matrixBytes(u32) + [16 floats if matrixBytes==64] + null-terminated name. The
16 floats are the file's row-major matrix - reshaping them row-major (not glm's column-major
convention) gives the actual authored matrix back, which is what this script does.

Usage:
    mdl_bones.py <path-to-puppet.mdl>

Needs: pip install numpy
"""
import struct
import sys

import numpy as np


def read_cstr(data, offset):
    end = data.index(b'\x00', offset)
    return data[offset:end].decode('latin1', errors='replace'), end + 1


def main(path):
    data = open(path, 'rb').read()
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

    for i in range(bone_count):
        offset += 1  # padding byte
        offset += 4  # type, unused
        parent = struct.unpack_from('<i', data, offset)[0]
        offset += 4
        matrix_bytes = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        matrix = None
        if matrix_bytes == 64:
            values = struct.unpack_from('<16f', data, offset)
            offset += 64
            matrix = np.array(values, dtype=float).reshape(4, 4)  # row-major as authored
        else:
            offset += matrix_bytes
        name, offset = read_cstr(data, offset)

        if matrix is None:
            print(f"{i} parent={parent} name={name!r} NO MATRIX ({matrix_bytes} bytes)")
            continue

        det = np.linalg.det(matrix[:3, :3])
        translation = matrix[3, :3]  # row-vector convention: translation lives in the last row
        flag = "  <-- MIRRORED/NEGATIVE SCALE" if det < 0 else ""
        print(f"{i} parent={parent} name={name!r} det3x3={det:.5f} translation={translation}{flag}")


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        raise SystemExit(1)
    main(sys.argv[1])
