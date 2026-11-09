#!/usr/bin/env python3
"""Makes stripped copies of a scene wallpaper for comparing the two engines step by step.

    strip_variants.py <id> [out dir]       (LWE_WORKSHOP_DIR as for the other tools)

Writes folders <out>/<id>_base, <id>_fx, <id>_fxp and <id>_full that both engines can load as plain scene folders:
  base  image layers only: no effects, no particle systems, no text
  fx    base with the layers' effects
  fxp   fx with the particle systems
  full  fx and particles and text (the scene as shipped, unpacked)
Capture each with render_we.py <folder> and score it with align_scan.py <folder>; the step at which the difference
appears says which kind of layer it comes from. Presets (no scene.pkg) are skipped.
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKSHOP = Path(__import__('os').environ.get('LWE_WORKSHOP_DIR', '/workspace/SteamLibrary/steamapps/workshop/content/431960'))


def is_text(obj):
    return 'text' in obj and 'image' not in obj and 'particle' not in obj


def build(item, variant, out):
    source = WORKSHOP / item
    folder = out / f'{item}_{variant}'
    folder.mkdir(parents = True, exist_ok = True)
    subprocess.run([sys.executable, str(HERE.parent / 'unpack_pkg.py'), str(source / 'scene.pkg'), str(folder)],
                   check = True, stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL)
    scene = json.loads((folder / 'scene.json').read_text())

    kept = []
    for obj in scene['objects']:
        if variant != 'full' and is_text(obj):
            continue
        if variant in ('base', 'fx') and 'particle' in obj:
            continue
        if variant == 'base' and 'effects' in obj:
            obj['effects'] = []
        kept.append(obj)

    scene['objects'] = kept
    (folder / 'scene.json').write_text(json.dumps(scene))
    project = json.loads((source / 'project.json').read_text(encoding = 'utf-8-sig'))
    project['file'] = 'scene.json'
    (folder / 'project.json').write_text(json.dumps(project, ensure_ascii = False))
    return folder


def main():
    item = sys.argv[1]
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path.home() / '.local/share/we_live/tests/strip'

    if not (WORKSHOP / item / 'scene.pkg').exists():
        print(item, 'has no scene.pkg')
        return

    for variant in ('base', 'fx', 'fxp', 'full'):
        print(build(item, variant, out))


if __name__ == '__main__':
    main()
