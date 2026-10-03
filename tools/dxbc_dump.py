#!/usr/bin/env python3
"""Disassembles the shaders Wallpaper Engine compiled and cached (shaders/blobsSM40/*.dxs in a workshop item)
or any plain DXBC file, to see what fxc made of a workshop shader's HLSL-only constructs.

A .dxs holds a small header (e.g. "SHDV0069") and then one DXBC per stage. For every DXBC this prints the
constant buffer layout from RDEF (name, byte offset, size, class/rows/cols/elements) and, with --code, a
plain listing of the SHEX/SHDR instructions (opcode names and operands, declarations by name only).

Usage:
    dxbc_dump.py <file.dxs|file.dxbc> [--code] [--grep <uniform name>]

    --grep only prints blobs whose RDEF mentions that name, handy over a whole blobsSM40 dir:
    dxbc_dump.py <item>/shaders/blobsSM40/*.dxs --grep g_AudioSpectrum64Left
"""
import re
import struct
import sys

OPCODES = (
    "add and break breakc call callc case continue continuec cut default deriv_rtx deriv_rty discard div dp2 dp3 "
    "dp4 else emit emitthencut endif endloop endswitch eq exp frc ftoi ftou ge iadd if ieq ige ilt imad imax imin "
    "imul ine ineg ishl ishr itof label ld ld_ms log loop lt mad min max customdata mov movc mul ne nop not or "
    "resinfo ret retc round_ne round_ni round_pi round_z rsq sample sample_c sample_c_lz sample_l sample_d sample_b "
    "sqrt switch sincos udiv ult uge umul umad umax umin ushr utof xor dcl_resource dcl_constantbuffer dcl_sampler "
    "dcl_index_range dcl_gs_output_topology dcl_gs_input dcl_max_output_vertex_count dcl_input dcl_input_sgv "
    "dcl_input_siv dcl_input_ps dcl_input_ps_sgv dcl_input_ps_siv dcl_output dcl_output_sgv dcl_output_siv "
    "dcl_temps dcl_indexable_temp dcl_global_flags"
).split()
OPERAND_TYPES = {0: 'r', 1: 'v', 2: 'o', 3: 'x', 4: 'l', 5: 'd', 6: 's', 7: 't', 8: 'cb', 9: 'icb', 10: 'label',
                 11: 'vPrim', 12: 'oDepth', 13: 'null'}
CUSTOMDATA = 53


def chunks(dxbc):
    count = struct.unpack_from('<I', dxbc, 28)[0]
    for i in range(count):
        offset = struct.unpack_from('<I', dxbc, 32 + 4 * i)[0]
        size = struct.unpack_from('<I', dxbc, offset + 4)[0]
        yield dxbc[offset:offset + 4], dxbc[offset + 8:offset + 8 + size]


def rdef(data):
    def cstr(offset):
        return data[offset:data.index(b'\0', offset)].decode()

    cb_count, cb_offset, _, _, target = struct.unpack_from('<IIIII', data, 0)
    var_stride = 40 if (target & 0xffff) >= 0x500 else 24
    lines = []
    for c in range(cb_count):
        name, var_count, var_offset, size, _, _ = struct.unpack_from('<IIIIII', data, cb_offset + 24 * c)
        lines.append(f'cbuffer {cstr(name)} size {size}')
        for v in range(var_count):
            vname, start, vsize, flags, type_offset, _ = struct.unpack_from('<IIIIII', data, var_offset + var_stride * v)
            cls, typ, rows, cols, elements, _ = struct.unpack_from('<HHHHHH', data, type_offset)
            # D3D_SVF_USED: the shader reads it, fxc keeps unused variables in the buffer layout
            used = 'used' if flags & 2 else 'unused'
            lines.append(f'  {cstr(vname)} offset {start} size {vsize} class {cls} type {typ} rows {rows} '
                         f'cols {cols} elements {elements} {used}')
    return lines


def disassemble(code):
    words = struct.unpack_from(f'<{len(code) // 4}I', code)
    pos = 2

    def operand():
        nonlocal pos
        token = words[pos]
        pos += 1
        components = token & 3
        selection = (token >> 2) & 3
        component_data = (token >> 4) & 0xff
        kind = (token >> 12) & 0xff
        dimension = (token >> 20) & 3
        extended = token >> 31
        while extended:
            extended = words[pos] >> 31
            pos += 1
        if kind == 4:
            count = 1 if components == 1 else 4
            values = words[pos:pos + count]
            pos += count
            return 'l(' + ','.join('%g' % struct.unpack('<f', struct.pack('<I', v))[0] for v in values) + ')'
        indices = []
        for i in range(dimension):
            representation = (token >> (22 + 3 * i)) & 7
            if representation == 0:
                indices.append(str(words[pos]))
                pos += 1
            elif representation == 2:
                indices.append(operand())
            elif representation == 3:
                immediate = words[pos]
                pos += 1
                indices.append(f'{operand()}+{immediate}')
            else:
                indices.append('?')
                pos += 1
        text = OPERAND_TYPES.get(kind, f't{kind}') + ''.join(f'[{i}]' for i in indices)
        if components == 2:
            if selection == 0:
                text += '.' + ''.join(c for bit, c in enumerate('xyzw') if component_data >> bit & 1)
            elif selection == 1:
                text += '.' + ''.join('xyzw'[(component_data >> (2 * k)) & 3] for k in range(4))
            else:
                text += '.' + 'xyzw'[component_data & 3]
        return text

    lines = []
    while pos < len(words):
        token = words[pos]
        opcode = token & 0x7ff
        length = (token >> 24) & 0x7f
        start = pos
        name = OPCODES[opcode] if opcode < len(OPCODES) else f'op{opcode}'
        if opcode == CUSTOMDATA:
            length = words[pos + 1]
        if name.startswith('dcl') or opcode == CUSTOMDATA:
            lines.append(name)
            pos = start + length
            continue
        pos += 1
        if token >> 31:
            pos += 1
        args = []
        try:
            while pos < start + length:
                args.append(operand())
        except IndexError:
            args.append('<decode error>')
        lines.append(name + ('_sat' if (token >> 13) & 1 else '') + ' ' + ', '.join(args))
        pos = start + length
    return lines


def main(argv):
    show_code = '--code' in argv
    grep = None
    if '--grep' in argv:
        grep = argv[argv.index('--grep') + 1]
        argv = [a for a in argv if a != grep]
    paths = [a for a in argv[1:] if not a.startswith('--')]
    for path in paths:
        data = open(path, 'rb').read()
        for index, match in enumerate(re.finditer(b'DXBC', data)):
            dxbc = data[match.start():]
            out = []
            for tag, body in chunks(dxbc):
                if tag == b'RDEF':
                    out += rdef(body)
                elif tag in (b'SHEX', b'SHDR'):
                    version = struct.unpack_from('<I', body, 0)[0]
                    stage = {0: 'ps', 1: 'vs', 2: 'gs'}.get(version >> 16, f'stage{version >> 16}')
                    out.insert(0, f'== {path} #{index} {stage}_{(version >> 4) & 0xf}_{version & 0xf}')
                    if show_code:
                        out += disassemble(body)
            if grep is None or any(grep in line for line in out):
                print('\n'.join(out))


if __name__ == '__main__':
    main(sys.argv)
