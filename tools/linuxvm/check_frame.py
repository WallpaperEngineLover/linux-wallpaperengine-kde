"""check_frame.py [--baseline base.png] shot.png [shot.png ...] - did the wallpaper reach the screen?
check_frame.py --differ a.png b.png - do two frames differ (hotswap)?
check_frame.py --similar a.png b.png [max] - are two frames of the same wallpaper alike (a hotswap must look like a fresh start)?
check_frame.py --crop from,to [--baseline base.png] shot.png - only the part of the width between the two fractions (one
monitor of a multi monitor screenshot)

Without --baseline a screenshot only fails when it is pure black. With the baseline (the desktop's own screenshot taken
before the engine started) it must also differ from it along the right edge and the top, where windows, panels and
desktop icons rarely are, so a desktop that still shows its default background fails. Prints one "PASS|FAIL <file>: <why>"
line per file, exit code 1 when any failed. Reference frames per wallpaper (upside down, wrong crop) would be the next
step, see RELEASE_TESTING.md.
"""
import sys
from PIL import Image, ImageChops, ImageStat


def strips(image):
    w, h = image.size
    return [
        image.crop((w * 88 // 100, h * 15 // 100, w * 99 // 100, h * 80 // 100)),
        image.crop((w * 30 // 100, h * 2 // 100, w * 85 // 100, h * 10 // 100)),
    ]


CROP = None


def crop(image):
    if CROP is None:
        return image
    w, h = image.size
    return image.crop((int(w * CROP[0]), 0, int(w * CROP[1]), h))


def check(path, baseline):
    image = crop(Image.open(path).convert("RGB"))
    parts = strips(image)
    stats = [ImageStat.Stat(part.convert("L")) for part in parts]
    mean = sum(stat.mean[0] for stat in stats) / len(stats)
    stddev = max(stat.stddev[0] for stat in stats)
    if mean < 1 and stddev < 1:
        return False, "black"
    if baseline is not None:
        base_parts = strips(crop(baseline).resize(image.size))
        diffs = [ImageStat.Stat(ImageChops.difference(a, b).convert("L")).mean[0] for a, b in zip(parts, base_parts)]
        diff = sum(diffs) / len(diffs)
        # a mostly black wallpaper has a small mean difference but still some lit pixels where a bare desktop has none
        histograms = [ImageChops.difference(a, b).convert("L").histogram() for a, b in zip(parts, base_parts)]
        lit = sum(sum(h[9:]) for h in histograms) / sum(sum(h) for h in histograms)
        if diff < 6 and lit < 0.003:
            return False, f"same as the desktop without a wallpaper (difference {diff:.1f})"
        return True, f"mean {mean:.0f}, difference from the baseline {diff:.0f}"
    return True, f"mean {mean:.0f}, stddev {stddev:.0f}"


def differ(a, b):
    """Two frames of the same desktop must differ clearly (a hotswap changed the picture)."""
    first, second = Image.open(a).convert("RGB"), Image.open(b).convert("RGB").resize(Image.open(a).size)
    parts = list(zip(strips(first), strips(second)))
    diff = sum(ImageStat.Stat(ImageChops.difference(x, y).convert("L")).mean[0] for x, y in parts) / len(parts)
    ok = diff >= 6
    print(f"{'PASS' if ok else 'FAIL'} {b}: {'differs from' if ok else 'same as'} {a} (difference {diff:.1f})")
    sys.exit(0 if ok else 1)


def similar(a, b, limit):
    size = (96, 54)
    first, second = Image.open(a).convert("RGB").resize(size), Image.open(b).convert("RGB").resize(size)
    diff = ImageStat.Stat(ImageChops.difference(first, second).convert("L")).mean[0]
    ok = diff <= limit
    print(f"{'PASS' if ok else 'FAIL'} {b}: {'like' if ok else 'not like'} {a} (difference {diff:.1f}, limit {limit:g})")
    sys.exit(0 if ok else 1)


def main():
    global CROP
    args = sys.argv[1:]
    if args[:1] == ["--similar"]:
        similar(args[1], args[2], float(args[3]) if len(args) > 3 else 10)
    if args[:1] == ["--crop"]:
        CROP = [float(x) for x in args[1].split(",")]
        args = args[2:]
    if args[:1] == ["--differ"]:
        differ(args[1], args[2])
    baseline = None
    if args[:1] == ["--baseline"]:
        baseline = Image.open(args[1]).convert("RGB")
        args = args[2:]
    failed = False
    for path in args:
        ok, why = check(path, baseline)
        failed |= not ok
        print(f"{'PASS' if ok else 'FAIL'} {path}: {why}")
    sys.exit(1 if failed else 0)


main()
