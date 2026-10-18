"""check_frame.py [--baseline base.png] shot.png [shot.png ...] - did the wallpaper reach the screen?

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


def check(path, baseline):
    image = Image.open(path).convert("RGB")
    parts = strips(image)
    stats = [ImageStat.Stat(part.convert("L")) for part in parts]
    mean = sum(stat.mean[0] for stat in stats) / len(stats)
    stddev = max(stat.stddev[0] for stat in stats)
    if mean < 1 and stddev < 1:
        return False, "black"
    if baseline is not None:
        base_parts = strips(baseline.resize(image.size))
        diffs = [ImageStat.Stat(ImageChops.difference(a, b).convert("L")).mean[0] for a, b in zip(parts, base_parts)]
        diff = sum(diffs) / len(diffs)
        if diff < 6:
            return False, f"same as the desktop without a wallpaper (difference {diff:.1f})"
        return True, f"mean {mean:.0f}, difference from the baseline {diff:.0f}"
    return True, f"mean {mean:.0f}, stddev {stddev:.0f}"


def main():
    args = sys.argv[1:]
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
