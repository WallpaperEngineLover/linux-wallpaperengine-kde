#!/usr/bin/env python3
"""A scene whose image layer's texture is a plain video file in the project folder (a texture name that is no .tex falls
back to the file itself, like a user-picked file) and whose script calls the IVideoTexture API on that layer, for the
coverage runs (coverage_scripts.sh).

Usage: make_video_texture_scene.py <out dir> <video file> [runaway]

With "runaway" the ended callback never returns (needs LWE_SCRIPT_TIMEOUT_MS), the script engine drops it."""
import json, os, shutil, sys

out = sys.argv[1]
os.makedirs(out + '/models', exist_ok=True)
os.makedirs(out + '/materials', exist_ok=True)
shutil.copy(sys.argv[2], out + '/clip.mp4')

SCRIPT = r"""
'use strict';
let frame = 0;
let ended = 0;
const t = (f) => { try { return f(); } catch (e) { return undefined; } };

export function update(value) {
  frame++;
  const v = t(() => thisLayer.getVideoTexture());
  if (!v) return value;
  if (frame === 3) {
    t(() => v.addEndedCallback(() => { ended++; }));
    t(() => v.addEndedCallback(function () { ended++; }));
    t(() => v.addEndedCallback());
    RUNAWAY
    t(() => v.addEndedCallback(7));
    t(() => v.isPlaying()); t(() => v.getCurrentTime()); t(() => v.duration);
    t(() => v.rate = 2); t(() => v.rate); t(() => v.rate = 0.5); t(() => v.rate = -1); t(() => v.rate = 'x');
    t(() => v.loop = false); t(() => v.loop); t(() => v.loop = true);
  }
  if (frame === 6) { t(() => v.pause()); t(() => v.isPlaying()); t(() => v.setCurrentTime(0.2)); t(() => v.getCurrentTime()); }
  if (frame === 9) { t(() => v.play()); t(() => v.isPlaying()); t(() => v.setCurrentTime(-3)); t(() => v.setCurrentTime('x')); t(() => v.setCurrentTime(1e9)); }
  if (frame === 12) { t(() => v.loop = false); t(() => v.stop()); t(() => v.isPlaying()); t(() => v.play()); }
  if (frame === 15) { t(() => v.loop = true); t(() => v.stop()); t(() => v.play()); t(() => v.setCurrentTime(0.1)); }
  if (frame === 20) { t(() => v.setCurrentTime(0.0)); t(() => v.pause()); t(() => v.play()); }
  return value;
}
"""

script = SCRIPT.replace("RUNAWAY", "t(() => v.addEndedCallback(() => { for (;;) { } }));" if len(sys.argv) > 3 else "")
json.dump({"file": "scene.json", "type": "scene", "title": "video texture", "general": {"properties": {}}},
    open(out + '/project.json', 'w'))
json.dump({"autosize": True, "material": "materials/vid.json"}, open(out + '/models/vid.json', 'w'))
json.dump({"passes": [{"shader": "genericimage4", "blending": "translucent", "cullmode": "nocull",
                       "depthtest": "disabled", "depthwrite": "disabled",
                       "textures": ["clip.mp4"]}]},
          open(out + '/materials/vid.json', 'w'))
json.dump({"version": 3, "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
           "general": {"orthogonalprojection": {"width": 1920, "height": 1080}, "clearcolor": "0 0 0",
                       "bloom": False, "cameraparallax": False},
           "objects": [{"id": 2, "name": "vid", "image": "models/vid.json", "origin": "960 540 0",
                        "size": "1280 720", "scale": "1 1 1", "angles": "0 0 0", "alpha": 1.0, "visible":
                        {"script": script, "value": True}}]},
          open(out + '/scene.json', 'w'), indent=1)
print('ok')
