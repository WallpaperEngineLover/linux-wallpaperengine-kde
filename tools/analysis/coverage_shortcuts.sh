#!/usr/bin/env bash
# User shortcut icons: a wallpaper with usershortcut properties (Holi Burst, 3440956853) gets private .desktop files whose
# Icon is a SVG, a PNG, a missing file and a broken SVG, a folder and a URL (plain values have no icon). Nothing is
# clicked, so no program starts.
# Usage: tools/analysis/coverage_shortcuts.sh <engine binary> [assets dir] [workshop content dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
ITEM=${ITEM:-3440956853}
[ -d "$CONTENT/$ITEM" ] || { echo "SKIP: wallpaper $ITEM not in $CONTENT"; exit 0; }
work=$(mktemp -d /tmp/lwe-coverage-shortcuts.XXXXXX)
trap 'rm -rf "$work"' EXIT
export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent

cat > "$work/icon.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48"><rect width="64" height="48" fill="#3080ff"/><circle cx="32" cy="24" r="16" fill="#fff"/></svg>
SVG
echo '<svg' > "$work/broken.svg"
python3 -I - "$work/icon.png" <<'PY'
import sys
from PIL import Image
Image.new("RGBA", (40, 40), (200, 40, 40, 255)).save(sys.argv[1])
PY
for name in svg png broken missing; do
    case $name in
        svg) icon=$work/icon.svg ;;
        png) icon=$work/icon.png ;;
        broken) icon=$work/broken.svg ;;
        missing) icon=$work/nothing.png ;;
    esac
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\nIcon=%s\n' "$name" "$icon" > "$work/$name.desktop"
done
mkdir -p "$work/folder"

HEADLESS_RENDER_DELAY=60 HEADLESS_RENDER_TIMEOUT=120 HEADLESS_RENDER_SIZE=1280x720 \
    "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" \
    --set-property "launcher1=$work/svg.desktop" --set-property "launcher2=$work/png.desktop" \
    --set-property "launcher3=$work/broken.desktop" --set-property "launcher4=$work/missing.desktop" \
    --set-property "launcher5=$work/folder" --set-property "launcher6=https://example.invalid/" \
    --set-property "launcher7={\"commandtype\":\"command\",\"file\":\"true\"}" \
    --set-property "launcher8=nothing-installed.desktop" "$CONTENT/$ITEM" > "$work/run.log" 2>&1
echo "exit $?"
