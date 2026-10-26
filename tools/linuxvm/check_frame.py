"""check_frame.py [--baseline base.png] shot.png [shot.png ...] - did the wallpaper reach the screen?
check_frame.py --differ a.png b.png - do two frames differ (hotswap)?
check_frame.py --similar a.png b.png [max] - are two frames of the same wallpaper alike (a hotswap must look like a fresh start)?
check_frame.py --blend old.png new.png half.ppm - is the frame dumped halfway through a fade (LWE_TRANSITION_DUMP) about half old, half new?
check_frame.py --reference ref.png shot.png [shot.png ...] - does the shot look like the reference frame of that wallpaper (reference/<id>.png or
any reference/<id>-<variant>.png, a date dependent wallpaper has one per season,
make_reference_frames.sh), not upside down or mirrored, not another crop?
check_frame.py --crop from,to [--baseline base.png] shot.png - only the part of the width between the two fractions (one
monitor of a multi monitor screenshot)

Without --baseline a screenshot only fails when it is pure black. With the baseline (the desktop's own screenshot taken
before the engine started) it must also differ from it along the right edge and the top, where windows, panels and
desktop icons rarely are, so a desktop that still shows its default background fails. Prints one "PASS|FAIL <file>: <why>"
line per file, exit code 1 when any failed. With --reference the middle of the shot is compared cell by cell with the
reference frame of the wallpaper: enough cells must match (a window or an animation may cover some), and when too few do
and the flipped reference matches clearly better the shot is reported as upside down or mirrored.
"""
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageOps, ImageStat


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


GRID = (32, 18)
MIDDLE = (0.08, 0.08, 0.92, 0.86)
CELL_LIMIT = 36
MATCH_RATIO = 0.5


def cells(image):
    w, h = image.size
    middle = image.crop((int(w * MIDDLE[0]), int(h * MIDDLE[1]), int(w * MIDDLE[2]), int(h * MIDDLE[3])))
    small = middle.resize(GRID, Image.BOX)
    return list(small.get_flattened_data() if hasattr(small, "get_flattened_data") else small.getdata())


def match_ratio(a, b):
    close = sum(1 for p, q in zip(a, b) if sum(abs(x - y) for x, y in zip(p, q)) / 3 <= CELL_LIMIT)
    return close / len(a)


def fill_crop(reference, size):
    """the scene fills the screen and crops whichever axis overflows, so the reference is cut to the shot's shape"""
    w, h = reference.size
    wanted = size[0] / size[1]
    if abs(w / h - wanted) < 0.02:
        return reference
    if w / h > wanted:
        width = round(h * wanted)
        return reference.crop(((w - width) // 2, 0, (w - width) // 2 + width, h))
    height = round(w / wanted)
    return reference.crop((0, (h - height) // 2, w, (h - height) // 2 + height))


def check_reference(path, reference):
    image = crop(Image.open(path).convert("RGB"))
    reference = fill_crop(reference, image.size)
    shot = cells(image)
    straight = match_ratio(shot, cells(reference))
    for name, flipped in (("upside down", ImageOps.flip(reference)), ("mirrored", ImageOps.mirror(reference))):
        ratio = match_ratio(shot, cells(flipped))
        if ratio >= 0.6 and ratio >= straight + 0.15:
            return False, f"{name} ({ratio:.0%} of the cells match the flipped reference, {straight:.0%} the reference)"
    if straight < MATCH_RATIO:
        return False, f"does not look like the reference (only {straight:.0%} of the cells match)"
    return True, f"{straight:.0%} of the cells match the reference"


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


def blend(old, new, half, low=0.25, high=0.75):
    """Mean colour of a fade's halfway frame, as a share of the way from the old picture's to the new one's."""
    a, b, m = (ImageStat.Stat(Image.open(path).convert("RGB")).mean for path in (old, new, half))
    span = sum((y - x) ** 2 for x, y in zip(a, b))
    if sum(abs(y - x) for x, y in zip(a, b)) < 20:
        print(f"SKIP {half}: the two wallpapers have nearly the same average colour")
        sys.exit(0)
    share = sum((mc - ac) * (bc - ac) for mc, ac, bc in zip(m, a, b)) / span
    ok = low < share < high
    print(f"{'PASS' if ok else 'FAIL'} {half}: halfway through the fade the picture is {share:.0%} new")
    sys.exit(0 if ok else 1)


def main():
    global CROP
    args = sys.argv[1:]
    if args[:1] == ["--similar"]:
        similar(args[1], args[2], float(args[3]) if len(args) > 3 else 10)
    if args[:1] == ["--blend"]:
        blend(args[1], args[2], args[3])
    if args[:1] == ["--crop"]:
        CROP = [float(x) for x in args[1].split(",")]
        args = args[2:]
    if args[:1] == ["--differ"]:
        differ(args[1], args[2])
    if args[:1] == ["--reference"]:
        main_reference = Path(args[1])
        references = [main_reference] + sorted(main_reference.parent.glob(f"{main_reference.stem}-*.png"))
        failed = False
        for path in args[2:]:
            results = [check_reference(path, Image.open(reference).convert("RGB")) for reference in references]
            ok, why = next((r for r in results if r[0]), results[0])
            failed |= not ok
            print(f"{'PASS' if ok else 'FAIL'} {path}: {why}")
        sys.exit(1 if failed else 0)
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
