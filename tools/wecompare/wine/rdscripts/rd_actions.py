import os, renderdoc as rd
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
texs={t.resourceId:t for t in ctl.GetTextures()}
names={r.resourceId:r.name for r in ctl.GetResources()}
out.write('TEXTURES mips>1:\n')
for t in texs.values():
    if t.mips>1 and t.width>=256: out.write('  %s %s %dx%d mips=%d fmt=%s\n'%(t.resourceId,names.get(t.resourceId),t.width,t.height,t.mips,t.format.Name()))
def walk(a,d=0):
    for c in a:
        flags=c.flags
        outs=[str(o) for o in c.outputs if o!=rd.ResourceId.Null()]
        nm=c.GetName(ctl.GetStructuredFile())
        if not (flags & rd.ActionFlags.Drawcall) or os.environ.get('ALL'):
            out.write('%s%d %s outs=%s copySrc=%s copyDst=%s\n'%('  '*d,c.eventId,nm,outs,c.copySource,c.copyDestination))
        else:
            out.write('%s%d DRAW %s n=%d outs=%s\n'%('  '*d,c.eventId,nm,c.numIndices,outs))
        walk(c.children,d+1)
walk(ctl.GetRootActions())
ctl.Shutdown(); cap.Shutdown()
out.close(); os._exit(0)
