#!/usr/bin/env python3
# vncshot.py <vnc unix socket> <out.png> - one full frame from QEMU's VNC server. QMP's screendump has no surface with
# virtio-vga-gl + egl-headless, the VNC server gets the frames read back from the GPU.
import socket, struct, sys
from PIL import Image


def recv(s, n):
    data = b''
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            raise EOFError('VNC server closed the connection')
        data += chunk
    return data


def main():
    s = socket.socket(socket.AF_UNIX)
    s.connect(sys.argv[1])
    s.sendall(recv(s, 12))  # same protocol version back
    types = recv(s, recv(s, 1)[0])
    if 1 not in types:
        raise RuntimeError('VNC server wants authentication')
    s.sendall(b'\x01')
    if struct.unpack('>I', recv(s, 4))[0] != 0:
        raise RuntimeError('VNC security handshake failed')
    s.sendall(b'\x01')  # shared
    width, height = struct.unpack('>HH', recv(s, 4))
    recv(s, 16)
    recv(s, struct.unpack('>I', recv(s, 4))[0])
    s.sendall(struct.pack('>B3xBBBBHHHBBB3x', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
    s.sendall(struct.pack('>BBH', 2, 0, 1) + struct.pack('>i', 0))  # raw encoding only
    s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, width, height))
    image = Image.new('RGB', (width, height))
    while True:
        kind = recv(s, 1)[0]
        if kind == 0:
            recv(s, 1)
            for _ in range(struct.unpack('>H', recv(s, 2))[0]):
                x, y, w, h, encoding = struct.unpack('>HHHHi', recv(s, 12))
                if encoding == -223:  # desktop size
                    width, height = w, h
                    image = Image.new('RGB', (width, height))
                    s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, width, height))
                    continue
                if encoding != 0:
                    raise RuntimeError('unexpected encoding %d' % encoding)
                image.paste(Image.frombytes('RGB', (w, h), recv(s, w * h * 4), 'raw', 'BGRX'), (x, y))
            break
        elif kind == 2:
            continue
        elif kind == 3:
            recv(s, 3)
            recv(s, struct.unpack('>I', recv(s, 4))[0])
        elif kind == 1:
            recv(s, 3)
            recv(s, 6 * struct.unpack('>H', recv(s, 2))[0])
    image.save(sys.argv[2])


main()
