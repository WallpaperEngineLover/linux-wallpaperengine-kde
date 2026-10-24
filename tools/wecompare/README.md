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
- `ours_window.sh <scene dir> <seconds> [engine args]` - this engine as a 1920x1080 window on :98, isolated like
  WE, for timing grabs off the same X server as live WE. Sets `vblank_mode=0`: Xwayland on the headless weston
  presents a vsynced GLX window at 1 Hz. Its first present takes ~0.7 s there (20 ms headless), so the first frame
  is late.
- `we_full.sh <scene> <outdir> [count] [interval]` - single cold run with full-screen grabs.
  `WE_DESKTOP=1` runs WE in desktop mode (wallpaper of monitor 0, fullscreen, no window frame) instead of
  `-playInWindow`: Wine has no Progman/WorkerW shell windows, `wedesktop.c` (built on first use with
  `x86_64-w64-mingw32-gcc`, or `python3 -m ziglang cc` when that is missing) provides the Windows 10 layout WE looks
  for. `WE_UNLOAD=1` opens another wallpaper (`WE_UNLOAD_SCENE`) before stopping, so WE unloads the scene and writes
  its script localStorage to `bin/scenestorage`; give it a `WE_LIVE_DIR` whose `we28/bin` is a real folder of links,
  not a link into the Steam install. `WE_WAIT=<seconds>` (default 25) is the wait before the first grab.
- `span_display.sh start <WxH+X+Y>... | layout ... | stop | env` - a private multi monitor display: headless sway with
  one output per argument at that position (60 Hz) and its own Xwayland as window manager, so Wine sees one RandR
  monitor per output. sway runs under proot with a private `/tmp` (the shared X socket directory is read-only, and
  lock files for :0-:9 keep it off the desktop's display number); everything lives in `$WE_SPAN_DIR`
  (`~/.local/share/we_live/span`), which also holds a WE install copy and a Wine prefix with `"Managed"="N"` (WE's
  windows land where WE puts them).
- `we_span.sh <scene> <outdir> [count] [interval]` - live WE in desktop mode on those monitors (`WE_SPAN_MONITORS`),
  `WE_SPAN_LAYOUT` 0/1/2 = per monitor / stretch over all / clone, `WE_SPAN_GROUP="Monitor0,Monitor1"` a stretch group,
  `WE_SPAN_CURSOR` pointer positions (XTest warps, Wine reads the X pointer); grim grabs of the whole layout. It
  writes `wallpaperconfig` and starts WE without `-control` (openWallpaper rewrites the config without the layout),
  and turns `safemode` off: a killed WE leaves `faultyShutdown` set and the next start would run `-nowallpapers`.
- `we_pause.sh <scene> <outdir> [seconds]` / `ours_pause.py <scene> <outdir>` - a global pause and resume (6 s and 12 s) under live WE
  (desktop mode, `-control pause`; `-playInWindow` ignores it) and under this engine (mocked UPower battery with
  `--fps-battery 0`), frame numbers of the videotex kit's `scene_pause` decoded into `frames.txt`
- `ours_span.sh <scene> <outdir> [count] [interval] [engine args]` - this engine on the same outputs
  (`OURS_SPAN_SCREENS` = the `--screen-span` list, `none` for one `--screen-root` per output), pointer through a wlr
  virtual pointer. `../spancmp.py <we dir> <ours dir> [WxH+X+Y...]` compares the grabs per output.
- `we_resize.sh <scene> <outdir> <WxH>...` / `ours_resize.sh` - `-playInWindow` (ours: `--window` on the same
  Xwayland) started at the first size and resized to the others: WE through `wewinsize.exe` (SetWindowPos inside
  Wine, WE's own WM_SIZE path), ours by an X configure. `WE_RESIZE_FRESH`/`OURS_RESIZE_FRESH` start fresh per size,
  `RESIZE_POINTER="dx,dy"` puts the pointer at the same client position in both.
- `we_cursor.sh <scene> <outdir> "x,y x,y" [settle]` - warps the pointer before each grab (parallax, cursor).
- `we_capture.sh <scene> [secs]` - RenderDoc capture of one WE frame (inspect with `tools/rdc_dump.py` on :98,
  `rdscripts/` has the replay scripts).
- `audio_sink.sh start|stop|env|record <wav> <secs>` - a private PulseAudio (or, where pulseaudio isn't installed, a private PipeWire + pipewire-pulse + WirePlumber) with one null sink (never the desktop's
  server) and a recorder on its monitor. `WE_AUDIO_REC=<wav> we_full.sh ...` plays live WE into it, and
  `HEADLESS_RENDER_PULSE_SERVER` does the same for `tools/headless_render.sh`; `../wavstat.py` prints levels. Align
  recordings by onset, not wall clock. `headless_render.sh` refuses a private server that is also the caller's
  `PULSE_SERVER`, so run it with `env -u PULSE_SERVER`. `WE_HRTF=1 we_full.sh ...` puts WE's sound objects on the
  headphones (HRTF) path: an `alsoft.ini` with `stereo-mode = headphones` next to `wallpaper64.exe`, which OpenAL Soft
  1.21.1 reads from the exe's folder; without the variable the file is removed again. Compare with this engine's
  `--hrtf on`. A sink left as the private server's default by another test (e.g. one with a headphone form factor)
  takes WE's audio away from `lwe_null`: `pactl set-default-sink lwe_null` first.
- `WE_WINE_DIR=<wine build root> WE_PREFIX=<prefix>` run WE on another Wine (e.g. WineHQ 11.0 unpacked into
  `~/.local/share/wine11`, prefix `~/.local/share/we_live/wineprefix11` with upstream DXVK 3.1.1 PE DLLs and
  mscoree/mshtml disabled), `WE_WINEDEBUG` sets WINEDEBUG.
- `gdb_video_hr.py` - HRESULTs of WE's video texture upload (keyed mutex + TransferVideoFrame) on a live WE.
- `gdb_video_state.py` - the video player's play gate (play requested, last bind vs scene frame counter), the
  OnVideoStreamTick result and each texture bind / TransferVideoFrame call, logged when they change.
- `gdb_video_texdesc.py` - the video texture's `D3D11_TEXTURE2D_DESC` and the shared handle the player opens; attach
  before the wallpaper loads (WE loads in about a second).
- `sharedvideo/run.sh <video> [seconds]` - builds and runs `sharedvideo.exe`, WE's video texture path rebuilt as a
  small D3D11 program (keyed-mutex texture shared between a render and a player device, media engine
  TransferVideoFrame, readbacks on both sides; `SV_WE=1` WE's byte stream load, `SV_THREAD=1` its tick/transfer
  thread split, `SV_RFLAGS`/`SV_FORMAT` device flags and texture format). `sharedvideo.exe --open <handle>` reads a
  live WE's video texture from a second process in the same prefix. Needs the MinGW compiler: Fedora's mingw64-gcc,
  -gcc-c++, -headers, -binutils and -cpp rpms unpacked into `~/.local/share/mingw-root` (`dnf download`, `rpm2cpio`,
  no root), `MINGW_CXX` overrides.
- Video textures under Wine 11.0 stop after one pass: `SetLoop` is a stub, the ENDED event makes WE drop play and
  Wine's fallback transfer then clears the texture to transparent black. Test with a video longer than the capture.
- `repack_pkg.py in.pkg out.pkg name=file ...` - replaces files inside a scene.pkg (test variants of scenes
  whose texture names Wine can't open in folder mode).

## Proton

`wine/proton.sh` installs Proton builds next to the system Wine and gives every WE script a third backend:
`WE_PROTON=<name>` on `render_we.py` (frames go to `$WE_LIVE_DIR/refs_proton/<name>`), `we_full.sh`, `we_cursor.sh`,
`we_capture.sh`, `we_resize.sh`, `we_span.sh` and `sharedvideo/run.sh`; `eval "$(wine/proton.sh env <name>)"` sets
`WE_WINE_DIR`/`WE_PREFIX` for anything else (e.g. the `videotex` kit's `we_video.sh`).

    wine/proton.sh available                       # GE-Proton and proton-cachyos releases, Valve branches
    wine/proton.sh install GE-Proton11-9           # GitHub tarball, sha512 checked, into ~/.local/share/proton
    wine/proton.sh install proton-experimental     # Valve, from Steam's CDN
    WE_PROTON=GE-Proton11-9 ./render_we.py 1367456719

- The shim (`$PROTON_DIR/<name>/shim/bin/wine`) runs Proton's own `run` path through `proton_run.py`, which only
  swaps the last step (starting the program through steam.exe) for a plain `wine <program>`. `runinprefix` is not
  enough: Proton adds its DXVK/vkd3d-proton dll overrides in the prefix setup that only `run` does, so WE silently
  ran on wined3d. Valve's steam.exe also asserts without a Steam client. Each prefix is
  `$WE_LIVE_DIR/proton/<name>/pfx`, `SteamAppId` is 431960, xalia is off. Check `wallpaper64_d3d11.log` in
  `$WE_LIVE_DIR` for the DXVK version Proton brought (GE 11-9: 3.1.1-75).
- No Steam Linux Runtime container: pressure-vessel needs user namespaces, which Docker doesn't give, so Proton runs on
  the image's libraries (the Mesa/RADV a Steam user's container imports anyway). Proton's wine starts every program
  through the i386 preloader, so the image needs `glibc.i686` and the 32-bit libraries in the Dockerfile; without them
  `wine/proton.sh i686` unpacks a private glibc and the shim runs Proton under proot (slower, no timing work).
- Valve builds come from Steam with DepotDownloader (`wine/proton_valve.sh`, tools in `~/.local/share/proton/tools`).
  Anonymous logins only get Proton Experimental, `STEAM_USER=<account>` the numbered branches (asks for the password
  and Steam Guard code, run it in a terminal). DepotDownloader writes symlinks as empty files and leaves their targets
  encrypted, `steam_manifest_links.py` restores them (1896 in Experimental) with the depot key, plus the executable bits
  and `steampipe_fixups.json`. steamcmd refuses anonymous Proton downloads ("No subscription").

Compared with system Wine 11.0 + DXVK 3.1.1 (2026-10-15, GE-Proton11-9, proton-cachyos 11.0-20261005, Valve
experimental-11.0-20261008):

| case | Wine 11.0 | GE | CachyOS | Valve experimental |
|---|---|---|---|---|
| scenes (1111948150, 2488408736) | ok | same frames | same | same |
| `systemfont_*` texts (2431040693's clock) | not drawn | drawn | drawn | drawn |
| video wallpaper (1367456719) | black | plays | black | Proton's color bar placeholder |
| video textures in scenes (`tests/videotex`) | one pass, then cleared | WE crashes | WE crashes | WE crashes |
| web wallpapers | white page | Chromium browser window | same | same |

Video textures: under every Proton build the source resolver hands WE's wrapper media source a null inner source
("Unimplemented flags 0x11", Valve: "Unsupported demuxer"), and WE dereferences it in
`CreatePresentationDescriptor` (`sub_1400F1780`, crash at `wallpaper64.exe+0xF178D`); `PROTON_MEDIA_FORCE_GST=1`
(CachyOS) doesn't change it. `media_engine_SetLoop` is a stub in all of them too, so the loop seam still needs the
Windows VM. So: GE-Proton for video wallpapers and texts with system fonts, plain Wine for video textures.

## Windows VM (`winvm/`)

For what only real Windows shows (Media Foundation's video loop, anything Wine emulates instead of running). Needs
`/dev/kvm` in the container (`docker-compose.yml` passes it, `KVM_GID` is the host's kvm group) and QEMU, OVMF, swtpm,
xorriso and smbd (Dockerfile). Everything lives in `~/.local/share/winvm` (the `wine-we` volume): disk, TPM state,
firmware variables, the ISO, and `share/`, which Windows sees as `\\10.0.2.4\qemu`.

- Windows 11 IoT Enterprise LTSC evaluation ISO from Microsoft's Evaluation Center into `iso/`, then
  `winvm.sh install` (unattended, `autounattend.xml`: local account `we`/`we`, auto logon, `firstlogon.ps1` turns off
  sleep, lock screen, UAC and updates). The OOBE region/keyboard pages may still need `winvm.sh keys ret` three times.
- The evaluation has to be activated online once or it reports "expired" and shuts down every hour:
  `WINVM_RESTRICT=off winvm.sh start`, `winvm.sh run -c 'cscript //nologo C:\Windows\System32\slmgr.vbs /ato'`, then
  boot it offline again (90 days). Otherwise the network is `restrict=on`: nothing but the SMB share.
- `winvm.sh bootstrap` once: types the agent installer into Win+R. The agent (`agent.ps1`, a logon task) runs every
  script `winvm.sh run <file.ps1 | -c '...'>` drops into the share and returns its output. GUI programs go through
  `winvm.sh launch <exe> [args]` (a one-off scheduled task; a program started from a job inherits its output handles
  and the agent would wait for it).
- `winvm.sh shot <png>` / `keys` / `type` go through QMP, the display is never on a real one. No audio device.
- Live WE: copy `we_live/we28` dereferenced into `share/` and on to `C:\winvm\we28` (robocopy), start it with
  `launch 'C:\winvm\we28\wallpaper64.exe' '-control openWallpaper -file <scene.json> -monitor 0 -playInWindow -silent'`,
  then `run we_window.ps1` hides the "Wallpaper UI" pop-up (it covers the middle of the window) and prints the client
  rectangle. D3D11 is WARP (Microsoft Basic Display Adapter, feature level 12_1), so heavy scenes are slow.
- `grab_frames.py <qmp.sock> <out> <seconds> <x y w h>` decodes the `videotex` kit's frame number from screendumps (~85
  per second), `../videoseam.py` turns such grabs (or `grab_ours.py`'s) into loop seam statistics.
- `sharedvideo.exe` (built by `wine/sharedvideo/run.sh`'s compiler line) never sees the media engine load on WARP,
  WE itself plays fine there.

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
    WINEPREFIX=$WE_LIVE_DIR/wineprefix DISPLAY=:98 wine wineboot --init
    # DXVK dlls into the prefix (Debian: `dxvk-setup install -s -y`); copy, never symlink, WE_WINED3D=1 needs wined3d
    for d in d3d11 dxgi; do
        cp /usr/lib64/wine-wow64/wine/x86_64-windows/dxvk-$d.dll $WE_LIVE_DIR/wineprefix/drive_c/windows/system32/$d.dll
    done

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
  headless). The video wallpaper type is black in WE under Wine (video textures inside scenes do play), use
  `WE_PROTON=GE-Proton11-9` (see Proton) or the Windows VM.
- Texts using `systemfont_*` aren't drawn by WE under Wine (the prefix has no Windows fonts); Proton prefixes have
  its font set and draw them.
- Folder scenes with CJK texture names fail to load textures in WE under Wine: pass scene.pkg (or repack).
- Crop: WE window = `[30:1080, 4:1920]` of the :98 screen, ours `[0:1050, 0:1916]`.
