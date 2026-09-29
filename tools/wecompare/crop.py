#!/usr/bin/env python3
"""crop.py <id> <ours.png> <out.png> x y w h [scale] - same region (window coords) of the best WE frame and ours, side by side"""
import glob, os, sys
import numpy as np
from PIL import Image
id_, ours, out = sys.argv[1:4]
x, y, w, h = map(int, sys.argv[4:8])
k = int(sys.argv[8]) if len(sys.argv) > 8 else 2
o = Image.open(ours).convert('RGB').crop((0, 0, 1916, 1050))
fs = sorted(glob.glob(os.path.expanduser(f'~/.local/share/we_live/refs/{id_}/f*.png')))
we = [Image.open(f).convert('RGB').crop((4, 30, 1920, 1080)) for f in fs]
oa = np.asarray(o, np.float32)
b = min(we, key=lambda im: np.abs(np.asarray(im, np.float32) - oa).mean())
g = Image.new('RGB', (2 * w * k + 8, h * k), (255, 0, 255))
g.paste(b.crop((x, y, x + w, y + h)).resize((w * k, h * k), Image.NEAREST), (0, 0))
g.paste(o.crop((x, y, x + w, y + h)).resize((w * k, h * k), Image.NEAREST), (w * k + 8, 0))
g.save(out)
