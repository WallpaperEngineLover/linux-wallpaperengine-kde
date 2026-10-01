import os, traceback, renderdoc as rd
# depth-only draws (shadow passes): rasterizer, depth state and viewports; env CAP, OUT
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
texs={t.resourceId:t for t in ctl.GetTextures()}
draws=[]
def walk(a):
    for c in a:
        if c.flags & rd.ActionFlags.Drawcall: draws.append(c)
        walk(c.children)
walk(ctl.GetRootActions())
def fields(o):
    return {k: str(getattr(o, k)) for k in dir(o) if not k.startswith('_') and not callable(getattr(o, k))}
try:
    n = 0
    for c in draws:
        ctl.SetFrameEvent(c.eventId, True)
        vk = ctl.GetVulkanPipelineState()
        p = ctl.GetPipelineState()
        depth = p.GetDepthTarget()
        outs = [o for o in p.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        if depth.resource == rd.ResourceId.Null() or outs:
            continue
        t = texs.get(depth.resource)
        out.write('eid %d idx %d inst %d depth %s %dx%d fmt %s\n' % (c.eventId, c.numIndices, c.numInstances, depth.resource, t.width, t.height, t.format.Name()))
        out.write('  rast %s\n' % fields(vk.rasterizer))
        out.write('  ds %s\n' % fields(vk.depthStencil))
        out.write('  vps %s\n' % [(v.vp.x, v.vp.y, v.vp.width, v.vp.height) for v in vk.viewportScissor.viewportScissors][:6])
        n += 1
        if n > 8:
            break
except Exception:
    out.write(traceback.format_exc())
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
