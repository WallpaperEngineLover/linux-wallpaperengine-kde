#!/usr/bin/env python3
"""frames.py <id> <ours.png> <out.png> - all WE ref frames + ours in one grid (for animated items)"""
import glob, os, sys
from PIL import Image
id_, ours, out = sys.argv[1:4]
fs = sorted(glob.glob(os.path.expanduser(f'~/.local/share/we_live/refs/{id_}/f*.png')))
ims = [Image.open(f).convert('RGB').crop((4, 30, 1920, 1080)) for f in fs] + [Image.open(ours).convert('RGB').crop((0, 0, 1916, 1050))]
g = Image.new('RGB', (1920, 350 * ((len(ims) + 2) // 3)), (255, 0, 255))
for i, im in enumerate(ims):
    g.paste(im.resize((638, 350)), ((i % 3) * 640, (i // 3) * 350))
g.save(out)
