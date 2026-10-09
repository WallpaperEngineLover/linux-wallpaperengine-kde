# HRESULTs of WE's video texture upload (wallpaper64.exe 2.8.42 sub_1400F3480): the keyed mutex AcquireSync and the
# media engine's TransferVideoFrame into the shared texture; the frame only counts when both succeed.
#     OUT=hr.txt COUNT=40 gdb -q -batch -p <wallpaper64.exe pid> -x gdb_video_hr.py
import gdb, os
gdb.execute('set pagination off')
gdb.execute('set confirm off')
out = open(os.environ.get('OUT', 'video_hr.txt'), 'w')
count = [0]
class B(gdb.Breakpoint):
    def __init__(self, addr, kind):
        super().__init__('*' + hex(addr), internal=False); self.kind = kind
    def stop(self):
        eax = int(gdb.selected_frame().read_register('rax')) & 0xffffffff
        out.write('%s hr=0x%08x\n' % (self.kind, eax)); out.flush()
        count[0] += 1
        if count[0] >= int(os.environ.get('COUNT', 40)):
            gdb.execute('delete'); out.close()
            gdb.post_event(lambda: gdb.execute('detach'))
        return False
B(0x1400f3517, 'AcquireSync')
B(0x1400f3545, 'TransferVideoFrame')
gdb.execute('continue')
