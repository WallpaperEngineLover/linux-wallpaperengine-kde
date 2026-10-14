# D3D11_TEXTURE2D_DESC and initial data of WE's video textures (wallpaper64.exe 2.8.42 CreateTexture2D calls: .tex with
# an embedded MP4 in sub_1400EC220 at 0x1400eda09, gif/video files in sub_1400EB570) on a live WE; attach before the
# wallpaper loads. Also prints the legacy shared handle the player opens (sub_1400F2510 OpenSharedResource at
# 0x1400f260c), sharedvideo.exe --open <handle> reads that texture from a second process.
#     OUT=texdesc.txt gdb -q -batch -p <wallpaper64.exe pid> -x gdb_video_texdesc.py
import gdb, os, struct
gdb.execute('set pagination off')
gdb.execute('set confirm off')
out = open(os.environ.get('OUT', 'video_texdesc.txt'), 'w')
inf = gdb.selected_inferior()
def reg(n):
    return int(gdb.selected_frame().read_register(n)) & 0xffffffffffffffff
def mem(addr, fmt):
    return struct.unpack(fmt, bytes(inf.read_memory(addr, struct.calcsize(fmt))))
class B(gdb.Breakpoint):
    def __init__(self, addr):
        super().__init__('*' + hex(addr), internal=False)
        self.addr = addr
    def stop(self):
        rdx, r8 = reg('rdx'), reg('r8')
        out.write('at %#x\n' % self.addr)
        w, h, mips, arr, fmt, sc, sq, usage, bind, cpu, misc = mem(rdx, '11I')
        out.write('desc %dx%d mips %d array %d format %d samples %d/%d usage %d bind %#x cpu %#x misc %#x\n'
                  % (w, h, mips, arr, fmt, sc, sq, usage, bind, cpu, misc))
        for i in range(max(mips, 1)):
            if r8:
                p, pitch, slice_ = mem(r8 + 16 * i, 'QII')
                out.write('  init[%d] %#x pitch %d slice %d\n' % (i, p, pitch, slice_))
        out.flush()
        return False
class Open(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*0x1400f260c', internal=False)
    def stop(self):
        out.write('player OpenSharedResource handle %#x\n' % reg('rdx'))
        out.flush()
        return False
Open()
for a in (0x1400eda09, 0x1400eb93c, 0x1400ebb86, 0x1400ebedb):
    B(a)
out.write('attached\n')
out.flush()
gdb.execute('continue')
