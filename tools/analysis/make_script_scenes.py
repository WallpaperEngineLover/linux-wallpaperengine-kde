#!/usr/bin/env python3
"""Scenes for the script engine's argument checks and error paths, for the coverage runs (coverage_scripts.sh).

fuzz     a script walks every function and property of the layers, thisScene, engine, input and the other script
         objects and calls or assigns them with odd arguments inside try/catch, then creates and destroys layers
errors   scripts that fail to compile, throw in init/update/destroy, return the wrong type, write odd values
runaway  an update that never returns (needs LWE_SCRIPT_TIMEOUT_MS)
heap     an update that allocates until the heap limit (needs LWE_SCRIPT_HEAP_MB)

Usage: make_script_scenes.py <out dir> [assets dir]"""
import json, os, shutil, sys

out = sys.argv[1]
A = sys.argv[2] if len(sys.argv) > 2 else '/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets'


def layer(id, name, origin, size, color, **extra):
    o = {"id": id, "name": name, "image": "models/util/solidlayer.json", "size": size, "origin": origin,
         "color": color, "alpha": 1.0, "visible": True}
    o.update(extra)
    return o


def text(id, name, value, **extra):
    o = {"id": id, "name": name, "origin": "400 300 0", "scale": "1 1 1", "angles": "0 0 0", "visible": True,
         "text": value, "font": "fonts/mono.ttf", "pointsize": 20.0, "color": "1 1 1", "alpha": 1.0,
         "horizontalalign": "left", "verticalalign": "top", "padding": 4, "size": "300 60"}
    o.update(extra)
    return o


def write(name, objects, scripts=()):
    folder = os.path.join(out, name)
    os.makedirs(folder + '/fonts', exist_ok=True)
    shutil.copy(A + '/fonts/RobotoMono-Regular.ttf', folder + '/fonts/mono.ttf')
    json.dump({"file": "scene.json", "type": "scene", "title": name, "general": {"properties": {}}},
              open(folder + '/project.json', 'w'))
    json.dump({"version": 3, "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
               "general": {"orthogonalprojection": {"width": 1920, "height": 1080}, "clearcolor": "0 0 0",
                           "bloom": False, "cameraparallax": False},
               "objects": objects}, open(folder + '/scene.json', 'w'), indent=1)


FUZZ = r"""
'use strict';
const ARGS = [[], [0], [1, 2, 3], ['x'], [null], [undefined], [{}], [-1], [1e9], [NaN], ['x', 'y'], [true],
              [new Vec3(1, 2, 3)], [new Vec2(1, 2)], [[1, 2, 3]], [{ x: 1, y: 2, z: 3 }], [0, 0, 0, 0], [function () {}]];
const VALUES = [0, 1.5, -3, 'x', '1 2 3', true, false, null, undefined, NaN, Infinity, { x: 1, y: 2, z: 3 }, [1, 2, 3],
                new Vec3(1, 2, 3), new Vec2(1, 2), new Vec4(1, 2, 3, 4), { r: 1, g: 0, b: 0 }, function () {}];
const SKIP = /^(destroyLayer|destroy|openUserShortcut|setInterval|setTimeout)$/;
let frame = 0;
let nested = 0;

function names(object) {
  const found = new Set();
  for (let o = object; o && o !== Object.prototype; o = Object.getPrototypeOf(o)) {
    for (const n of Object.getOwnPropertyNames(o)) found.add(n);
  }
  for (const n in object) found.add(n);
  return [...found];
}

function poke(object, label, depth = 0) {
  if (!object || (typeof object !== 'object' && typeof object !== 'function') || depth > 1 || (depth > 0 && ++nested > 150)) return;
  for (const n of names(object)) {
    let member;
    try { member = object[n]; } catch (e) { continue; }
    if (typeof member === 'function') {
      if (SKIP.test(n)) continue;
      for (const args of ARGS) {
        try {
          const result = member.apply(object, args);
          if (result && typeof result === 'object' && result !== object) poke(result, label + '.' + n, depth + 1);
        } catch (e) { }
      }
      try { new member(); } catch (e) { }
    } else {
      for (const v of VALUES) {
        try { object[n] = v; } catch (e) { }
      }
      try { void JSON.stringify(object[n]); } catch (e) { }
    }
  }
}

export function update(value) {
  frame++;
  if (frame === 3) {
    const count = thisScene.getLayerCount ? thisScene.getLayerCount() : 0;
    for (let i = 0; i < count + 2; i++) {
      let l;
      try { l = thisScene.getLayerByIndex ? thisScene.getLayerByIndex(i) : null; } catch (e) { }
      if (!l) { try { l = thisScene.enumerateLayers()[i]; } catch (e) { } }
      if (l) poke(l, 'layer' + i);
    }
    for (const name of ['img', 'label', 'glow', 'spark', 'missing']) {
      let l; try { l = thisScene.getLayer(name); } catch (e) { }
      if (l) poke(l, name);
    }
    poke(thisScene, 'scene');
    poke(engine, 'engine');
    poke(input, 'input');
    poke(shared, 'shared');
    poke(console, 'console');
    try { poke(localStorage, 'localStorage'); } catch (e) { }
    poke(thisLayer, 'thisLayer');
  }
  if (frame === 4) {
    const configs = [
      '{"image":"models/util/solidlayer.json","name":"made1","origin":"100 100 0","size":"50 50"}',
      '{"text":"made","name":"made2","font":"fonts/mono.ttf","pointsize":12}',
      '{"name":"made3","particle":{"emitter":[{"name":"boxrandom","rate":5}]}}',
      '{"name":"made4","light":"point","radius":50}',
      '{"image":5}', '{', '', 'null', '[]',
    ];
    const made = [];
    for (const c of configs) {
      try { made.push(thisScene.createLayer(c)); } catch (e) { }
      try { made.push(thisScene.createLayer(JSON.parse(c))); } catch (e) { }
    }
    for (const m of made) { if (m) { try { poke(m, 'made'); } catch (e) { } } }
    try { thisScene.sortLayer(made[0], 0); thisScene.sortLayer(made[1], 99); thisScene.sortLayer('x', 1); } catch (e) { }
    for (const m of made) { try { thisScene.destroyLayer(m); } catch (e) { } try { thisScene.destroyLayer(m); } catch (e) { } }
    for (const a of [null, 'img', 'nothing', 5, -1, {}, undefined]) { try { thisScene.destroyLayer(a); } catch (e) { } }
    try { thisScene.destroyLayer(thisScene.getLayer('victim')); } catch (e) { }
  }
  if (frame === 5) {
    try { setTimeout(() => {}, 10); setInterval(() => { throw new Error('tick'); }, 20); } catch (e) { }
    try { clearTimeout(9999); clearTimeout('x'); } catch (e) { }
    try { engine.registerAudioBuffers(16); engine.registerAudioBuffers(64); engine.registerAudioBuffers(0); engine.registerAudioBuffers('x'); } catch (e) { }
  }
  return value;
}
"""

objects = [
    layer(1, "bg", "960 540 0", "1920 1080", "0.1 0.1 0.2"),
    layer(2, "img", "400 400 0", "200 120", "0.8 0.3 0.2"),
    layer(3, "victim", "800 400 0", "100 100", "0.2 0.8 0.3"),
    text(4, "label", "probe"),
    {"id": 5, "name": "glow", "light": "point", "origin": "600 600 0", "radius": 200, "intensity": 1.0,
     "color": "1 1 1", "visible": True},
    {"id": 6, "name": "spark", "origin": "1000 600 0", "visible": True,
     "particle": {"maxcount": 20, "emitter": [{"name": "boxrandom", "rate": 20, "distancemax": "50 50 0"}],
                  "initializer": [{"name": "lifetimerandom", "min": 1, "max": 2}],
                  "operator": [{"name": "movement"}]}},
    {"id": 7, "name": "probe", "origin": "0 0 0", "visible": {"script": FUZZ, "value": True}},
]
write('fuzz', objects)

ERRORS = {
    'syntax': "export function update(v) { return v +; }",
    'throws': "export function update(v) { throw new Error('boom'); }",
    'init': "export function init() { throw new Error('init'); }\nexport function update(v) { return v; }",
    'destroy': "export function update(v) { return v; }\nexport function destroy() { throw new Error('destroy'); }",
    'string': "export function update(v) { return 'not a number'; }",
    'nan': "export function update(v) { return NaN; }",
    'object': "export function update(v) { return { a: 1 }; }",
    'undef': "export function update(v) { }",
    'vec': "export function update(v) { return new Vec3(1, 'a', null); }",
    'missing': "export var x = 1;",
    'cursor': "export function cursorDown(e) { throw new Error('d'); }\nexport function cursorUp(e) { return 1; }\n"
              "export function cursorClick(e) { }\nexport function cursorEnter() { throw 1; }\nexport function cursorLeave() { }\n"
              "export function update(v) { return v; }",
    'props': "export var scriptProperties = createScriptProperties().addSlider({name:'a',value:1,min:0,max:2}).addCheckbox({name:'b',value:true}).addColor({name:'c',value:'1 0 0'}).addText({name:'d',value:'x'}).addCombo({name:'e',options:[{label:'A',value:'1'}],value:'1'}).addFile({name:'f'}).addDirectory({name:'g'}).addSceneTexture({name:'h'}).finish();\n"
             "export function update(v) { return scriptProperties.a; }",
    'import': "import * as WEMath from 'WEMath';\nimport * as WEColor from 'WEColor';\nimport * as WEVector from 'WEVector';\nimport * as Nope from 'Nothing';\n"
              "export function update(v) { return WEMath.mix(0, 1, 0.5) + WEMath.smoothStep(0, 1, 0.3) + WEColor.rgb2hsv(new Vec3(1, 0, 0)).x; }",
    'text': None,
}
objects = [layer(1, "bg", "960 540 0", "1920 1080", "0.1 0.2 0.1")]
oid = 2
for name, source in ERRORS.items():
    if source is None:
        continue
    for prop, value in (("alpha", 1.0), ("origin", "100 100 0"), ("angles", "0 0 0"), ("scale", "1 1 1"),
                        ("visible", True), ("color", "1 1 1")):
        if name == 'cursor' and prop != 'alpha':
            continue
        item = layer(oid, "e%d" % oid, "%d %d 0" % (100 + oid * 20, 300), "40 40", "1 1 1")
        item[prop] = {"script": source, "value": value}
        objects.append(item)
        oid += 1
for index, source in enumerate((ERRORS['syntax'], ERRORS['throws'], ERRORS['string'], ERRORS['init'], ERRORS['props'],
                                "export function update(v) { return v + ' ' + engine.frametime.toFixed(2) + ' ' + new Date().getFullYear(); }",
                                "export function update(v) { return 42; }", "export function update(v) { return null; }",
                                "export function update(v) { return undefined; }")):
    objects.append(text(oid, "t%d" % oid, {"script": source, "value": "text"}, origin="100 %d 0" % (100 + index * 60)))
    oid += 1
write('errors', objects)

loop1 = layer(2, "loop", "400 400 0", "100 100", "1 1 1")
loop1["alpha"] = {"script": "export function update(v) { while (true) { } }", "value": 1.0}
loop2 = layer(3, "loop2", "600 400 0", "100 100", "1 1 1")
loop2["origin"] = {"script": "export function update(v) { for (;;) { Math.random(); } }", "value": "600 400 0"}
write('runaway', [layer(1, "bg", "960 540 0", "1920 1080", "0.3 0.1 0.1"), loop1, loop2,
                  text(4, "loop3", {"script": "export function update(v) { while (true) { } }", "value": "x"})])

hog = layer(2, "hog", "400 400 0", "100 100", "1 1 1")
hog["alpha"] = {"script": "const keep = [];\nexport function update(v) { for (;;) { keep.push(new Array(1 << 20).fill(1.5)); } }",
                "value": 1.0}
write('heap', [layer(1, "bg", "960 540 0", "1920 1080", "0.1 0.1 0.3"), hog])

print('ok')
