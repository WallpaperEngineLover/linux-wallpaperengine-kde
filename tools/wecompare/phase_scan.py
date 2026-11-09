#!/usr/bin/env python3
"""Scores our render at many time offsets against every real-WE frame of an item.

    phase_scan.py <out dir> [ids...]       (REFS, LWE_WORKSHOP_DIR, LWE_ASSETS_DIR as for compare.py)

WE animates and its frames are taken a few seconds after the load, so one frame of ours says little when the two are at
different points of a loop. This renders ours with a fixed timestep (1/30 s per frame) at --frames offsets, scores every
render against every WE frame with compare.py's metric (mean absolute difference of the window crop) and keeps the best
pair as <out>/<id>.png (WE | ours) with the whole table in <out>/phase.json. An item that is the same in both engines
reaches the noise floor at some offset; one that stays high at every offset differs for a real reason.
Clock and time zone are not pinned: WE under Wine reads the real clock too.
"""
import argparse
import concurrent.futures
import glob
import json
import os
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


def has_video_texture(item):
    path = Path(os.environ.get('LWE_LIBRARY_DIR', os.path.expanduser('~/.local/share/lwe-library'))) / 'features.json'
    try:
        return 'video-tex' in json.loads(path.read_text()).get(item, {}).get('features', {})
    except (OSError, ValueError):
        return False


def load_we(item, try_swap):
    """WE frames, plus the same frames with red and blue exchanged for items with a video texture: live WE under Wine
    draws some of those swapped (not all, 3155776049's are right), scan_one keeps whichever fits better."""
    frames = [np.asarray(Image.open(f).convert('RGB'))[30:1080, 4:1920].astype(np.float32)
              for f in sorted(glob.glob(f'{REFS}/{item}/f*.png'))]
    if try_swap and has_video_texture(item):
        return frames + [f[..., ::-1] for f in frames]
    return frames


def scan_one(args, item, we):
    work = Path(tempfile.mkdtemp(prefix = 'phase-', dir = args.out))
    table = []
    best = (1e9, None, None, None)
    half = len(glob.glob(f'{REFS}/{item}/f*.png'))

    try:
        for frame in args.frames:
            ns = argparse.Namespace(binary = args.binary, width = 1920, height = 1058, frame = frame, assets = ASSETS,
                                    timeout = 120, memory_limit = 8, step = 1 / 30, clock = 0, gpu = 'auto', jobs = args.jobs)
            env = regress.render_env(ns, '', None)
            env.pop('LWE_FIXED_CLOCK', None)
            env.pop('LD_PRELOAD', None)
            env['TZ'] = os.environ.get('TZ', 'Europe/Berlin')
            env['LWE_HEADLESS_CURSOR'] = '0.49792,0.51796'
            shot = work / f'{frame}.png'
            status, _, _ = regress.render_once(ns, env, WORKSHOP / item, shot, work / f'{frame}.log', [], ('--msaa', args.msaa, '--allow-parallax-overflow'))

            if status != 'ok':
                table.append({'frame': frame, 'status': status})
                continue

            ours = np.asarray(Image.open(shot).convert('RGB'))[0:1050, 0:1916].astype(np.float32)
            diffs = [float(np.abs(w - ours).mean()) for w in we]
            k = int(np.argmin(diffs))
            table.append({'frame': frame, 'best_we': k % half, 'swapped': k >= half, 'diff': round(diffs[k], 2)})

            if diffs[k] < best[0]:
                best = (diffs[k], frame, k, ours)

        record = {'we_frames': len(we), 'table': table}

        if best[3] is not None:
            record.update(best_diff = round(best[0], 2), best_frame = best[1], best_we = best[2] % half, swapped = best[2] >= half)
            pair = np.concatenate([we[best[2]], best[3]], axis = 1)
            Image.fromarray(np.clip(pair, 0, 255).astype(np.uint8)).resize((1916, 525)).save(args.out / f'{item}.png')
    finally:
        shutil.rmtree(work, ignore_errors = True)

    return item, record


def main():
    parser = argparse.ArgumentParser(description = __doc__.split('\n')[0])
    parser.add_argument('out', type = Path)
    parser.add_argument('ids', nargs = '*')
    parser.add_argument('--binary', type = Path, default = HERE.parent.parent / 'build/output/linux-wallpaperengine')
    parser.add_argument('--frames', type = lambda s: [int(x) for x in s.split(',')],
                        default = list(range(15, 601, 15)), help = 'comma separated frame counts (default 15..600 step 15)')
    parser.add_argument('--jobs', type = int, default = 8)
    parser.add_argument('--msaa', default = 'x2', help = "anti-aliasing of the render, the rig's WE config has x2 (alpha to coverage sprites go black without it)")
    parser.add_argument('--swap-rb-video', action = 'store_true',
                        help = 'also try the WE frames of video texture items with red and blue exchanged (Wine draws some that way)')
    args = parser.parse_args()
    args.out.mkdir(parents = True, exist_ok = True)
    ids = args.ids or sorted(p.name for p in Path(REFS).iterdir() if glob.glob(f'{p}/f*.png'))
    path = args.out / 'phase.json'
    results = json.loads(path.read_text()) if path.exists() else {}

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = []
        for item in ids:
            we = load_we(item, args.swap_rb_video)
            if we and (WORKSHOP / item / 'project.json').exists():
                futures.append(pool.submit(scan_one, args, item, we))
        for done in concurrent.futures.as_completed(futures):
            item, record = done.result()
            results[item] = record
            print(item, record.get('best_diff'), 'frame', record.get('best_frame'), flush = True)
            path.write_text(json.dumps(results, indent = 1, sort_keys = True))


if __name__ == '__main__':
    main()
