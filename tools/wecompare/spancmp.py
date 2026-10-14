#!/usr/bin/env python3
"""spancmp.py <we dir> <ours dir> [WxH+X+Y ...] - compares we_span.sh and ours_span.sh grabs of the same layout per
output: mean difference, share of pixels off by more than 24 levels, and the shift that lines the grids up
(phase correlation); writes <ours dir>/cmp_<n>.png (WE | ours | diff) per grab pair"""
import glob
import sys

import numpy as np
from PIL import Image

we_dir, ours_dir = sys.argv[1], sys.argv[2]
specs = sys.argv[3:] or ['1920x1080+0+0', '1280x1024+2120+200']
rects = []
for spec in specs:
    size, x, y = spec.split('+')
    w, h = size.split('x')
    rects.append((int(x), int(y), int(w), int(h)))


def shift(a, b):
    fa, fb = np.fft.fft2(a.mean(2)), np.fft.fft2(b.mean(2))
    r = np.fft.ifft2(fa * np.conj(fb) / (np.abs(fa * np.conj(fb)) + 1e-9)).real
    dy, dx = np.unravel_index(np.argmax(r), r.shape)
    return (dx if dx < a.shape[1] // 2 else dx - a.shape[1]), (dy if dy < a.shape[0] // 2 else dy - a.shape[0])


for n, (wf, of) in enumerate(zip(sorted(glob.glob(we_dir + '/f*.png')), sorted(glob.glob(ours_dir + '/f*.png')))):
    we = np.asarray(Image.open(wf).convert('RGB')).astype(np.int16)
    ours = np.asarray(Image.open(of).convert('RGB')).astype(np.int16)
    if we.shape != ours.shape:
        print(n, 'size differs', we.shape, ours.shape)
        continue
    diff = np.abs(we - ours).max(2)
    for i, (x, y, w, h) in enumerate(rects):
        d = diff[y:y + h, x:x + w]
        print('grab %d output %d: mean %.2f, >24: %.3f%%, shift %s' % (
            n, i + 1, d.mean(), (d > 24).mean() * 100, shift(we[y:y + h, x:x + w], ours[y:y + h, x:x + w])))
    out = np.concatenate([we, ours, np.repeat(np.minimum(diff * 4, 255)[..., None], 3, 2)], 0).astype(np.uint8)
    Image.fromarray(out).resize((out.shape[1] // 3, out.shape[0] // 3)).save('%s/cmp_%d.png' % (ours_dir, n))
