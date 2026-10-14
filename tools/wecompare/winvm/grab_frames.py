#!/usr/bin/env python3
# grab_frames.py <qmp socket> <out.txt> <seconds> <x0 y0 w h> - screendumps of the VM as fast as QEMU makes them,
# each decoded to the frame number of the videotex kit's test video (7 blocks of the 640x360 source, decode.py
# layout) inside the screen rectangle the 1920x1080 scene covers. Lines are "<seconds> <frame>", -1 = no number
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp import QMP

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(parts[4][:w * h * 3], np.uint8).reshape(h, w, 3).mean(2)

sock, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
x0, y0, w, h = map(float, sys.argv[4:8])
pts = [(int(x0 + (20 + b * 88 + 36) * w / 640), int(y0 + 80 * h / 360)) for b in range(7)]
q = QMP(sock)
tmp = '/dev/shm/winvm_grab_%d.ppm' % os.getpid()
res = []
t0 = time.time()
while time.time() - t0 < secs:
    q.cmd('screendump', filename=tmp)
    t = time.time() - t0
    a = read_ppm(tmp)
    bits = 0
    for px, py in pts:
        v = a[py - 3:py + 4, px - 3:px + 4].mean()
        bits = -1 if (bits < 0 or 60 < v < 190) else bits * 2 + (v >= 190)
    res.append((t, bits))
os.unlink(tmp)
open(out, 'w').write(''.join('%.4f %d\n' % r for r in res))
