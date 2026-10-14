# Arguments of WE's TransferVideoFrame call (wallpaper64.exe 2.8.42 sub_1400F3480 at 0x1400f353f) on a live WE: the
# destination texture, source rect, destination rect and the player's video info (+0x68 w, +0x6c h, +0x70 fps).
#     OUT=transfer.txt COUNT=10 gdb -q -batch -p <wallpaper64.exe pid> -x gdb_video_transfer.py
import gdb, os, struct
gdb.execute('set pagination off')
gdb.execute('set confirm off')
out = open(os.environ.get('OUT', 'video_transfer.txt'), 'w')
inf = gdb.selected_inferior()
count = [0]
def reg(n):
    return int(gdb.selected_frame().read_register(n)) & 0xffffffffffffffff
def mem(addr, n):
    return bytes(inf.read_memory(addr, n))
class B(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*0x1400f353f', internal=False)
    def stop(self):
        rbx, rdx, r8, r9 = reg('rbx'), reg('rdx'), reg('r8'), reg('r9')
        src = struct.unpack('4f', mem(r8, 16))
        dst = struct.unpack('4i', mem(r9, 16))
        w, h, fps = struct.unpack('iif', mem(rbx + 0x68, 12))
        target = struct.unpack('Q', mem(rbx + 0x88, 8))[0]
        out.write('player %#x dst tex %#x src %s dst %s info %dx%d fps %.3f target %#x\n'
                  % (rbx, rdx, src, dst, w, h, fps, target))
        out.flush()
        count[0] += 1
        if count[0] >= int(os.environ.get('COUNT', 10)):
            gdb.execute('delete')
            out.close()
            gdb.post_event(lambda: gdb.execute('detach'))
        return False
B()
gdb.execute('continue')
