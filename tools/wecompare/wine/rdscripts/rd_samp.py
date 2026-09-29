import os, renderdoc as rd
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
for eid in [int(x) for x in os.environ['EIDS'].split(',')]:
    ctl.SetFrameEvent(eid,True)
    s=ctl.GetPipelineState()
    for sm in s.GetSamplers(rd.ShaderStage.Fragment):
        d=sm.sampler
        out.write('%d slot=%d filter=%s/%s/%s addr=%s %s lod=[%s,%s] bias=%s aniso=%s\n'%(eid,sm.access.index,d.filter.minify,d.filter.magnify,d.filter.mip,d.addressU,d.addressV,d.minLOD,d.maxLOD,d.mipBias,d.maxAnisotropy))
    for ro in s.GetReadOnlyResources(rd.ShaderStage.Fragment):
        dd=ro.descriptor
        out.write('   tex slot=%d res=%s firstMip=%d numMips=%d\n'%(ro.access.index,dd.resource,dd.firstMip,dd.numMips))
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
