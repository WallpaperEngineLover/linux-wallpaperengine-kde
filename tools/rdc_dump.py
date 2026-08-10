#!/usr/bin/env python3
"""RenderDoc capture inspector for this engine's GL puppet-compositing pipeline.

Runs inside qrenderdoc's embedded Python interpreter (not a normal python3 - the
`renderdoc`/`qrenderdoc` modules only exist there). Invoke as:

    env -u WAYLAND_DISPLAY QT_QPA_PLATFORM=xcb DISPLAY=:0 \
        RDC_CAPFILE=/path/to/capture.rdc RDC_MODE=positions RDC_EVENTID=1775 \
        qrenderdoc --python tools/rdc_dump.py

Modes (set via RDC_MODE):
  list        - print eventId/name/numIndices/outputs for every draw call in the capture.
  positions   - dump the a_Position vertex buffer for RDC_EVENTID (comma-separated list ok).
  texcoords   - dump the a_TexCoord vertex buffer for RDC_EVENTID.
  uniforms    - dump the $Globals uniform block (g_ModelViewProjectionMatrix etc) for RDC_EVENTID.
  resources   - dump the fragment shader's bound texture resourceIds/names for RDC_EVENTID,
                useful for confirming two draws are part of the same object's FBO->composite chain
                (an intermediate pass's output resourceId showing up as a later draw's input).
  screenshot  - save the final swapchain image of the whole frame (last draw call) to RDC_OUT (PNG).

Several embedded-API quirks aren't documented anywhere obvious - see the per-function comments
below (GetConstantBlock's resourceId lives under .descriptor.resource not .resourceId, resource
names need a manual resourceId->name dict from controller.GetResources() since there's no
GetResourceName(), etc).
"""
import os
import sys
import struct

import renderdoc as rd

CAPFILE = os.environ.get("RDC_CAPFILE")
MODE = os.environ.get("RDC_MODE", "list")
EVENTIDS = [int(e) for e in os.environ.get("RDC_EVENTID", "").split(",") if e.strip()]
OUT = os.environ.get("RDC_OUT")

out_lines = []


def log(s):
    print(s)
    out_lines.append(str(s))


def walk_actions(controller, fn):
    def recurse(act):
        fn(act)
        for c in act.children:
            recurse(c)
    for a in controller.GetRootActions():
        recurse(a)


def dump_vbuffer(controller, eid, attr_name):
    controller.SetFrameEvent(eid, True)
    state = controller.GetPipelineState()
    vp = state.GetViewport(0)
    vbs = state.GetVBuffers()
    attrs = state.GetVertexInputs()
    log(f"eventId={eid} viewport=({vp.x},{vp.y},{vp.width},{vp.height})")
    for attr in attrs:
        if attr.name != attr_name:
            continue
        vb = vbs[attr.vertexBuffer]
        total_offset = attr.byteOffset + vb.byteOffset
        data = controller.GetBufferData(vb.resourceId, total_offset, 0)
        comp = attr.format.compCount
        n = (len(data) // 4 // comp) * comp
        vals = struct.unpack_from(f"<{n}f", data, 0)
        log(f"  {attr_name} ({comp} floats/vertex, {n // comp} vertices): {vals}")
        if comp >= 2:
            xs = vals[0::comp]
            ys = vals[1::comp]
            log(f"  {attr_name} X range: {min(xs)} to {max(xs)} (center {(min(xs)+max(xs))/2})")
            log(f"  {attr_name} Y range: {min(ys)} to {max(ys)} (center {(min(ys)+max(ys))/2})")


def dump_uniforms(controller, eid):
    controller.SetFrameEvent(eid, True)
    state = controller.GetPipelineState()
    refl = state.GetShaderReflection(rd.ShaderStage.Vertex)
    vs_id = state.GetShader(rd.ShaderStage.Vertex)
    # GetConstantBlock's return type (BoundCBuffer) hides the actual resourceId/byteOffset/byteSize
    # under .descriptor, not as direct attributes - not obvious from the method name.
    cbuf = state.GetConstantBlock(rd.ShaderStage.Vertex, 0, 0)
    d = cbuf.descriptor
    variables = controller.GetCBufferVariableContents(
        state.GetGraphicsPipelineObject(), vs_id, rd.ShaderStage.Vertex, refl.entryPoint, 0,
        d.resource, d.byteOffset, d.byteSize)
    log(f"eventId={eid} $Globals:")
    for v in variables:
        log(f"  {v.name}: {v.value.f32v[:16]}")


def dump_resources(controller, eid, resnames):
    controller.SetFrameEvent(eid, True)
    state = controller.GetPipelineState()
    def act_info():
        found = [(None, None)]
        def fn(act):
            if act.eventId == eid:
                found[0] = (act.GetName(controller.GetStructuredFile()), list(act.outputs))
        walk_actions(controller, fn)
        return found[0]
    name, outputs = act_info()
    log(f"eventId={eid} name={name} outputs={outputs}")
    for ro in state.GetReadOnlyResources(rd.ShaderStage.Fragment):
        rid = ro.descriptor.resource
        log(f"  fragment slot={ro.access.index} resourceId={rid} name={resnames.get(rid, '?')}")


def do_list(controller):
    def fn(act):
        if act.flags & rd.ActionFlags.Drawcall:
            log(f"eventId={act.eventId} name={act.GetName(controller.GetStructuredFile())} "
                f"numIndices={act.numIndices} outputs={list(act.outputs)}")
    walk_actions(controller, fn)


def do_screenshot(controller):
    last_draw = [None]
    def fn(act):
        if act.flags & rd.ActionFlags.Drawcall:
            last_draw[0] = act
    walk_actions(controller, fn)
    if last_draw[0] is None:
        log("no draw calls found")
        return
    controller.SetFrameEvent(last_draw[0].eventId, True)
    log(f"last draw eventId={last_draw[0].eventId}")
    texsave = rd.TextureSave()
    # the output resourceId lives on the *action*, not the controller - there's no
    # controller.GetOutputTargets() despite PipeState having one for the currently bound FBO.
    texsave.resourceId = last_draw[0].outputs[0]
    if texsave.resourceId == rd.ResourceId.Null():
        log("no output resource on last draw")
        return
    texsave.destType = rd.FileType.PNG
    texsave.mip = 0
    texsave.slice.sliceIndex = 0
    ok = controller.SaveTexture(texsave, OUT or "screenshot.png")
    log(f"SaveTexture -> {OUT or 'screenshot.png'} ok={ok}")


def sampleCode(controller):
    if MODE == "list":
        do_list(controller)
    elif MODE == "positions":
        for eid in EVENTIDS:
            dump_vbuffer(controller, eid, "a_Position")
    elif MODE == "texcoords":
        for eid in EVENTIDS:
            dump_vbuffer(controller, eid, "a_TexCoord")
    elif MODE == "uniforms":
        for eid in EVENTIDS:
            dump_uniforms(controller, eid)
    elif MODE == "resources":
        resnames = {r.resourceId: r.name for r in controller.GetResources()}
        for eid in EVENTIDS:
            dump_resources(controller, eid, resnames)
    elif MODE == "screenshot":
        do_screenshot(controller)
    else:
        log(f"unknown RDC_MODE={MODE!r}")


def main():
    if not CAPFILE:
        log("RDC_CAPFILE env var not set")
        return
    import time
    pyrenderdoc.LoadCapture(CAPFILE, rd.ReplayOptions(), CAPFILE, False, True)
    for _ in range(200):
        if pyrenderdoc.IsCaptureLoaded():
            break
        time.sleep(0.1)
    log(f"IsCaptureLoaded: {pyrenderdoc.IsCaptureLoaded()}")
    pyrenderdoc.Replay().BlockInvoke(sampleCode)


try:
    main()
except Exception as e:
    import traceback
    log(f"EXCEPTION: {e}")
    log(traceback.format_exc())

log_path = OUT + ".log" if (OUT and MODE == "screenshot") else OUT
if log_path:
    with open(log_path, "w") as f:
        f.write("\n".join(out_lines))

os._exit(0)
