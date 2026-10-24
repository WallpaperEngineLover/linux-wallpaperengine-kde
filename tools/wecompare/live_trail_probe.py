#!/usr/bin/env python3
"""Times the ropetrail history pushes of a running real WE (Wine) from its process memory.

usage: live_trail_probe.py <trail length> [seconds to sample] [pid]

A WE particle system keeps the countdown to the next history push at +588 and the interval (length / segments) at
+592 (u16 segment count at +584, sub_1402308A0). The system is found by scanning for a float at +592 times the
segment count at +584 equal to the renderer's length, with pointers where a ropetrail object has them, then +588 is read every millisecond: each frame lowers it by that frame's dt, a push resets it to the interval.
Prints one line per detected frame: time (ms), dt (ms) or PUSH.
"""
import re
import struct
import subprocess
import sys
import time

import numpy as np

length = float(sys.argv[1])
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
pid = int(sys.argv[3]) if len(sys.argv) > 3 else int(
    subprocess.check_output(['pgrep', '-x', 'wallpaper64.exe'], text=True).split()[0])

mem = open(f'/proc/{pid}/mem', 'rb', 0)


def regions():
    for line in open(f'/proc/{pid}/maps'):
        parts = line.split()
        if 'rw' not in parts[1]:
            continue
        a, b = (int(x, 16) for x in parts[0].split('-'))
        if b - a <= 1 << 30:
            yield a, b


def read(addr, size):
    mem.seek(addr)
    return mem.read(size)


def plausible(ptr):
    return 0x10000 < ptr < 0x7FFFFFFFFFFF and ptr % 8 == 0


def scan():
    found = []
    for a, b in regions():
        try:
            data = read(a, b - a)
        except OSError:
            continue
        floats = np.frombuffer(data[: len(data) // 4 * 4], dtype='<f4')
        with np.errstate(invalid='ignore'):
            likely = np.nonzero((floats > 0.001) & (floats < 1.0))[0]
        for i in likely:
            off = int(i) * 4 - 592
            if off < 0 or off + 616 > len(data):
                continue
            segments = struct.unpack_from('<H', data, off + 584)[0]
            interval = float(floats[i])
            timer = struct.unpack_from('<f', data, off + 588)[0]
            if not (2 <= segments <= 128 and abs(segments * interval - length) < 1e-3
                    and 0.0 <= timer <= interval + 1e-4):
                continue
            pointers = [struct.unpack_from('<Q', data, off + o)[0] for o in (512, 560, 568)]
            if all(plausible(p) for p in pointers):
                found.append(a + off)
    return found


def timer_of(addr):
    data = read(addr + 588, 4)
    return struct.unpack('<f', data)[0] if len(data) == 4 else None


def lock(limit):
    """A system that is counting: freed ones leave stale copies behind, and a particle system that is destroyed
    (scene reload, all particles gone) has to be found again"""
    deadline = time.perf_counter() + limit
    while time.perf_counter() < deadline:
        for addr in scan():
            seen = set()
            for _ in range(150):
                seen.add(timer_of(addr))
                time.sleep(0.002)
            if len(seen) >= 2 and None not in seen:
                return addr
    return None


base = lock(40)
if base is None:
    sys.exit('no ropetrail system found that keeps counting')

print('system at', hex(base), file=sys.stderr)
last = None
start = time.perf_counter()
while time.perf_counter() - start < seconds:
    timer = timer_of(base)
    now = (time.perf_counter() - start) * 1000.0
    if timer is None or timer < 0 or timer > 10:
        print(f'{now:.2f} LOST', file=sys.stderr)
        base = lock(10)
        last = None
        if base is None:
            break
        continue
    if last is not None and timer != last:
        if timer > last:
            print(f'{now:.2f} PUSH {last:.5f} -> {timer:.5f}')
        else:
            print(f'{now:.2f} dt {(last - timer) * 1000:.3f}')
    last = timer
    time.sleep(0.0005)
