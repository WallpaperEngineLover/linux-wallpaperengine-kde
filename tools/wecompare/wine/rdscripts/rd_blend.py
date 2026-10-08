# every draw with its outputs, fragment textures and the first colour target's blend state
# CAP=<rdc> OUT=<txt> QT_QPA_PLATFORM=xcb DISPLAY=:98 qrenderdoc --python rd_blend.py
import os, renderdoc as rd
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
texs={t.resourceId:t for t in ctl.GetTextures()}
def desc(r):
    t=texs.get(r)
    return '%d(%dx%d)'%(int(r),t.width,t.height) if t else str(int(r))
def walk(a):
    for c in a:
        if c.flags & rd.ActionFlags.Drawcall:
            ctl.SetFrameEvent(c.eventId,True)
            s=ctl.GetPipelineState()
            b=s.GetColorBlends()
            bs=[]
            for x in b[:1]:
                bs.append('en=%d c=%s/%s/%s a=%s/%s/%s mask=%x'%(x.enabled,x.colorBlend.source,x.colorBlend.destination,x.colorBlend.operation,x.alphaBlend.source,x.alphaBlend.destination,x.alphaBlend.operation,x.writeMask))
            ro=[desc(x.descriptor.resource) for x in s.GetReadOnlyResources(rd.ShaderStage.Fragment)]
            outs=[desc(o) for o in c.outputs if o!=rd.ResourceId.Null()]
            out.write('%d n=%d outs=%s tex=%s %s\n'%(c.eventId,c.numIndices,outs,ro,bs))
        walk(c.children)
walk(ctl.GetRootActions())
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
