import os, renderdoc as rd
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
names={r.resourceId:r.name for r in ctl.GetResources()}
for eid in [int(x) for x in os.environ['EIDS'].split(',')]:
    ctl.SetFrameEvent(eid,True)
    s=ctl.GetPipelineState()
    out.write('== %d\n'%eid)
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Fragment):
        refl=s.GetShaderReflection(stage)
        if refl is None: continue
        for ro in s.GetReadOnlyResources(stage):
            out.write('  %s tex slot=%d %s\n'%(stage,ro.access.index,ro.descriptor.resource))
        for i,cb in enumerate(refl.constantBlocks):
            b=s.GetConstantBlock(stage,i,0); d=b.descriptor
            try:
                vs=ctl.GetCBufferVariableContents(s.GetGraphicsPipelineObject(), s.GetShader(stage), stage, refl.entryPoint, i, d.resource, d.byteOffset, d.byteSize)
            except Exception as e:
                out.write('   cb err %s\n'%e); continue
            def dump(v,p=''):
                if v.members:
                    for m in v.members: dump(m,p+v.name+'.')
                else:
                    out.write('   %s%s %s = %s\n'%(stage==rd.ShaderStage.Vertex and 'V:' or 'F:',p,v.name,list(v.value.f32v[:v.rows*v.columns])))
            for v in vs: dump(v)
    if os.environ.get('SRC'):
        refl=s.GetShaderReflection(rd.ShaderStage.Fragment)
        out.write('   frag entry %s\n'%refl.entryPoint)
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
