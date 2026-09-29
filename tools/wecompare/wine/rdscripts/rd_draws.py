import os, renderdoc as rd
# every draw with its index count, render targets and fragment textures (id WxH format)
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
texs={t.resourceId:t for t in ctl.GetTextures()}
def desc(r):
    t=texs.get(r)
    return '%s(%dx%d %s)'%(int(r),t.width,t.height,t.format.Name()) if t else str(int(r))
def walk(a):
    for c in a:
        if c.flags & rd.ActionFlags.Drawcall:
            ctl.SetFrameEvent(c.eventId,True)
            s=ctl.GetPipelineState()
            ro=[desc(x.descriptor.resource) for x in s.GetReadOnlyResources(rd.ShaderStage.Fragment)]
            outs=[desc(o) for o in c.outputs if o!=rd.ResourceId.Null()]
            out.write('%d n=%d inst=%d outs=%s tex=%s\n'%(c.eventId,c.numIndices,c.numInstances,outs,ro))
        walk(c.children)
walk(ctl.GetRootActions())
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
