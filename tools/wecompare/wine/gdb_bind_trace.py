# Material binds and blend state builds of a running WE under Wine, with caller, material, blending byte and device
# blend mode/flags.
#     OUT=trace.txt COUNT=400 gdb -q -batch -p <wallpaper64.exe pid> -x gdb_bind_trace.py
# IDA addresses work directly. gdb may crash on detach, WE keeps running; keep ulimit -c 0.
import gdb, os
gdb.execute('set pagination off')
gdb.execute('set confirm off')
inf = gdb.selected_inferior()
def rd(addr, n): return bytes(inf.read_memory(addr, n))
def q(addr): return int.from_bytes(rd(addr, 8), 'little')
def name(mat):
    try:
        size = q(mat + 512 + 16); cap = q(mat + 512 + 24)
        p = q(mat + 512) if cap > 15 else mat + 512
        return rd(p, min(size, 80)).decode('latin1')
    except Exception as e: return '?'
out = open(os.environ.get('OUT', 'bind_trace.txt'), 'w')
count = [0]
class B(gdb.Breakpoint):
    def __init__(self, addr, kind):
        super().__init__('*' + hex(addr), internal=False); self.kind = kind
    def stop(self):
        f = gdb.selected_frame()
        rcx = int(f.read_register('rcx')); rsp = int(f.read_register('rsp'))
        ra = q(rsp)
        if self.kind == 'bind':
            dev = q(q(rcx + 200) + 5400)
            line = 'BIND mat=%x ret=%x blend=%d name=%s dev38=%d dev40=%x' % (rcx, ra, rd(rcx + 496, 1)[0], name(rcx), rd(dev + 38, 1)[0], int.from_bytes(rd(dev + 40, 2), 'little'))
        else:
            line = 'BLEND ret=%x dev38=%d dev40=%x' % (ra, rd(rcx + 38, 1)[0], int.from_bytes(rd(rcx + 40, 2), 'little'))
        out.write(line + '\n'); out.flush()
        count[0] += 1
        if count[0] >= int(os.environ.get('COUNT', 400)):
            gdb.execute('delete'); out.close()
            gdb.post_event(lambda: gdb.execute('detach'))
        return False
B(0x140155fc0, 'bind')
B(0x140099f60, 'blend')
gdb.execute('continue')
