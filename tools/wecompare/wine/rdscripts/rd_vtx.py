import os, renderdoc as rd, struct
out=open(os.environ['OUT'],'w')
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
for eid in [int(x) for x in os.environ['EIDS'].split(',')]:
    ctl.SetFrameEvent(eid,True)
    post=ctl.GetPostVSData(0,0,rd.MeshDataStage.VSOut)
    data=ctl.GetBufferData(post.vertexResourceId, post.vertexByteOffset, post.vertexByteStride*post.numIndices)
    out.write('== %d n=%d stride=%d\n'%(eid,post.numIndices,post.vertexByteStride))
    for i in range(post.numIndices):
        v=struct.unpack_from('4f',data,i*post.vertexByteStride)
        out.write('  %s\n'%(v,))
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
