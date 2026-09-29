import os, struct, renderdoc as rd
# raw float dump of every constant buffer bound to VS/GS/PS for EIDS (names are stripped in WE's DXBC)
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
for eid in [int(x) for x in os.environ['EIDS'].split(',')]:
    ctl.SetFrameEvent(eid,True)
    s=ctl.GetPipelineState()
    out.write('== %d\n'%eid)
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Geometry, rd.ShaderStage.Pixel):
        refl=s.GetShaderReflection(stage)
        if refl is None: continue
        for i,cb in enumerate(refl.constantBlocks):
            d=s.GetConstantBlock(stage,i,0).descriptor
            if d.resource==rd.ResourceId.Null(): continue
            data=ctl.GetBufferData(d.resource,d.byteOffset,min(d.byteSize,1024))
            fl=struct.unpack('<%df'%(len(data)//4),data[:len(data)//4*4])
            out.write(' %s cb%d %s size=%d\n'%(stage,i,cb.name,d.byteSize))
            for r in range(0,len(fl),4): out.write('   [%2d] %s\n'%(r//4,' '.join('%.6g'%x for x in fl[r:r+4])))
        if os.environ.get('DIS') and stage==getattr(rd.ShaderStage,os.environ.get('DIS')):
            t=ctl.DisassembleShader(s.GetGraphicsPipelineObject(),refl,'')
            out.write(t[:6000]+'\n')
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
