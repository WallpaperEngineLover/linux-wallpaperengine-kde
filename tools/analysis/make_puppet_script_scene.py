#!/usr/bin/env python3
"""A copy of a puppet scene wallpaper whose puppet layer runs a script that calls the animation layer and bone API
(getAnimationLayer*, createAnimationLayer, playSingleAnimation, destroyAnimationLayer, bone transforms, physics
impulses), for the coverage runs (coverage_scripts.sh). The wallpaper is not modified on disk.

Usage: make_puppet_script_scene.py <workshop item dir> <object id of the puppet layer> <out dir> [animation ids, e.g. 105,107]

The animation ids are the clips the script lays over the puppet as lasting additive and normal layers."""
import json, os, shutil, subprocess, sys

item, object_id, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
animations = [int(x) for x in sys.argv[4].split(',')] if len(sys.argv) > 4 else [105, 107, 112]
here = os.path.dirname(os.path.abspath(__file__))

SCRIPT = r"""
'use strict';
let frame = 0;
let ended = 0;
const ANIMATIONS = __ANIMATIONS__;
const mats = [new Mat4(), new Mat4().translate(new Vec3(5, 3, 0)), new Mat4().scale(new Vec3(1.1, 0.9, 1))];

export function cursorEnter(e) { ended += 1; }
export function cursorLeave(e) { ended += 1; }
export function cursorDown(e) { ended += 1; }
export function cursorUp(e) { ended += 1; }
export function cursorClick(e) { ended += 1; }

export function update(value) {
  frame++;
  const t = (f) => { try { return f(); } catch (e) { return undefined; } };
  if (frame === 3) {
    const count = thisLayer.getAnimationLayerCount();
    for (let i = -1; i <= count; i++) {
      const l = t(() => thisLayer.getAnimationLayer(i));
      if (l) { t(() => l.name); t(() => l.rate = 0.7); t(() => l.blend = 0.4); t(() => l.visible = false); t(() => l.visible = true);
               t(() => l.getAnimation()); t(() => l.play()); t(() => l.pause()); t(() => l.stop()); }
    }
    for (const n of ['nope', '', 'kopf', 'move']) t(() => thisLayer.getAnimationLayer(n));
    const first = t(() => thisLayer.getAnimationLayer(0));
    if (first) {
      t(() => first.addEndedCallback(() => { ended++; }));
      t(() => first.addEndedCallback());
      t(() => first.addEndedCallback(5));
      t(() => first.setFrame(2)); t(() => first.getFrame()); t(() => first.isPlaying());
      t(() => first.fps); t(() => first.frameCount); t(() => first.duration);
      t(() => first.setFrame(first.frameCount + 5));
    }
    t(() => thisLayer.getAnimationLayer());
    t(() => thisLayer.getAnimationLayer({}));
  }
  if (frame === 4) {
    const made = [];
    for (const a of [[105], ['move'], [107, { rate: 2, blend: 0.5 }], [{ animation: 112 }], [], [9999], [null],
                     [112, '{"rate":1,"blend":1,"name":"extra"}']]) {
      made.push(t(() => thisLayer.createAnimationLayer(...a)));
      made.push(t(() => thisLayer.playSingleAnimation(...a)));
    }
    for (const m of made) { if (m) { t(() => m.addEndedCallback(() => { ended++; })); t(() => m.setFrame(0)); t(() => m.rate = 8); t(() => m.rate = 1.5); t(() => m.getAnimation()); t(() => m.name); } }
    t(() => thisLayer.getAnimationLayerCount());
    for (const m of made) t(() => thisLayer.destroyAnimationLayer(m));
    t(() => thisLayer.destroyAnimationLayer(made[0]));
    t(() => thisLayer.destroyAnimationLayer('extra'));
    t(() => thisLayer.destroyAnimationLayer('nope'));
    t(() => thisLayer.destroyAnimationLayer(0));
    t(() => thisLayer.destroyAnimationLayer(-1));
    t(() => thisLayer.destroyAnimationLayer(99));
    t(() => thisLayer.destroyAnimationLayer({}));
    t(() => thisLayer.destroyAnimationLayer());
  }
  if (frame === 11) {
    // lasting layers over the puppet: additive ones and normal ones at several blends, so the pose has to weigh them
    ANIMATIONS.forEach((id, i) => {
      t(() => thisLayer.createAnimationLayer({ animation: id, additive: i % 2 === 0, blend: 0.3 + 0.2 * i, rate: 1 + i,
                                               visible: true, name: 'lasting' + i }));
    });
    t(() => thisLayer.createAnimationLayer({ animation: ANIMATIONS[0], additive: false, blend: 1, visible: true }));
  }
  if (frame >= 5 && frame <= 8) {
    const bones = thisLayer.getBoneCount();
    for (let i = -1; i <= bones; i += Math.max(1, Math.floor(bones / 12))) {
      t(() => thisLayer.getBoneParentIndex(i));
      for (const m of mats) {
        t(() => thisLayer.getBoneTransform(i));
        t(() => thisLayer.setBoneTransform(i, m));
        t(() => thisLayer.getLocalBoneTransform(i));
        t(() => thisLayer.setLocalBoneTransform(i, m));
      }
      t(() => thisLayer.getLocalBoneAngles(i));
      t(() => thisLayer.setLocalBoneAngles(i, new Vec3(0, 0, 0.2 * frame)));
      t(() => thisLayer.getLocalBoneOrigin(i));
      t(() => thisLayer.setLocalBoneOrigin(i, new Vec3(1, 2, 0)));
      t(() => thisLayer.applyBonePhysicsImpulse(i, new Vec3(30, 0, 0), new Vec3(0, 10, 0)));
      t(() => thisLayer.resetBonePhysicsSimulation(i));
    }
    for (const n of ['', 'nope', 'root']) {
      t(() => thisLayer.getBoneIndex(n));
      t(() => thisLayer.getBoneParentIndex(n));
      t(() => thisLayer.getBoneTransform(n));
      t(() => thisLayer.setLocalBoneAngles(n, new Vec3(0, 0, 1)));
      t(() => thisLayer.applyBonePhysicsImpulse(n, new Vec3(1, 0, 0), new Vec3(0, 1, 0)));
      t(() => thisLayer.resetBonePhysicsSimulation(n));
    }
    t(() => thisLayer.getBoneIndex(0));
    t(() => thisLayer.setBoneTransform(0));
    t(() => thisLayer.applyBonePhysicsImpulse(0));
  }
  if (frame >= 10 && frame <= 24) {
    const effects = thisLayer.getEffectCount();
    for (let i = 0; i < effects; i++) {
      const e = t(() => thisLayer.getEffect(i));
      if (e) { t(() => e.visible = (frame + i) % 3 !== 0); t(() => e.name = 'fx' + frame); t(() => e.name); t(() => e.name = 5); }
    }
    if (frame % 4 === 0) t(() => thisLayer.colorBlendMode = frame % 8 === 0 ? 0 : 9);
  }
  return value;
}
"""

os.makedirs(out, exist_ok=True)
subprocess.run([sys.executable, '-I', os.path.join(here, '..', 'unpack_pkg.py'), os.path.join(item, 'scene.pkg'), out],
               check=True, stdout=subprocess.DEVNULL)
shutil.copy(os.path.join(item, 'project.json'), os.path.join(out, 'project.json'))

path = os.path.join(out, 'scene.json')
scene = json.load(open(path))
for obj in scene['objects']:
    if obj.get('id') == object_id:
        obj['visible'] = {'script': SCRIPT.replace('__ANIMATIONS__', json.dumps(animations)), 'value': True}
        break
else:
    sys.exit('no object %d in %s' % (object_id, item))
json.dump(scene, open(path, 'w'))
print('ok')
