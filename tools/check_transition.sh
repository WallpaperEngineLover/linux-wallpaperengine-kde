#!/usr/bin/env bash
# Checks --transition: hotswaps a scene onto another one headlessly on the GPU (headless_render.sh) with a fixed time step, so
# frame N always shows the same moment, and looks at the picture halfway through the transition and after it.
#   fade:        halfway the picture is half old, half new (mean colour between the two)
#   slide-left:  halfway the right side shows the new wallpaper and the left side the right half of the old one
# Screenshots never contain a transition (they read the wallpaper's own buffer), so the engine dumps the output at the halfway
# frame (LWE_TRANSITION_DUMP).
# Usage: tools/check_transition.sh <engine binary> [assets dir] [workshop content dir]
# Exit code 0 when they look right (or the wallpapers are missing, then it says SKIP). Needs Pillow.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
FROM=1120596121 TO=1111948150
STEP=0.05 SWAP_FRAME=30 DURATION=1000
HALF=$((SWAP_FRAME + DURATION / 2 / 50)) END=$((HALF + 10))
for id in $FROM $TO; do [ -d "$CONTENT/$id" ] || { echo "SKIP: wallpaper $id not in $CONTENT"; exit 0; }; done
work=$(mktemp -d /tmp/lwe-transition.XXXXXX)
trap 'rm -rf "$work"' EXIT
export LWE_FIXED_TIMESTEP=$STEP

render() {  # <out> <frame> <wallpaper> [engine args...]
    local out=$1 frame=$2 id=$3; shift 3
    HEADLESS_RENDER_DELAY=$frame "$HERE/headless_render.sh" "$BINARY" "$out" --assets-dir "$ASSETS" "$@" "$CONTENT/$id" \
        > "$out.log" 2>&1 || { cat "$out.log"; exit 1; }
}

render "$work/old.png" $((SWAP_FRAME - 1)) $FROM
render "$work/new-half.png" "$HALF" $TO

echo "path=$CONTENT/$TO" > "$work/ctl"
for mode in fade slide-left; do
    LWE_TRANSITION_DUMP="$work/$mode-half.ppm" LWE_HOTSWAP_AT_FRAME=$SWAP_FRAME render "$work/$mode.png" "$END" $FROM \
        --control-file "$work/ctl" --transition "$mode" --transition-duration $DURATION
done

python3 -I - "$work" <<'PY'
import sys
from PIL import Image, ImageChops, ImageOps, ImageStat

work = sys.argv[1]
load = lambda name: Image.open(f"{work}/{name}").convert("RGB")
mean = lambda im: ImageStat.Stat(im).mean
diff = lambda a, b: sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3
part = lambda im, x0, x1: im.crop((int(im.width * x0), 0, int(im.width * x1), im.height))
bad = 0


def verdict(ok, text):
    global bad
    print(("PASS " if ok else "FAIL ") + text)
    bad += not ok


old, new = load("old.png"), load("new-half.png")
fade, slide = ImageOps.flip(load("fade-half.ppm")), ImageOps.flip(load("slide-left-half.ppm"))

a, b, m = mean(old), mean(new), mean(fade)
span = sum(abs(x - y) for x, y in zip(a, b))
if span < 20:
    print("SKIP fade midpoint: the two wallpapers have nearly the same average colour")
else:
    share = sum((mc - ac) * (bc - ac) for mc, ac, bc in zip(m, a, b)) / sum((bc - ac) ** 2 for ac, bc in zip(a, b))
    verdict(0.3 < share < 0.7, f"fade: halfway the picture is {share:.0%} new")

verdict(diff(part(slide, 0.6, 1.0), part(new, 0.6, 1.0)) < 14, "slide-left: halfway the right side shows the new wallpaper")
best = min(diff(part(slide, 0.0, 0.4), part(old, s, s + 0.4)) for s in [0.5 + i / 100 for i in range(11)])
verdict(best < 14, f"slide-left: halfway the left side is the right part of the old picture (difference {best:.1f})")
sys.exit(1 if bad else 0)
PY
