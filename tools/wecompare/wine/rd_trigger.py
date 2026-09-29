# qrenderdoc --python rd_trigger.py: one-frame capture of the RenderDoc-hooked process RD_PID, path written to RD_TRIGGER_OUT
import os, time, renderdoc as rd
log = open(os.environ['RD_TRIGGER_OUT'], 'w', buffering=1)
pid = int(os.environ['RD_PID'])
ident = 0
while True:
    ident = rd.EnumerateRemoteTargets('localhost', ident)
    if ident == 0:
        break
    tc = rd.CreateTargetControl('localhost', ident, 'we_live', True)
    if tc is None:
        continue
    if tc.GetPID() != pid:
        tc.Shutdown()
        continue
    tc.TriggerCapture(1)
    deadline = time.time() + 40
    while time.time() < deadline:
        msg = tc.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            log.write(msg.newCapture.path + '\n')
            break
        if msg.type == rd.TargetControlMessageType.Disconnected:
            log.write('disconnected\n')
            break
    tc.Shutdown()
    break
log.write('done\n')
os._exit(0)
