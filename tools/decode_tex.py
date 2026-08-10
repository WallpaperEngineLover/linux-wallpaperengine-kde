#!/usr/bin/env python3
"""Decodes a Wallpaper Engine .tex file's first mipmap into a plain image file.

Only handles the common case: TEXV0005/TEXI0001 header, TEXB0003/TEXB0004 container,
FreeImage-encoded payload (FIF_PNG in every sample seen so far) - i.e. the mipmap bytes
are themselves a complete PNG/etc file, not raw pixel data. Doesn't touch mipmaps beyond
the first, animated (TEXS*) textures, or DXT/BC-compressed formats.

Usage:
    decode_tex.py <path-to.tex> <output-image-path>

Needs: pip install lz4 (only when the mipmap is LZ4-compressed, i.e. most of them).
"""
import struct
import sys

try:
    import lz4.block
except ImportError:
    lz4 = None


def decode(path, out_path):
    f = open(path, 'rb')
    magic1 = f.read(9)
    assert magic1[:8] == b'TEXV0005', magic1
    magic2 = f.read(9)
    assert magic2[:8] == b'TEXI0001', magic2
    fmt, flags, tex_w, tex_h, w, h = struct.unpack('<6I', f.read(24))
    f.read(4)  # ignored trailing header field

    container_magic = f.read(9)
    image_count = struct.unpack('<I', f.read(4))[0]
    fif = None
    if container_magic[:8] in (b'TEXB0003', b'TEXB0004'):
        fif = struct.unpack('<I', f.read(4))[0]
        if container_magic[:8] == b'TEXB0004':
            f.read(4)  # isVideoMp4 flag
    print('container', container_magic[:8], 'fif', fif, 'imageCount', image_count, 'declaredSize', w, h)

    mipmap_count = struct.unpack('<I', f.read(4))[0]
    print('mipmapCount', mipmap_count)
    mip_w, mip_h = struct.unpack('<II', f.read(8))
    compression, uncompressed_size, compressed_size = struct.unpack('<Iii', f.read(12))
    print('mip0', mip_w, mip_h, 'compression', compression,
          'uncompressedSize', uncompressed_size, 'compressedSize', compressed_size)

    if compression == 0:
        # misnamed in the engine's own parser too: compressedSize holds the real length here
        uncompressed_size = compressed_size
        data = f.read(uncompressed_size)
    elif compression == 1:
        if lz4 is None:
            raise SystemExit("mipmap is LZ4-compressed - install with: pip install lz4")
        comp = f.read(compressed_size)
        data = lz4.block.decompress(comp, uncompressed_size=uncompressed_size)
    else:
        raise SystemExit(f"unknown compression mode {compression}")

    open(out_path, 'wb').write(data)
    print('wrote', out_path, len(data), 'bytes, first bytes:', data[:8])


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(1)
    decode(sys.argv[1], sys.argv[2])
