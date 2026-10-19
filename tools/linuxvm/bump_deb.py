#!/usr/bin/env python3
"""bump_deb.py <in.deb> <out.deb> <version> - the same package with another Version in its control file, so a .deb can
stand in for a newer release (fake_github.py). Needs no dpkg: a .deb is an ar archive and control.tar.* holds the file."""
import io
import re
import sys
import tarfile


def read_ar(data):
    assert data[:8] == b"!<arch>\n"
    pos, members = 8, []
    while pos < len(data):
        header = data[pos:pos + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58])
        members.append((name, data[pos + 60:pos + 60 + size], header))
        pos += 60 + size + (size & 1)
    return members


def bump_control(blob, name, version):
    mode = {"gz": "gz", "xz": "xz", "zst": None}.get(name.rsplit(".", 1)[-1])
    if mode is None:
        sys.exit(f"{name}: compression not supported, repack the .deb with xz or gzip")
    out = io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:" + mode) as src, tarfile.open(fileobj=out, mode="w:" + mode) as dst:
        for member in src:
            data = src.extractfile(member).read() if member.isfile() else None
            if member.name.endswith("control") and data is not None:
                data = re.sub(rb"(?m)^Version: .*$", b"Version: " + version.encode(), data)
                member.size = len(data)
            dst.addfile(member, io.BytesIO(data) if data is not None else None)
    return out.getvalue()


def main():
    source, target, version = sys.argv[1:4]
    members = read_ar(open(source, "rb").read())
    out = bytearray(b"!<arch>\n")
    for name, blob, header in members:
        if name.startswith("control.tar"):
            blob = bump_control(blob, name, version)
        out += header[:48] + str(len(blob)).ljust(10).encode() + header[58:60] + blob + (b"\n" if len(blob) & 1 else b"")
    open(target, "wb").write(out)


main()
