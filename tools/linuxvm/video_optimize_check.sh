#!/bin/bash
# video_optimize_check.sh <vm> <we_manager dist/linux-unpacked | .deb> [screen] - we_manager's "Optimize video" on a VM, through the
# real app over DevTools (cdp.py): a generated 1920x1080 non-HEVC video in an extra library folder (the entry is a local one, Steam's
# folders are off limits) is converted to H.265, then scaled down in fit and fill mode, a conversion that has nothing to do is
# refused, a discarded result leaves the file as it was, and the engine plays the converted file. The encoder is whatever works
# in the guest (GPU encoders, else libx265); without any encoder the app downloads its ffmpeg build, a download that fails is a
# FAIL (a platform the app has no build for is a SKIP). FFmpeg's own checks (size, codec) use the app's downloaded ffmpeg and
# ffprobe when it has them, else the system's. The app runs in English. Prints one PASS|FAIL|SKIP line per check.
# FAKE_FFMPEG=1 (default, 0 turns it off): the app's ffmpeg download in a guest without an encoder comes from fake_github.py on the host (wem_lib.sh wem_ffmpeg_fake_start), needs the cache in ~/.cache/lwe-release-prep.
# WEM_LAUNCH and WEM_FLAGS are added to the app's start line like in the other we_manager scripts.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2 screen=${3:-Virtual-1}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
skip() { echo "SKIP $1"; }
CDP_TIMEOUT=${CDP_TIMEOUT:-300}
. "$HERE/wem_lib.sh"
ID=900000001

sshv 'mkdir -p ~/wem; pkill -f "[w]e-manager"; pkill -x linux-wallpaper; pkill -x python3; rm -rf ~/lwe-video-lib; true'
wem_wait_gone
wem_install || exit 1
wem_steam_lib
wem_engine_fallback
FAKE_FFMPEG=${FAKE_FFMPEG:-1}
fake_work=
if [ "$FAKE_FFMPEG" = 1 ]; then
    fake_work=$(mktemp -d)
    if wem_ffmpeg_fake_start "$fake_work"; then
        trap 'wem_ffmpeg_fake_stop; rm -rf "$fake_work"' EXIT
        wem_start_line="env NODE_EXTRA_CA_CERTS=\$HOME/wem/fake-ca.pem $wem_start_line"
    else
        echo "     no cached ffmpeg or the fake release did not start, the app uses the guest's own or the real download"
        rm -rf "$fake_work"; fake_work=
        wem_ffmpeg_seed
    fi
else
    wem_ffmpeg_seed
fi
wem_run || exit 1
HOME_DIR=$(sshv 'echo $HOME')
LIB=$HOME_DIR/lwe-video-lib

stop_app() { cdp 'window.electronAPI.lwe.killAll()' >/dev/null; sshv 'pkill -f "[w]e-manager"; pkill -x python3; rm -rf ~/lwe-video-lib; true'; }

cdp "window.electronAPI.config.setLanguage('en')" >/dev/null
settings() {
    cdp "window.electronAPI.videoTools.setSettings({backupOriginal: $1, encoder: 'auto', crf: 26, scaleTo: '$2', scaleMode: 'fit'})" >/dev/null
}
encoder_list() { cdp 'window.electronAPI.videoTools.status().then(s => s.encoders.map(e => e.encoder).join(String.fromCharCode(43)))'; }
probe_info() { cdp "window.electronAPI.videoTools.probe('${1:-$ID}').then(p => p.info.codec + ',' + p.info.width + ',' + p.info.height)"; }

encoders=$(encoder_list)
if ! [[ $encoders =~ ^\"[a-z0-9_+]*\"$ ]]; then
    fail "the app does not answer: $encoders $(sshv 'tail -3 ~/wem/app.log')"; stop_app; exit 1
fi
if [ "$encoders" = '""' ]; then
    if [ "$(cdp 'window.electronAPI.videoTools.status().then(s => s.canDownload)')" != true ]; then
        skip "no H.265 encoder here and the app has no ffmpeg build for this platform"; stop_app; exit 0
    fi
    if [ -n "$fake_work" ]; then
        echo "     no encoder in the guest, downloading the app's ffmpeg from the fake release on the host"
    else
        echo "     no encoder in the guest, downloading the app's ffmpeg"
    fi
    dl=$(CDP_TIMEOUT=900 cdp 'window.electronAPI.videoTools.download()')
    if [ -n "$fake_work" ]; then
        grep -q 'ffmpeg-n7.1-latest-linux64-gpl-7.1.tar.xz' "$fake_work/fake.log" && pass "ffmpeg download served by the fake release" \
            || fail "the ffmpeg download did not come from the fake release: $(tail -3 "$fake_work/fake.log")"
    fi
    wem_ffmpeg_keep
    encoders=$(encoder_list)
    if [ "$encoders" = '""' ] || ! [[ $encoders =~ ^\"[a-z0-9_+]+\"$ ]]; then
        fail "no H.265 encoder after the ffmpeg download: $dl $encoders"; stop_app; exit 1
    fi
fi
pass "H.265 encoders: $encoders"

read -r FFMPEG FFPROBE < <(sshv '. ~/sessenv; d=${XDG_DATA_HOME:-$HOME/.local/share}/we-manager/ffmpeg; if [ -x $d/ffmpeg ] && [ -x $d/ffprobe ]; then echo "$d/ffmpeg $d/ffprobe"; else echo "$(command -v ffmpeg) $(command -v ffprobe)"; fi')
if [ -z "$FFMPEG" ] || [ -z "$FFPROBE" ]; then fail "no ffmpeg and ffprobe pair in the guest"; stop_app; exit 1; fi

gen_video() {
    sshv "mkdir -p $LIB/$1 && cd $LIB/$1 &&
      { $FFMPEG -y -v error -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 3 -c:v libx264 -crf 10 -pix_fmt yuv420p vid.mp4 ||
        $FFMPEG -y -v error -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 3 -c:v mpeg4 -b:v 8M -pix_fmt yuv420p vid.mp4; } &&
      $FFMPEG -y -v error -i vid.mp4 -frames:v 1 preview.jpg &&
      echo '{\"title\":\"$2\",\"type\":\"video\",\"file\":\"vid.mp4\",\"preview\":\"preview.jpg\",\"tags\":[]}' > project.json"
}
gen_video "$ID" vidtest || { fail "could not generate the test video"; stop_app; exit 1; }

vprobe_of() { sshv "$FFPROBE -v error -select_streams v:0 -show_entries stream=codec_name,width,height -of csv=p=0 $LIB/${1:-$ID}/vid.mp4"; }
expect_video() {
    local got; got=$(vprobe_of "${3:-$ID}")
    [ "$got" = "$1" ] && pass "$2 ($got)" || fail "$2: wanted $1, the file is $got"
}
originals() { sshv "ls $LIB/${1:-$ID} | grep -c '(original'"; }
temps() { sshv "ls -A $LIB/${1:-$ID} | grep -c optimizing"; }
optimize() { cdp "window.electronAPI.videoTools.start('$ID', $1)"; }
finish() { cdp "window.electronAPI.videoTools.finish('$ID', $1)" >/dev/null; }

settings true none
cdp "window.electronAPI.config.setExtraLibraries([{path: '$LIB', name: 'vidtest'}])" >/dev/null
cdp 'window.electronAPI.library.scan()' >/dev/null
info=$(probe_info)
case $info in
*Error*|'') fail "the app can't probe the test video: $info"; stop_app; exit 1 ;;
esac
[ "$info" != '"hevc,1920,1080"' ] && [[ $info == *,1920,1080* ]] && pass "source probed: $info" || fail "unexpected source: $info"

out=$(optimize "{encoder: 'auto'}")
if [[ $out == *newSize* ]]; then pass "H.265 conversion ran: $out"; else fail "conversion failed: $out"; fi
finish true
expect_video hevc,1920,1080 "converted file is H.265 at the original size"
tag=$(sshv "$FFPROBE -v error -select_streams v:0 -show_entries stream=codec_tag_string -of csv=p=0 $LIB/$ID/vid.mp4")
[ "$tag" = hvc1 ] || [ "$tag" = hev1 ] && pass "the mp4 carries a usable H.265 tag ($tag)" || fail "unexpected H.265 tag in the mp4: $tag"
[ "$(originals)" -ge 1 ] && pass "a copy of the original was kept next to the video" || fail "no (original) copy after replacing the video"
orig=$(sshv "$FFPROBE -v error -select_streams v:0 -show_entries stream=codec_name,width,height -of csv=p=0 \"$LIB/$ID/vid (original).mp4\"")
[[ $orig == *,1920,1080 ]] && [ "$orig" != hevc,1920,1080 ] && pass "the copy is the untouched source ($orig)" || fail "the (original) copy is not the source: $orig"
[ "$(temps)" = 0 ] && pass "no temp file left behind" || fail "temp file left in the folder"

out=$(optimize "{encoder: 'auto', scale: {width: 1280, height: 720, mode: 'fit'}}")
[[ $out == *newSize* ]] && pass "downscale to 1280x720 (fit) ran" || fail "downscale failed: $out"
finish true
expect_video hevc,1280,720 "fit downscale gives 1280x720 H.265"

out=$(optimize "{encoder: 'auto', scale: {width: 640, height: 640, mode: 'fill'}}")
[[ $out == *newSize* ]] && pass "downscale to 640x640 (fill) ran" || fail "fill downscale failed: $out"
finish true
expect_video hevc,640,640 "fill downscale crops to 640x640 H.265"

out=$(optimize "{encoder: 'auto'}")
if [[ $out == *newSize* ]]; then fail "an H.265 file with nothing to change was converted again"
elif [[ $out == *"Nothing to do"* ]]; then pass "nothing to do on an H.265 file is refused"
else fail "an H.265 file with nothing to change was not refused the expected way: $out"; fi

out=$(optimize "{encoder: 'auto', scale: {width: 320, height: 180, mode: 'fit'}}")
if [[ $out == *newSize* ]] && [ "$(temps)" -ge 1 ]; then
    finish false
    expect_video hevc,640,640 "a discarded result leaves the file alone"
    [ "$(temps)" = 0 ] && pass "discard removes the temp file" || fail "discard left the temp file"
else
    fail "the 320x180 conversion to discard did not stage a result: $out"
fi

after=$(probe_info)
[ "$after" = '"hevc,640,640"' ] && pass "the app reads the new file: $after" || fail "the app still sees the old file: $after"

vdur() { sshv "$FFPROBE -v error -show_entries format=duration -of csv=p=0 $LIB/$ID/vid.mp4"; }
dur_near() {
    local got; got=$(vdur)
    awk -v g="$got" -v w="$1" -v t="$2" 'BEGIN { d = g - w; if (d < 0) d = -d; exit !(d <= t) }' \
        && pass "$3 (${got}s)" || fail "$3: wanted ${1}s, the file is ${got}s"
}
dur_near 3 0.3 "the converted file is still about 3 s long"

settings false none
copies=$(originals)
out=$(optimize "{encoder: 'auto', cut: {start: 0.5, end: 2.5}}")
[[ $out == *newSize* ]] && pass "cut 0.5-2.5 s (re-encoded) ran" || fail "cut failed: $out"
finish true
dur_near 2 0.4 "the cut file is about 2 s long"
expect_video hevc,640,640 "the cut keeps codec and size"

out=$(optimize "{encoder: 'auto', cut: {start: 0, end: 1, copy: true}}")
[[ $out == *newSize* ]] && pass "cut 0-1 s (stream copy) ran" || fail "copy cut failed: $out"
finish true
dur_near 1 0.6 "the stream-copy cut is about 1 s long"
expect_video hevc,640,640 "the stream-copy cut keeps codec and size"

[ "$(originals)" = "$copies" ] && pass "no copy of the original with the backup switched off" || fail "a copy was made although the backup is off"
settings true none

out=$(optimize "{encoder: 'auto', force: true}")
[[ $out == *newSize* ]] && pass "encoding an H.265 file again works with force" || fail "force re-encode failed: $out"
finish true
expect_video hevc,640,640 "the forced re-encode keeps codec and size"

ID2=900000002
gen_video "$ID2" quicktest || { fail "could not generate the quick optimize test video"; stop_app; exit 1; }
cdp 'window.electronAPI.library.scan()' >/dev/null
settings true 4k
res=$(cdp "window.electronAPI.videoTools.quick(['$ID2']).then(r => r.optimized + ',' + r.lines.length)")
[ "$res" = '"1,0"' ] && pass "quick optimize converted the video ($res)" || fail "quick optimize: wanted 1 optimized and no lines, got $res"
expect_video hevc,1920,1080 "a 1080p video stays 1080p with a 4K target" "$ID2"
[ "$(originals "$ID2")" -ge 1 ] && pass "quick optimize kept a copy of the original" || fail "quick optimize left no (original) copy"
res=$(cdp "window.electronAPI.videoTools.quick(['$ID2']).then(r => r.optimized + ',' + r.lines.map(l => l.kind + ':' + l.id).join(String.fromCharCode(124)))")
[ "$res" = '"0,skipped:'$ID2'"' ] && pass "an H.265 video is listed as skipped ($res)" || fail "quick optimize: the H.265 video should be skipped with a reason, got $res"
settings false 720p
res=$(cdp "window.electronAPI.videoTools.quick(['$ID2']).then(r => r.optimized + ',' + r.lines.length)")
[ "$res" = '"1,0"' ] && pass "quick optimize scales an H.265 video down ($res)" || fail "quick optimize with a 720p target: got $res"
expect_video hevc,1280,720 "quick optimize scaled to 1280x720" "$ID2"
settings true none

cdp "window.electronAPI.lwe.launch('$LIB/$ID', {screen: '$screen'})" >/dev/null; sleep 15
engine_cmd() { sshv 'pgrep -af "/[l]inux-wallpaperengine(\.bin)? " | head -1'; }
if [[ $(engine_cmd) == *"$LIB/$ID"* ]]; then
    sleep 10
    [[ $(engine_cmd) == *"$LIB/$ID"* ]] && pass "the engine plays the converted video" || fail "the engine quit on the converted video: $(sshv 'tail -3 ~/wem/app.log')"
else
    fail "the engine does not run the converted video: $(sshv 'tail -3 ~/wem/app.log')"
fi

stop_app
exit $status
