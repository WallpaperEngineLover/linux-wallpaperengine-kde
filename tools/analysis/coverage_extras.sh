#!/usr/bin/env bash
# What the unit tests and tools/regression/regress.py (scenes) do not run, for a coverage build: every installed web
# wallpaper (CEF host, HEADLESS_RENDER_WEB=1), the presets (they load their base wallpaper, which can be a web page), the SOG splat item, the video wallpapers and the tools/check_*.sh scripts that drive hotswap, transitions, playlists and splats.
# Runs one at a time. Usage: tools/analysis/coverage_extras.sh <engine binary of the coverage build> [assets] [workshop content]
# Afterwards: gcovr -r . --object-directory <build-cov> --filter 'src/WallpaperEngine/' --exclude 'src/WallpaperEngine/Testing'
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
work=$(mktemp -d /tmp/lwe-coverage-extras.XXXXXX)
trap 'rm -rf "$work"' EXIT

kinds=$(python3 -I - "$CONTENT" <<'PY'
import glob, json, sys
for path in sorted(glob.glob(sys.argv[1] + '/*/project.json')):
    try:
        project = json.load(open(path))
    except Exception:
        continue
    kind = str(project.get('type', '')).lower()
    if not kind and project.get('preset') is not None and project.get('dependency'):
        kind = 'preset'
    if kind in ('web', 'video', 'preset'):
        print(kind, path.split('/')[-2])
PY
)

while read -r kind id; do
    [ -n "$id" ] || continue
    env=()
    delay=150
    if [ "$kind" = web ] || [ "$kind" = preset ]; then env=(HEADLESS_RENDER_WEB=1); delay=700; fi
    if env "${env[@]}" HEADLESS_RENDER_DELAY=$delay HEADLESS_RENDER_TIMEOUT=150 HEADLESS_RENDER_SIZE=1280x720 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" "$CONTENT/$id" > "$work/$id.log" 2>&1; then
        echo "ok   $kind $id"
    else
        echo "FAIL $kind $id"
        tail -3 "$work/$id.log"
    fi
    rm -f "$work/shot.png"
done <<< "$kinds"

for check in check_dump_structure check_hotswap_keys check_hotswap_video check_splat check_transition check_video_sequence check_video_start; do
    echo "== $check"
    "$HERE/$check.sh" "$BINARY" "$ASSETS" "$CONTENT" 2>&1 | tail -4
done
