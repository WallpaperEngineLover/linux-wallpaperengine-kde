#!/usr/bin/env python3
"""Patches a copy of wallpaper64.exe 2.8.42 so the LED plugin loads without the UI: sub_140065110 (the plugin scan) always runs
with its "load" argument set (0x140065122: movzx ebx, dl -> mov bl, 1; nop). Keeps <exe>.orig.

    we_patch_led.py <path to the copy of wallpaper64.exe>
"""
import shutil, struct, sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
pe = struct.unpack_from('<I', data, 0x3c)[0]
sections = struct.unpack_from('<H', data, pe + 6)[0]
optional = struct.unpack_from('<H', data, pe + 20)[0]
base = struct.unpack_from('<Q', data, pe + 48)[0]
rva = 0x140065122 - base

for i in range(sections):
    o = pe + 24 + optional + 40 * i
    size, va, raw_size, raw = struct.unpack_from('<IIII', data, o + 8)
    if va <= rva < va + max(size, raw_size):
        at = raw + rva - va
        if data[at:at + 3] == bytes.fromhex('b30190'):
            sys.exit('already patched')
        if data[at:at + 3] != bytes.fromhex('0fb6da'):
            sys.exit('not wallpaper64.exe 2.8.42: unexpected bytes %s' % data[at:at + 3].hex())
        shutil.copy(path, path + '.orig')
        data[at:at + 3] = bytes.fromhex('b30190')
        open(path, 'wb').write(data)
        print('patched', path)
        break
else:
    sys.exit('address not in any section')
