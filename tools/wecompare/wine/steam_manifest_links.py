#!/usr/bin/env python3
"""Restores what DepotDownloader leaves out of a Steam depot: symlinks (written as empty files), the executable bit
Steam gives every file on Linux, and steampipe_fixups.json (empty dirs, read-only files) when the depot has one.

    steam_manifest_links.py <install dir> [depot key hex]     reads <install dir>/.DepotDownloader/*.manifest

Link targets stay encrypted in the saved manifest (only file names get decrypted), the depot key decrypts them
(Steam's symmetric scheme: AES-256-ECB encrypted IV, then AES-256-CBC). Needs pycryptodomex for that (the
steam package's venv has it).
"""
import base64, glob, os, stat, struct, subprocess, sys

PAYLOAD_MAGIC = 0x71F617D0
FLAG_SYMLINK = 0x200
FLAG_DIRECTORY = 0x40


def varint(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7f) << s; s += 7
        if c < 0x80:
            return v, i


def fields(b):
    i = 0
    while i < len(b):
        key, i = varint(b, i)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, i = varint(b, i)
        elif wt == 2:
            n, i = varint(b, i)
            v = b[i:i + n]; i += n
        elif wt == 1:
            v = b[i:i + 8]; i += 8
        elif wt == 5:
            v = b[i:i + 4]; i += 4
        else:
            raise ValueError('wire type %d' % wt)
        yield num, v


def mappings(path):
    data = open(path, 'rb').read()
    magic, n = struct.unpack_from('<II', data, 0)
    if magic != PAYLOAD_MAGIC:
        sys.exit('%s: not a Steam manifest payload' % path)
    for num, m in fields(data[8:8 + n]):
        if num != 1:
            continue
        f = dict(fields(m))
        yield f.get(1, b'').decode().replace('\\', '/'), f.get(3, 0), f.get(7, b'').decode()


def decrypt(target, key):
    from Cryptodome.Cipher import AES
    from Cryptodome.Util.Padding import unpad
    raw = base64.b64decode(target)
    iv = AES.new(key, AES.MODE_ECB).decrypt(raw[:16])
    return unpad(AES.new(key, AES.MODE_CBC, iv).decrypt(raw[16:]), 16).rstrip(b'\0').decode()


root = sys.argv[1]
key = bytes.fromhex(sys.argv[2]) if len(sys.argv) > 2 else None
links = 0
for man in glob.glob(os.path.join(root, '.DepotDownloader', '*.manifest')):
    for name, flags, target in mappings(man):
        p = os.path.join(root, name)
        if flags & FLAG_SYMLINK and target:
            if key:
                target = decrypt(target, key)
            if os.path.islink(p) or (os.path.isfile(p) and os.path.getsize(p) == 0):
                os.remove(p)
            if not os.path.lexists(p):
                os.makedirs(os.path.dirname(p), exist_ok=True)
                os.symlink(target, p)
                links += 1
for d, _, files in os.walk(root):
    for f in files:
        p = os.path.join(d, f)
        if not os.path.islink(p):
            os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
fix = os.path.join(root, 'steampipe_fixups.py')
if os.path.exists(fix):
    subprocess.run([sys.executable, '-I', fix, 'restore', root], check=True)
print('%d symlinks restored' % links)
