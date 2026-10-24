import os, renderdoc as rd
# disassembly of the vertex and fragment shader bound at each event of EIDS (env CAP, EIDS, OUT); OUT gets <stage> sections
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
targets=ctl.GetDisassemblyTargets(True)
out.write('targets %s\n'%targets)
for eid in [int(x) for x in os.environ['EIDS'].split(',')]:
    ctl.SetFrameEvent(eid,True)
    s=ctl.GetPipelineState()
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Fragment):
        refl=s.GetShaderReflection(stage)
        if refl is None: continue
        out.write('== %d %s\n'%(eid,stage))
        out.write(ctl.DisassembleShader(s.GetGraphicsPipelineObject(), refl, targets[0]))
        out.write('\n')
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
