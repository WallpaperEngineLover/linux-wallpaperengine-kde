# wecompare - library inventory and comparison against the real Wallpaper Engine

Tools to scan a workshop library for features, render every wallpaper with this engine and with the real
Wallpaper Engine (2.8.x, Windows build under Wine + DXVK on an isolated display), and compare the two.

Data locations (all overridable):

| what | default | env |
|---|---|---|
| workshop items | `/workspace/SteamLibrary/steamapps/workshop/content/431960` | `LWE_WORKSHOP_DIR` |
| WE assets | `/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets` | `LWE_ASSETS_DIR` |
| feature database, results | `~/.local/share/lwe-library` | `LWE_LIBRARY_DIR` |
| Wine prefix + WE install copy | `~/.local/share/we_live` | `WE_LIVE_DIR` |
| WE reference frames | `~/.local/share/we_live/refs/<id>/f*.png` | `REFS` |

## Scripts

- `scan.py [ids]` - feature scan (reads scene.pkg in memory, well under a second for 100+ items). Writes
  `features.json` and `FEATURES.md` (feature -> item ids, rarest first) and prints new/removed items.
- `render_we.py [ids | scene paths]` - real WE reference frames, ~12s per item (~30s for 100+ object scenes):
  a fresh WE per item that is captured as soon as the first frame is rendered instead of after fixed sleeps.
  Skips items that already have frames unless `FORCE=1`. Scene dirs/files (test variants) go to
  `$REFS/<dir name>`. `FRAMES`, `INTERVAL`, `SETTLE`, `SWITCH_TIMEOUT` (per attempt), `RETRIES` (new instances while the window stays flat, video-texture scenes need that sometimes) tune the capture. Reusing one
  instance via `-control openWallpaper` doesn't work under Wine (the command only re-shows the UI window).
- `render_ours.sh <outdir> [ids]` - headless GPU render of this engine at the WE window size (1920x1058), cursor at WE's pointer position,
  frame 600 at 30 fps. `LWE=<dir>` picks another build (the dir needs its own copy of the lib .so).
- `compare.py <ours dir> <review dir> [ids]` - scores against the closest WE frame (they animate), writes
  `<id>.png` (WE | ours, diff heat below), 4-item `sheet_*.png` for skimming and `scores.json`
  (mean diff, area with big differences, WE frame-to-frame motion, global shift by phase correlation).
- `frames.py <id> <ours.png> <out.png>` - all WE frames plus ours in one grid, to tell animation from real
  differences.
- `crop.py <id> <ours.png> <out.png> x y w h [zoom]` - same region of both side by side.

`wine/` has the lower level real-WE helpers:

- `we_display.sh` - starts the isolated display :98 (headless weston + Xwayland in a private runtime dir).
- `we_full.sh <scene> <outdir> [count] [interval]` - single cold run with full-screen grabs.
- `we_cursor.sh <scene> <outdir> "x,y x,y" [settle]` - warps the pointer before each grab (parallax, cursor).
- `we_capture.sh <scene> [secs]` - RenderDoc capture of one WE frame (inspect with `tools/rdc_dump.py` on :98,
  `rdscripts/` has the replay scripts).
- `audio_sink.sh start|stop|env|record <wav> <secs>` - a private PulseAudio with one null sink (never the desktop's
  server) and a recorder on its monitor. `WE_AUDIO_REC=<wav> we_full.sh ...` plays live WE into it, and
  `HEADLESS_RENDER_PULSE_SERVER` does the same for `tools/headless_render.sh`; `../wavstat.py` prints levels. Align
  recordings by onset, not wall clock.
- `WE_WINE_DIR=<wine build root> WE_PREFIX=<prefix>` run WE on another Wine (e.g. WineHQ 11.0 unpacked into
  `~/.local/share/wine11`, prefix `~/.local/share/we_live/wineprefix11` with upstream DXVK 3.1.1 PE DLLs and
  mscoree/mshtml disabled), `WE_WINEDEBUG` sets WINEDEBUG.
- `gdb_video_hr.py` - HRESULTs of WE's video texture upload (keyed mutex + TransferVideoFrame) on a live WE.
- `repack_pkg.py in.pkg out.pkg name=file ...` - replaces files inside a scene.pkg (test variants of scenes
  whose texture names Wine can't open in folder mode).

## Regression pass after engine changes

    ./render_ours.sh /tmp/ours [ids] && ./compare.py /tmp/ours /tmp/review [ids]

WE refs only need re-rendering when the item was updated on the workshop (`mtime` in features.json). Pick
targets by feature from `FEATURES.md`.

## Setting up the WE side (once per `WE_LIVE_DIR`)

    mkdir -p $WE_LIVE_DIR/we28 && cd $WE_LIVE_DIR/we28
    cp <wallpaper_engine>/distribution/wallpaper64.exe <wallpaper_engine>/distribution/version.json .
    ln -s <wallpaper_engine>/distribution/bin bin; ln -s <wallpaper_engine>/assets assets
    ln -s <wallpaper_engine>/distribution/plugins plugins
    bash wine/we_display.sh
    WINEPREFIX=$WE_LIVE_DIR/wineprefix DISPLAY=:98 wine64 wineboot --init
    WINEPREFIX=$WE_LIVE_DIR/wineprefix dxvk-setup install -s -y

For RenderDoc: `mkdir -p $WE_LIVE_DIR/vklayer; cp wine/renderdoc_capture.json $WE_LIVE_DIR/vklayer/;
cp wine/rd_trigger.py $WE_LIVE_DIR/`, and `cp wine/qrenderdoc_analytics.json ~/.local/share/qrenderdoc/analytics.json`
(the first-run analytics dialog blocks the trigger script otherwise). Set `"postprocessing": "enabled"` in
`we28/config.json` (WE's default; disabled turns bloom off and shows up as false differences).

## Pitfalls

- Audio: if the desktop's audio server socket is reachable (e.g. a shared `/run/user/<uid>/pulse`), Wine and
  this engine both connect to it. WE then plays wallpaper sounds on real speakers and both sides pick up real
  system audio, so audio-reactive layers differ between runs. All scripts point `PULSE_SERVER` /
  `PIPEWIRE_REMOTE` at dead sockets; keep that in anything new (verify with `strace -f -e trace=connect`).
- Never use :0 for WE. DXVK crashes on Xvfb (0 Hz mode), hence weston + Xwayland on :98.
- Scores are only a triage hint: scroll/shake/particles, wall-clock texts and heavily downscaled 8K scenes score
  high while matching. Check `frames.py` before calling something different.
- Web wallpapers can't be compared (WE's CEF under Wine renders a white page, this engine renders black
  headless). The video wallpaper type is black in WE under Wine (video textures inside scenes do play).
- Texts using `systemfont_*` aren't drawn by WE under Wine (the prefix has no Windows fonts).
- Folder scenes with CJK texture names fail to load textures in WE under Wine: pass scene.pkg (or repack).
- Crop: WE window = `[30:1080, 4:1920]` of the :98 screen, ours `[0:1050, 0:1916]`.
