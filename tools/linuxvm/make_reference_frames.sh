#!/bin/bash
# make_reference_frames.sh [binary] [payload dir] - renders the test wallpapers of the VM payload headlessly at 1920x1080 and keeps
# a 192x108 copy of each in reference/<id>.png, what check_frame.py --reference compares the VM screenshots with (upside down,
# wrong crop). Video wallpapers are left out, their frame depends on the moment of the shot. Needs a GPU render node.
HERE=$(cd "$(dirname "$0")" && pwd)
binary=${1:-$HERE/../../build/output/linux-wallpaperengine}
payload=${2:-$HOME/.local/share/linuxvm/payload}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$HERE/reference"
for dir in "$payload"/wp-*; do
    id=${dir##*wp-}
    type=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1] + '/project.json')).get('type', ''))" "$dir")
    case $type in
    video) continue ;;
    web) export HEADLESS_RENDER_WEB=1 HEADLESS_RENDER_DELAY=700 HEADLESS_RENDER_TIMEOUT=120 ;;
    *) unset HEADLESS_RENDER_WEB; export HEADLESS_RENDER_DELAY=150 HEADLESS_RENDER_TIMEOUT=60 ;;
    esac
    if HEADLESS_RENDER_SIZE=1920x1080 "$HERE/../headless_render.sh" "$binary" "$tmp/$id.png" --assets-dir "$payload/assets" "$dir" > "$tmp/$id.log" 2>&1 \
        && [ -s "$tmp/$id.png" ]; then
        python3 -I -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').resize((192, 108), Image.LANCZOS).save(sys.argv[2])" \
            "$tmp/$id.png" "$HERE/reference/$id.png" && echo "$id ($type): reference written"
    else
        echo "$id ($type): render failed, see $tmp/$id.log"; cp "$tmp/$id.log" "/tmp/make_reference_$id.log" 2>/dev/null
    fi
done
