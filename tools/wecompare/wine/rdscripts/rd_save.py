import os, renderdoc as rd
cap=rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'],'',None)
st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
for spec in os.environ['SAVE'].split(','):
    eid,res,mip,out=spec.split(':')
    ctl.SetFrameEvent(int(eid),True)
    rid=[t.resourceId for t in ctl.GetTextures() if int(t.resourceId)==int(res)][0]
    s=rd.TextureSave(); s.resourceId=rid; s.mip=int(mip); s.destType=rd.FileType.PNG; s.alpha=rd.AlphaMapping.Preserve
    ctl.SaveTexture(s,out)
ctl.Shutdown(); cap.Shutdown(); os._exit(0)
