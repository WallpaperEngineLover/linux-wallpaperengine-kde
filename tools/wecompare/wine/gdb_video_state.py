# State of WE's video texture player on a live WE (wallpaper64.exe 2.8.42): the player thread's play gate
# (sub_1400F3930 at 0x1400f3a34: +0x90 play requested, +0x60 frame counter of the last transfer vs the scene counter,
# +0x91 engine playing, +0x92 dirty), the OnVideoStreamTick result (0x1400f3aeb), every entry of the transfer
# (sub_1400F3480 at 0x1400f3496, +0xc9 needs transfer) and the TransferVideoFrame arguments (0x1400f353f).
# Only lines that differ from the previous one of the same kind are written.
#     OUT=state.txt SECONDS=20 gdb -q -batch -p <wallpaper64.exe pid> -x gdb_video_state.py
import gdb, os, struct, time
gdb.execute('set pagination off')
gdb.execute('set confirm off')
out = open(os.environ.get('OUT', 'video_state.txt'), 'w')
inf = gdb.selected_inferior()
end = time.time() + float(os.environ.get('SECONDS', 20))
last = {}
def reg(n):
    return int(gdb.selected_frame().read_register(n)) & 0xffffffffffffffff
def mem(addr, fmt):
    return struct.unpack(fmt, bytes(inf.read_memory(addr, struct.calcsize(fmt))))
def player(rbx):
    counter = mem(mem(rbx + 0x50, 'Q')[0] + 0x144, 'I')[0]
    req, playing, dirty = mem(rbx + 0x90, 'BBB')
    w, h, fps = mem(rbx + 0x68, 'iif')
    return 'player %#x req %d playing %d dirty %d last %d counter %d needs %d info %dx%d %.2f' % (
        rbx, req, playing, dirty, mem(rbx + 0x60, 'I')[0], counter, mem(rbx + 0xc9, 'B')[0], w, h, fps)
def log(kind, text):
    if last.get(kind) != text:
        last[kind] = text
        out.write('%.3f %s %s\n' % (time.time(), kind, text))
        out.flush()
    if time.time() > end:
        gdb.execute('delete')
        out.close()
        gdb.post_event(lambda: gdb.execute('detach'))
class B(gdb.Breakpoint):
    def __init__(self, addr, kind, fn):
        super().__init__('*' + hex(addr), internal=False)
        self.kind, self.fn = kind, fn
    def stop(self):
        log(self.kind, self.fn())
        return False
B(0x1400f3a34, 'gate', lambda: 'want %d ' % (reg('rdi') & 0xff) + player(reg('rbx')))
B(0x1400f3aeb, 'tick', lambda: 'hr=0x%08x' % (reg('rax') & 0xffffffff))
B(0x1400f3496, 'bind', lambda: player(reg('rbx')))
B(0x1400f353f, 'transfer', lambda: 'dst tex %#x src %s dst %s' % (reg('rdx'), mem(reg('r8'), '4f'), mem(reg('r9'), '4i')))
gdb.execute('continue')
