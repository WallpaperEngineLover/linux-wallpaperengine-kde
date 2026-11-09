# vertex inputs of a draw: layout, the first vertices and per-attribute statistics over the non-empty ones
# CAP=<rdc> OUT=<txt> EIDS=45,51 QT_QPA_PLATFORM=xcb DISPLAY=:98 qrenderdoc --python rd_vin.py
import os, struct, renderdoc as rd
out = open(os.environ['OUT'], 'w')
cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ['CAP'], '', None)
st, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
for eid in [int(x) for x in os.environ['EIDS'].split(',')]:
    ctl.SetFrameEvent(eid, True)
    state = ctl.GetPipelineState()
    inputs = state.GetVertexInputs(); vbs = state.GetVBuffers()
    out.write('== event %d\n' % eid)
    columns = []
    for a in inputs:
        vb = vbs[a.vertexBuffer]
        n = a.format.compCount
        out.write('  %s buffer=%d offset=%d stride=%d comps=%d type=%s\n' % (a.name, a.vertexBuffer, a.byteOffset, vb.byteStride, n, a.format.compType))
        data = ctl.GetBufferData(vb.resourceId, vb.byteOffset, vb.byteSize)
        count = len(data) // max(vb.byteStride, 1)
        rows = [struct.unpack_from('%df' % n, data, i * vb.byteStride + a.byteOffset) for i in range(count)]
        columns.append((a.name, rows))
    count = min(len(r) for _, r in columns)
    out.write('  vertices %d\n' % count)
    for i in range(min(count, 6)):
        out.write('   %d %s\n' % (i, [tuple(round(x, 3) for x in r[i]) for _, r in columns]))
    for name, rows in columns:
        for c in range(len(rows[0])):
            vals = sorted(r[c] for r in rows[:count])
            nz = [v for v in vals if v != 0]
            out.write('  %s[%d] min=%.3f median=%.3f mean=%.3f max=%.3f nonzero=%d\n' % (name, c, vals[0], vals[len(vals) // 2], sum(vals) / len(vals), vals[-1], len(nz)))
ctl.Shutdown(); cap.Shutdown(); out.close(); os._exit(0)
