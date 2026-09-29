#!/usr/bin/env python3
"""Compares our renders against the real-WE reference frames.

    compare.py <ours dir> <review out dir> [ids...]

Scores each item against the closest of its WE frames (they animate), writes <review>/<id>.png
(top: WE | ours, bottom: diff heat | heat over ours) and small side-by-side sheets for skimming,
and prints/stores the scores in <review>/scores.json. WE window crop = [30:1080, 4:1920] of the
:98 screen, ours rendered at 1920x1058 -> [0:1050, 0:1916].
"""
import glob, json, os, re, sys
import numpy as np
from PIL import Image

REFS = os.environ.get('REFS', os.path.expanduser('~/.local/share/we_live/refs'))


def log_problems(path):
    if not os.path.exists(path):
        return []
    out = []
    for line in open(path, errors='replace'):
        if re.search(r'glslang|shader.*(fail|error)|exception|Segmentation|terminate called|could not|cannot|failed', line, re.I) \
                and 'NoReply' not in line and 'dbus' not in line.lower():
            out.append(line.strip()[:200])
    return out


def shift(a, b):
    """global (dx, dy) in pixels of b relative to a, phase correlation on grey images"""
    ga, gb = a.mean(axis=2), b.mean(axis=2)
    fa, fb = np.fft.fft2(ga - ga.mean()), np.fft.fft2(gb - gb.mean())
    r = fa.conj() * fb
    c = np.abs(np.fft.ifft2(r / (np.abs(r) + 1e-6)))
    y, x = np.unravel_index(np.argmax(c), c.shape)
    h, w = ga.shape
    return int(x if x < w // 2 else x - w), int(y if y < h // 2 else y - h), round(float(c.max()), 3)


def main():
    ours_dir, rev = sys.argv[1], sys.argv[2]
    os.makedirs(rev, exist_ok=True)
    ids = sys.argv[3:] or sorted(os.path.basename(p)[:-4] for p in glob.glob(ours_dir + '/*.png'))
    spath = os.path.join(rev, 'scores.json')
    scores = json.load(open(spath)) if os.path.exists(spath) else {}
    thumbs = []
    for id_ in ids:
        we = [np.asarray(Image.open(f).convert('RGB'))[30:1080, 4:1920].astype(np.float32)
              for f in sorted(glob.glob(f'{REFS}/{id_}/f*.png'))]
        p = f'{ours_dir}/{id_}.png'
        rec = {'we_frames': len(we), 'ours': os.path.exists(p), 'our_log': log_problems(f'{ours_dir}/{id_}.log')[:8]}
        if not we or not rec['ours']:
            scores[id_] = rec
            print(id_, 'missing', rec)
            continue
        ours = np.asarray(Image.open(p).convert('RGB'))[0:1050, 0:1916].astype(np.float32)
        diffs = [np.abs(w - ours).mean() for w in we]
        best = we[int(np.argmin(diffs))]
        motion = float(np.mean([np.abs(we[i] - we[i + 1]).mean() for i in range(len(we) - 1)])) if len(we) > 1 else 0.0
        # blank WE frames (load failure) show up as near-uniform images
        rec.update(diff=round(float(min(diffs)), 2), we_motion=round(motion, 2),
                   we_std=round(float(best.std()), 1), ours_std=round(float(ours.std()), 1),
                   big_diff_area=round(float((np.abs(best - ours).mean(axis=2) > 48).mean()), 3),
                   shift=shift(best, ours))
        scores[id_] = rec
        heat = np.clip(np.abs(best - ours).mean(axis=2) * 3, 0, 255)
        top = np.concatenate([best, ours], axis=1)
        grey = np.asarray(Image.fromarray(ours.astype(np.uint8)).convert('L').convert('RGB')).astype(np.float32) / 3
        bottom = np.concatenate([np.stack([heat] * 3, axis=2), grey + np.stack([heat, heat / 4, heat / 4], axis=2)], axis=1)
        Image.fromarray(np.clip(np.concatenate([top, bottom], axis=0), 0, 255).astype(np.uint8)).resize((1916, 1050)).save(f'{rev}/{id_}.png')
        thumbs.append((id_, Image.fromarray(top.astype(np.uint8)).resize((1280, 350))))
        print(f"{id_} diff={rec['diff']} area={rec['big_diff_area']} we-motion={rec['we_motion']} stds={rec['we_std']}/{rec['ours_std']} shift={rec['shift']}")
    json.dump(scores, open(spath, 'w'), indent=1, sort_keys=True)
    # sheets of 4 (WE left, ours right) for skimming
    for n in range(0, len(thumbs), 4):
        chunk = thumbs[n:n + 4]
        sheet = Image.new('RGB', (1280, 360 * len(chunk)), (255, 0, 255))
        for i, (id_, t) in enumerate(chunk):
            sheet.paste(t, (0, i * 360))
        sheet.save(f'{rev}/sheet_{n // 4:02d}_{"_".join(c[0] for c in chunk)}.png')


if __name__ == '__main__':
    main()
