import os, renderdoc as rd
# raw texture data of resource RES at event EID into OUT (a D32 atlas is little endian float32 rows); env CAP, EID, RES, OUT
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
ctl.SetFrameEvent(int(os.environ['EID']),True)
rid=[t for t in ctl.GetTextures() if int(t.resourceId)==int(os.environ['RES'])][0]
data=ctl.GetTextureData(rid.resourceId, rd.Subresource(0,0,0))
open(os.environ['OUT'],'wb').write(bytes(data))
open(os.environ['OUT']+'.txt','w').write('%d %d %s %d\n'%(rid.width,rid.height,rid.format.Name(),len(data)))
ctl.Shutdown(); cap.Shutdown(); os._exit(0)
