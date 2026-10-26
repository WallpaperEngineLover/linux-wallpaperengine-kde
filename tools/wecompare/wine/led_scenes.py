#!/usr/bin/env python3
"""Writes the LED test scenes into <out dir>/led_<name>: solid colored rectangles on a gray background, with ledsource set on
different layers (a none, b red, c red and blue, d the background, e a composelayer over a part of the scene, f blue and red
with blue first). Run them through we_led.sh and tools/check_leds.sh and compare the two 80 x 10 frames."""
import json, os, shutil, sys

SOURCE = os.path.expanduser('~/.local/share/we_live/tests/ptfx/w1/project.json')
RECTS = [('red', 300, 540, 400, 1080, '1 0 0'), ('green', 960, 300, 500, 300, '0 1 0'),
         ('blue', 1500, 800, 500, 400, '0 0 1'), ('yellow', 960, 900, 200, 100, '1 1 0')]
SWAPPED = [RECTS[2], RECTS[0], RECTS[1], RECTS[3]]


def write(out, led, rects, extra=()):
    os.makedirs(out, exist_ok=True)
    shutil.copy(SOURCE, out + '/project.json')
    objects = [{'id': 1, 'name': 'bg', 'image': 'models/util/solidlayer.json', 'size': '1920 1080', 'origin': '960 540 0',
                'color': '0.2 0.2 0.2', 'alpha': 1.0, 'visible': True, 'ledsource': 1 in led}]

    for i, (name, x, y, w, h, color) in enumerate(rects):
        objects.append({'id': 10 + i, 'name': name, 'image': 'models/util/solidlayer.json', 'size': '%d %d' % (w, h),
                        'origin': '%d %d 0' % (x, y), 'color': color, 'alpha': 1.0, 'visible': True,
                        'ledsource': (10 + i) in led})

    objects.extend(extra)
    scene = {'version': 3, 'camera': {'center': '0 0 -1', 'eye': '0 0 0', 'up': '0 1 0'},
             'general': {'orthogonalprojection': {'width': 1920, 'height': 1080}, 'clearcolor': '0 0 0', 'bloom': False,
                         'cameraparallax': False, 'ambientcolor': '0.2 0.2 0.2', 'skylightcolor': '0.3 0.3 0.3'},
             'objects': objects}
    json.dump(scene, open(out + '/scene.json', 'w'), indent=1)


out = sys.argv[1]
write(out + '/led_a', set(), RECTS)
write(out + '/led_b', {10}, RECTS)
write(out + '/led_c', {10, 12}, RECTS)
write(out + '/led_d', {1}, RECTS)
write(out + '/led_e', set(), RECTS, [{'id': 50, 'name': 'cmp', 'image': 'models/util/composelayer.json', 'size': '800 400',
                                      'origin': '700 540 0', 'visible': True, 'copybackground': True, 'ledsource': True}])
write(out + '/led_f', {10, 11}, SWAPPED)
