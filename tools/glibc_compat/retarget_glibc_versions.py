#!/usr/bin/env python3
"""Lowers a shared library's glibc requirement for symbols lwe_glibc_compat.c provides.

usage: retarget_glibc_versions.py <floor> <provided symbols, comma separated> <library>...

Every GLIBC_x version a library needs that is newer than <floor> (e.g. 2.35) is rewritten in its version needs
(.gnu.version_r) to GLIBC_2.2.5, which every x86_64 glibc defines, so the loader no longer refuses it on an older
glibc. The symbols bound to that version then can't match libc's own versioned copies and bind to the next
unversioned definition in the global scope, the compat library (add it as a dependency with patchelf). Refuses
when an undefined symbol of such a version isn't in the provided list. Prints the libraries it changed.
"""
import struct
import sys

SHT_DYNSYM = 11
SHT_GNU_VERNEED = 0x6FFFFFFE
SHT_GNU_VERSYM = 0x6FFFFFFF
BASE_VERSION = b"GLIBC_2.2.5"


def elf_hash(name):
    h = 0
    for c in name:
        h = (h << 4) + c
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


def version_tuple(name):
    return tuple(int(p) for p in name[len("GLIBC_"):].split("."))


def cstr(data, offset):
    return data[offset:data.index(b"\0", offset)]


def retarget(path, floor, provided):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return False

    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = []
    for i in range(shnum):
        name, stype, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", data, shoff + i * shentsize)
        sections.append({"type": stype, "offset": offset, "size": size, "link": link, "info": info,
                         "entsize": entsize})

    verneed = next((s for s in sections if s["type"] == SHT_GNU_VERNEED), None)
    if verneed is None:
        return False
    strtab = sections[verneed["link"]]["offset"]

    # vernaux entries of libc.so.6 newer than the floor, and where GLIBC_2.2.5's name sits in .dynstr
    base_name = None
    newer = {}
    offset = verneed["offset"]
    for _ in range(verneed["info"]):
        vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", data, offset)
        aux = offset + vn_aux
        for _ in range(vn_cnt):
            vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", data, aux)
            name = cstr(data, strtab + vna_name)
            if name == BASE_VERSION:
                base_name = vna_name
            elif name.startswith(b"GLIBC_") and version_tuple(name.decode()) > floor:
                newer[vna_other] = (aux, name.decode())
            aux += vna_next
        offset += vn_next

    if not newer:
        return False
    if base_name is None:
        raise SystemExit(f"{path}: needs {BASE_VERSION.decode()} for the rewrite but doesn't reference it")

    dynsym = next(s for s in sections if s["type"] == SHT_DYNSYM)
    versym = next(s for s in sections if s["type"] == SHT_GNU_VERSYM)
    symstr = sections[dynsym["link"]]["offset"]
    for i in range(dynsym["size"] // dynsym["entsize"]):
        st_name, st_info, st_other, st_shndx = struct.unpack_from("<IBBH", data, dynsym["offset"] + i * 24)
        index, = struct.unpack_from("<H", data, versym["offset"] + i * 2)
        index &= 0x7FFF
        if index in newer:
            symbol = cstr(data, symstr + st_name).decode()
            if st_shndx != 0 or symbol not in provided:
                raise SystemExit(f"{path}: {symbol}@{newer[index][1]} is newer than GLIBC_{'.'.join(map(str, floor))}"
                                 f" and not in lwe_glibc_compat.c")

    for aux, _ in newer.values():
        struct.pack_into("<I", data, aux, elf_hash(BASE_VERSION))
        struct.pack_into("<I", data, aux + 8, base_name)
    with open(path, "wb") as f:
        f.write(data)
    print(path)
    return True


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    floor = version_tuple("GLIBC_" + sys.argv[1])
    provided = set(sys.argv[2].split(","))
    for path in sys.argv[3:]:
        retarget(path, floor, provided)


if __name__ == "__main__":
    main()
