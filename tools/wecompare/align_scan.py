#!/usr/bin/env python3
"""Scores ours against the real-WE frames of an item after aligning the two timelines.

    align_scan.py <out dir> [ids or scene folders...]       (REFS, LWE_WORKSHOP_DIR, LWE_ASSETS_DIR as for compare.py)

phase_scan.py samples ours at fixed offsets, which is too coarse for a scene that changes fast (WE frames a second apart
differing by 45) and cannot tell WE's own launch-to-launch offset (engine.runtime in WE includes the load time) from a
real difference. This finds the frame of ours (fixed timestep, the WE rig's 15 fps, msaa x2, pointer at WE's position)
that matches WE's first capture best, scanning coarsely and then a frame at a time around the minimum, and then scores
the other WE frames at their capture times from there, taking the best of three neighbouring frames each. Items with a
video texture also try the WE frames with red and blue exchanged (Wine draws some that way). An item that is the same in
both engines ends at a few points or less; a larger value at the aligned frame is a real difference, whatever the diff
looked like at other offsets.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'regression'))
import regress  # noqa: E402

REFS = os.environ.get('REFS', os.path.expanduser('~/.local/share/we_live/refs'))
WORKSHOP = Path(os.environ.get('LWE_WORKSHOP_DIR', '/workspace/SteamLibrary/steamapps/workshop/content/431960'))
ASSETS = Path(os.environ.get('LWE_ASSETS_DIR', '/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets'))
FPS = 15


def has_video_texture(item):
    path = Path(os.environ.get('LWE_LIBRARY_DIR', os.path.expanduser('~/.local/share/lwe-library'))) / 'features.json'
    try:
        return 'video-tex' in json.loads(path.read_text()).get(item, {}).get('features', {})
    except (OSError, ValueError):
        return False


def load_we(item):
    files = sorted(glob.glob(f'{REFS}/{item}/f*.png'))
    times = [float(re.search(r'_(\d+(?:\.\d+)?)\.png$', os.path.basename(f)).group(1)) for f in files]
    return times, [np.asarray(Image.open(f).convert('RGB'))[30:1080, 4:1920].astype(np.float32) for f in files]


class Renderer:
    def __init__(self, args, item, work):
        self.args, self.item, self.work, self.cache = args, item, work, {}

    def frame(self, number):
        if number in self.cache:
            return self.cache[number]
        ns = argparse.Namespace(binary = self.args.binary, width = 1920, height = 1058, frame = number, assets = ASSETS,
                                timeout = 120, memory_limit = 8, step = 1 / FPS, clock = 0, gpu = 'auto', jobs = 1)
        env = regress.render_env(ns, '', None)
        env.pop('LWE_FIXED_CLOCK', None)
        env.pop('LD_PRELOAD', None)
        env['TZ'] = os.environ.get('TZ', 'Europe/Berlin')
        env['LWE_HEADLESS_CURSOR'] = '0.49792,0.51796'
        shot = self.work / f'{number}.png'
        status, _, _ = regress.render_once(ns, env, WORKSHOP / self.item, shot, self.work / f'{number}.log', [],
                                           ('--msaa', self.args.msaa, '--allow-parallax-overflow'))
        image = np.asarray(Image.open(shot).convert('RGB'))[0:1050, 0:1916].astype(np.float32) if status == 'ok' else None
        self.cache[number] = image
        return image

    def many(self, numbers, pool):
        todo = [n for n in numbers if n not in self.cache]
        list(pool.map(self.frame, todo))
        return {n: self.cache[n] for n in numbers}


def score(we_frames, ours, swap):
    best = (1e9, False)
    for flipped in ((False, True) if swap else (False,)):
        target = we_frames[..., ::-1] if flipped else we_frames
        value = float(np.abs(target - ours).mean())
        best = min(best, (value, flipped))
    return best


def scan_one(args, item, pool):
    key = Path(item).name
    times, frames = load_we(key)
    swap = has_video_texture(key)
    work = Path(tempfile.mkdtemp(prefix = 'align-', dir = args.out))
    renderer = Renderer(args, item, work)
    record = {'times': times}

    try:
        first = frames[0]
        coarse = renderer.many(list(range(args.first, args.last + 1, args.coarse)), pool)
        results = {n: score(first, img, swap)[0] for n, img in coarse.items() if img is not None}
        center = min(results, key = results.get)
        fine = renderer.many(list(range(max(center - args.coarse, 1), center + args.coarse + 1)), pool)
        results.update({n: score(first, img, swap)[0] for n, img in fine.items() if img is not None})
        start = min(results, key = results.get)
        record.update(align_frame = start, align_diff = round(results[start], 2),
                      neighbours = {str(n): round(results[n], 1) for n in (start - 1, start + 1) if n in results})

        aligned = []
        for time, we in zip(times, frames):
            expected = start + round(time * FPS)
            near = renderer.many([expected - 1, expected, expected + 1], pool)
            best = min((score(we, img, swap) + (n,) for n, img in near.items() if img is not None), default = None)
            aligned.append({'t': time, 'diff': round(best[0], 2), 'swapped': best[1], 'frame': best[2]} if best else None)

        record['aligned'] = aligned
        record['mean_aligned'] = round(float(np.mean([a['diff'] for a in aligned if a])), 2)

        if args.keep and start in renderer.cache:
            pair = np.concatenate([first[..., ::-1] if swap and aligned[0] and aligned[0]['swapped'] else first,
                                   renderer.cache[start]], axis = 1)
            Image.fromarray(np.clip(pair, 0, 255).astype(np.uint8)).resize((1916, 525)).save(args.out / f'{key}.png')
    finally:
        shutil.rmtree(work, ignore_errors = True)

    return item, record


def main():
    parser = argparse.ArgumentParser(description = __doc__.split('\n')[0])
    parser.add_argument('out', type = Path)
    parser.add_argument('ids', nargs = '*')
    parser.add_argument('--binary', type = Path, default = HERE.parent.parent / 'build/output/linux-wallpaperengine')
    parser.add_argument('--msaa', default = 'x2')
    parser.add_argument('--first', type = int, default = 30, help = 'first frame of the coarse scan (1/15 s each)')
    parser.add_argument('--last', type = int, default = 120)
    parser.add_argument('--coarse', type = int, default = 3)
    parser.add_argument('--jobs', type = int, default = 8)
    parser.add_argument('--keep', action = 'store_true', help = 'save the best aligned pair as <out>/<id>.png')
    args = parser.parse_args()
    args.out.mkdir(parents = True, exist_ok = True)
    path = args.out / 'align.json'
    results = json.loads(path.read_text()) if path.exists() else {}

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for item in args.ids:
            if not glob.glob(f'{REFS}/{Path(item).name}/f*.png'):
                print(item, 'no WE frames')
                continue
            item, record = scan_one(args, item, pool)
            results[Path(item).name] = record
            print(Path(item).name, 'aligned frame', record.get('align_frame'), 'f000 diff', record.get('align_diff'), 'mean over frames',
                  record.get('mean_aligned'), flush = True)
            path.write_text(json.dumps(results, indent = 1, sort_keys = True))


if __name__ == '__main__':
    main()
