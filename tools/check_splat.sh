#!/usr/bin/env bash
# Checks the native SOG splat renderer on the base SOG wallpaper (3752541815) with a headless GPU render:
#  - the picture comes out at the output's size (the frame buffer object was stuck at 16x17, screenshots were blocks)
#  - a sudden pointer zigzag makes the mouse ripple change the picture, and the property turns it off again
#  - performance mode draws at half size (its default) and still fills the screenshot
# Usage: tools/check_splat.sh <engine binary> [assets dir] [workshop content dir]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
ID=${SPLAT_ID:-3752541815}
[ -d "$CONTENT/$ID" ] || { echo "SKIP: wallpaper $ID not in $CONTENT"; exit 0; }
work=$(mktemp -d /tmp/lwe-splat.XXXXXX)
trap 'rm -rf "$work"' EXIT
PATH_ZIGZAG="2.0:0.2,0.5;2.03:0.8,0.5;2.06:0.2,0.5;2.09:0.8,0.5;2.12:0.2,0.5;2.15:0.8,0.5;2.18:0.5,0.5"

shot () { # name delay [property=value ...]
    local name=$1 delay=$2; shift 2
    local args=()
    for property in "$@"; do args+=(--set-property "$property"); done
    LWE_HEADLESS_CURSOR_PATH="$PATH_ZIGZAG" HEADLESS_RENDER_SIZE=1280x720 HEADLESS_RENDER_DELAY=$delay \
        "$HERE/headless_render.sh" "$BINARY" "$work/$name.png" --assets-dir "$ASSETS" --silent --fps 30 \
        "${args[@]}" "$CONTENT/$ID" > "$work/$name.log" 2>&1 || { cat "$work/$name.log"; exit 1; }
}

shot ripple 80 performanceMode=false mouseTrailEnabled=true
shot still 80 performanceMode=false mouseTrailEnabled=false
shot half 80 performanceMode=true mouseTrailEnabled=false

python3 - "$work" <<'PY'
import sys
import numpy as np
from PIL import Image

work = sys.argv[1]
load = lambda name: np.asarray(Image.open(f'{work}/{name}.png').convert('RGB')).astype(int)
ripple, still, half = load('ripple'), load('still'), load('half')
failed = False

if still.mean() < 30:
    print(f'FAIL: the picture is almost black (mean {still.mean():.1f}), the output is not the full frame'); failed = True

changed = (np.abs(ripple - still).max(axis=2) > 8).mean() * 100
if changed < 2:
    print(f'FAIL: the ripple changed only {changed:.2f}% of the picture'); failed = True
else:
    print(f'ok: the ripple changed {changed:.1f}% of the picture')

difference = np.abs(half - still).mean()
if difference == 0 or difference > 25:
    print(f'FAIL: performance mode picture differs by {difference:.1f} from the full size one'); failed = True
else:
    print(f'ok: performance mode picture differs by {difference:.1f} (half size, stretched)')

sys.exit(1 if failed else 0)
PY
