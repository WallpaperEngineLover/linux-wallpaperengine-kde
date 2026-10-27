# linux-wallpaperengine-kde

A fork of [Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine). It plays Wallpaper Engine (Steam app 431960) live wallpapers on Linux: scene/parallax backgrounds, video (via mpv), and web/HTML backgrounds (via CEF), rendered with a from-scratch OpenGL reimplementation of Wallpaper Engine's renderer.

It started with KDE, but meanwhile supports most desktop environments, on Wayland and X11.

The engine itself is a command-line tool driven entirely by flags, see Usage below. For a GUI (Workshop browsing, library, playlists, per-monitor wallpapers, backups) use [WE Manager](https://github.com/WallpaperEngineLover/we_manager), which also downloads and updates the engine for you.

## Identical to Wallpaper Engine

The goal of this fork is that a wallpaper looks and behaves here like it does in Wallpaper Engine 2.8. Where the engine has
a counterpart in `wallpaper64.exe` (2.8.0.42), the code is a port of that function and not a guess, and the results were
compared with the real Wallpaper Engine running under Wine (and on a real Windows VM) on close to 300 Workshop wallpapers.
These parts follow Wallpaper Engine one to one:

Scene layout and camera:

- Draw order is the `scene.json` array order, groups and locators only nest (a child shows only while every group above it shows), layers under a compose or passthrough layer are drawn into its buffer.
- Object transforms (parent chain, Euler order, alignment anchors, auto size, crop offset), the quad a layer draws and its half texel inset.
- Scaling modes (default fill, fit, stretch, center), alignment, zoom and offset, and rendering at each monitor's own pixel size like Wallpaper Engine's per monitor DPI awareness.
- Camera objects and camera paths, camera shake, camera fade, zoom, the scene camera that scripts can read and set, perspective cameras (3D scenes and perspective layers), fullscreen layers, fog by distance and height.
- Mouse parallax (formula, smoothing, per layer depth, wallpaper position), including that it only exists in orthographic scenes, and the cursor as Wallpaper Engine hands it to scenes, scripts and shaders (per monitor, unclamped).

Rendering:

- Effects: pass and buffer setup, buffer sizes and sharing, conditions, blend and depth state rules (translucent and additive never write depth, `normal` replaces, alpha only written into layer buffers), the composite buffers of layers, the 6x6 yellow and black checker for textures it cannot load.
- Shaders: Wallpaper Engine's shader dialect and its `#require` modules, combos and their defaults, constants, material overrides and uniform layout, translated to GLSL, with the texture slot, format and mask component combos it sets.
- Textures: `.tex` containers and formats (TEXV 4 and 5, TEXI, TEXB 3 and 4 with their conditions, sprite sheets, LUT volumes, video payloads) and how each is uploaded.
- Post processing: LDR and HDR bloom with Wallpaper Engine's own `util` materials, the combine passes (including its HDR scRGB output), color options, image filters, LUTs, flip, and the camera fade, in its order.
- Lighting: the old four slot point lights, the LightingV1 modules (point, spot, tube, directional) with cookies and shadow maps, volumetric light, planar reflections and the mip mapped frame buffer.
- Particles: emitters, initializers, operators and forces of 2.7 and 2.8 (boids, collisions, remap, turbulence, vortex, control point attraction, ...), child systems and their inheritance, control points, prewarm, ropes and trails, sprite sheets, orientation and culling, emitters that read an image layer.
- Text: sizing (`pointsize * 300 / 72`), HarfBuzz shaping with Wallpaper Engine's font fallback list, MSDF outline, blur and shadow, wrapping and ellipsis, anchors and backgrounds, colour glyphs, blend modes and effects on text.
- 3D models: skinning and morphs on the GPU, root motion, frustum culling and shadow casting.
- Puppets (2D characters): `.mdl` parsing, animation layers with their blending and fades, additive layers, bone physics, IK and rope chains, constraint blend rules, bone alpha, morph targets, clipping masks, hit boxes, draw order tracks, and the script bone API.
- Video wallpapers and video textures: when a texture video plays, pauses and loops, and the `IVideoTexture` object of scripts.
- Depth: scenes draw with a reversed 0 to 1 depth range like Wallpaper Engine's float depth buffer (needs `ARB_clip_control` or GL 4.5, `LWE_FORWARD_DEPTH=1` switches it off), and the jitter of volumetric light matches Wallpaper Engine's bit for bit on most pixels.

Sound:

- Sound objects: play modes (single, random, loop) and their timers, gain, spatialization, distance model, and what a volume change starts or stops.
- The sound mixer: OpenAL Soft 1.21.1 semantics (update size, per update gain ramps, the stop and pause fade, linear resampling) and its HRTF renderer for headphones, bit identical to OpenAL Soft and within 1e-7 of Wallpaper Engine's output.
- The audio spectrum that visualizers read: the same capture, FFT, band folding and smoothing, and the raw 128 band feed web wallpapers get.

SceneScript and properties:

- Scripts run on V8 with Wallpaper Engine's own `baseclasses.js` and `jsmodules`, so `Vec3`, `WEMath` and friends behave like the originals, and every script API object (layers, effects and materials, animations and animation layers, text, particle instances, model data, camera, `engine`, `input`, `thisScene`) has the members of the real thing in the same order.
- Script semantics: `init()` timing, `applyUserProperties()` with the same first call and keys, angles in degrees, timers, cursor events with propagation, hit boxes and local positions, `createLayer` and `destroyLayer`, `localStorage` in the same file format and location logic.
- User properties apply live without a reload, and scripts, animations and bindings on scene settings, object properties and effect constants work like Wallpaper Engine's.
- Wallpaper files with values of the wrong type are read the way Wallpaper Engine's JSON loader reads them where it ignores them (the default applies), and where it would abort the engine just doesn't crash.
- Presets and wallpapers that depend on another Workshop item.

Desktop behavior:

- Pause rules: fullscreen, maximized and focused windows pause per monitor by Wallpaper Engine's rules (minimized and other desktop windows do not count), with its defaults.
- Frame pacing: the FPS limit shares a one second budget between the frames of that second like Wallpaper Engine's main loop (`--frame-limiter fixed` brings back a fixed sleep per frame).
- Playlists: with `videosequence` a video plays once and its end advances the playlist, `playintro` plays the first item once at startup, and an item that fails to load is skipped for that round only.
- Stretch (span) groups: one wallpaper over the bounding box of several monitors, cursor included, pixel identical to Wallpaper Engine's stretch layout.
- Web wallpapers run on CEF 156 with its property, audio and file API.

Everything not listed here, or listed under Limitations, is either specific to this fork (see below) or not a copy yet.

## What this fork adds

KDE Plasma and Wayland:

- On Plasma the wallpaper is a real desktop surface (`plasma-shell` protocol): not in the taskbar or task switcher, unaffected by Show Desktop.
- Mouse-reactive wallpapers work on KDE Wayland, the cursor position comes from KWin since the desktop never gets pointer events there.
- Fullscreen-pause detection through KWin, since it doesn't implement `wlr-foreign-toplevel-management` like wlroots compositors.
- Native Wayland output with fractional scaling, GNOME Shell and COSMIC support, wlroots and Hyprland per output pause, and an X11 output that handles several screens and desktops that cover the root window.
- HDR output (`--hdr`) through the Wayland color management protocol, HDR videos included.
- Outside Plasma the KDE parts switch themselves off, and there is a build without them (`-DDISABLE_KDE_FEATURES=ON`).

Control:

- Multi-monitor handling: per-screen backgrounds (`--screen-root`), one wallpaper spanning several monitors (`--screen-span`), several wallpapers side by side on one monitor (`--screen-slot`), per-screen scaling/zoom/alignment/corner color, and Workshop playlists (`--playlist`).
- Animated transitions when the wallpaper changes (playlist or hotswap): fade, slide, wipe or zoom, `--transition` and `--transition-duration`.
- Live hotswap of the running wallpaper and its settings via `SIGUSR1`, separately per engine with `--control-file`, no process restart.
- A global playback speed multiplier (`--speed`), separate from the FPS cap; battery and idle pause (`--fps-battery`, `--idle-pause`).
- Video wallpapers can loop just part of the video (`--video-start`/`--video-end`), or several parts one after the other (`--video-segments "2:00-3:00,4:00-5:00"`).
- More granular audio: restrict sound to a single screen (`--audio-screen`), a separate ambient volume for non-video backgrounds (`--ambient-volume`), per-object sound volume (`--sound-volume`), and a tunable multiplier on audio-reactive properties (`--audio-sensitivity`, with `--list-audio-objects` to see what's wired up).
- RGB lighting like Wallpaper Engine's LED plugin (`--leds`): the scene, or the layers marked as LED source, shrunk to 80 x 10 and sent to every device of an OpenRGB server.
- Layer and effect introspection and toggling (`--list-objects`, `--disable-object`, `--list-effects`, `--disable-effect`, ...) for things the wallpaper author didn't expose as a property.
- Supersampling (`--ssaa`), an expanded canvas for oversize scenes (`--expand-canvas`), `--repair-assets` for wallpapers that only worked in older Wallpaper Engine versions.
- Web wallpapers in a separate, disposable process with GPU frame transfer through dmabuf (`--web-frames`), SOG depth-photo wallpapers drawn natively.
- Protections against runaway scripts (time, heap and buffer limits) and limits on every file reader, which Wallpaper Engine does not have.

## Requirements

- OpenGL 3.3
- CMake
- LZ4, Zlib
- SDL2
- FFmpeg
- X11 or Wayland (Xrandr on X11; wayland-client, wayland-protocols and EGL for Wayland)
- GLFW3, GLEW, GLUT, GLM
- MPV
- PulseAudio
- FFTW3
- FreeType, HarfBuzz
- D-Bus (for the KDE integration, see Build)
- V8 10.2 or newer, as shipped in Node.js's shared library (`libnode`): scene scripts run on it like in Wallpaper Engine. CMake looks for `v8.h` under `include/node` and `libnode`; point `-DV8_INCLUDE_DIR`/`-DV8_LIBRARY` at another V8 build if yours lives elsewhere

### Ubuntu 22.04
Its `libnode-dev` (Node.js 12) is too old for the scripting and its FFmpeg 4.4 too old for the audio code, use the portable build (see Installing) or 24.04.
```bash
sudo apt-get update
sudo apt-get install build-essential cmake libxrandr-dev libxinerama-dev libxcomposite-dev libxdamage-dev libxcursor-dev libxi-dev libgl-dev libglew-dev freeglut3-dev libsdl2-dev liblz4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libxxf86vm-dev libglm-dev libglfw3-dev libmpv-dev mpv libmpv1 libpulse-dev libpulse0 libfftw3-dev libfreetype-dev libharfbuzz-dev libdbus-1-dev libwayland-dev wayland-protocols libegl1-mesa-dev
```

### Ubuntu 24.04
```bash
sudo apt-get update
sudo apt-get install build-essential cmake libxrandr-dev libxinerama-dev libxcomposite-dev libxdamage-dev libxcursor-dev libxi-dev libgl-dev libglew-dev freeglut3-dev libsdl2-dev liblz4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libxxf86vm-dev libglm-dev libglfw3-dev libmpv-dev mpv libmpv2 libpulse-dev libpulse0 libfftw3-dev libfreetype-dev libharfbuzz-dev libdbus-1-dev libwayland-dev wayland-protocols libegl1-mesa-dev libnode-dev
```

### Fedora 44
```bash
sudo dnf install gcc g++ cmake pkg-config libXrandr-devel libXinerama-devel libXcomposite-devel libXdamage-devel libXcursor-devel libXi-devel mesa-libGL-devel mesa-libEGL-devel glew-devel freeglut-devel SDL2-devel lz4-devel ffmpeg-free-devel libXxf86vm-devel glm-devel glfw-devel mpv-devel pulseaudio-libs-devel fftw-devel freetype-devel harfbuzz-devel gmp-devel dbus-devel wayland-devel wayland-protocols-devel nodejs24-devel
```
If you already have Node.js 22, use `nodejs22-devel` instead, Fedora's nodejs -devel packages can't be installed side by side. `ffmpeg` and `ffmpeg-free` conflict, with RPM Fusion's full ffmpeg install its `ffmpeg-devel` instead.

### Arch Linux
```bash
sudo pacman -S --needed git base-devel cmake pkg-config glew freeglut sdl2 lz4 ffmpeg glm glfw mpv libpulse fftw freetype2 harfbuzz dbus libxrandr libxinerama libxcomposite libxdamage libxcursor libxi libxxf86vm wayland wayland-protocols mesa gmp nss at-spi2-core libcups libxcomposite libxdamage libxkbcommon pango cairo alsa-lib libdrm libxshmfence
```
The second half of that list is what the bundled CEF links against, a desktop install usually has it already. Arch's `nodejs` package has the headers but no `libnode`, the AUR's `libnode` package (a Node.js built with `--shared`) provides it, e.g. `paru -S libnode`.

### ALT Linux
```bash
sudo epm update
sudo epm install gcc-c++ make cmake libXrandr-devel libXinerama-devel libXcomposite-devel libXdamage-devel libXcursor-devel libXi-devel libGL-devel libGLEW-devel freeglut-devel libSDL2-devel liblz4-devel libavcodec-devel libavformat-devel libavutil-devel libswscale-devel libXxf86vm-devel libglm-devel libglfw3-devel libmpv-devel mpv libpulseaudio-devel libpulseaudio libfftw3-devel libpng-devel libffi-devel libswresample-devel libgmpxx-devel libfreetype-devel libharfbuzz-devel libdbus-devel
```

## Installing

Tagged releases on the [releases page](https://github.com/WallpaperEngineLover/linux-wallpaperengine-kde/releases) have ready-to-run builds, each with (`kde`) and without (`generic`) the KDE Plasma integration:

- **Portable** (`linux-wallpaperengine-kde-portable-x86_64.tar.gz`): runs on any x86_64 distribution with glibc 2.35 or newer (Ubuntu 22.04 and newer, Debian 12+, Fedora, openSUSE, Arch, SteamOS, Bazzite, Silverblue/Kinoite and other image-based systems) without installing anything: it carries FFmpeg, mpv, V8 and the rest, and only uses what every desktop has (Mesa, the display server's and audio server's libraries, the GTK stack CEF needs). A few libraries it brings are only used where the system has no library of that name (`lib/fallback`), the `linux-wallpaperengine` script in the folder takes care of that.
- **Distribution builds** for Ubuntu 24.04, Fedora 44 and Arch Linux use the distribution's own FFmpeg, mpv and so on: install the runtime packages from the list above (the non `-dev`/`-devel` ones). The Arch build also needs `libnode`, from the AUR or prebuilt from the `libnode-arch` release (see Limitations).

```bash
tar xzf linux-wallpaperengine-kde-portable-x86_64.tar.gz
./linux-wallpaperengine-kde-portable-x86_64/linux-wallpaperengine --help
```

The folder can live anywhere. Web wallpapers run in Chromium's sandbox when the system allows unprivileged user namespaces or `chrome-sandbox` is setuid root (`sudo chown root:root chrome-sandbox && sudo chmod 4755 chrome-sandbox`), otherwise without it (Ubuntu 24.04+ restricts user namespaces, a tarball in the home folder can't have a setuid file).

Other ways to get it:

- **AUR** (coming soon, AUR registration is paused right now): `linux-wallpaperengine-kde-bin` (the portable build in `/opt`) and `linux-wallpaperengine-kde-git` (built from this repository, needs the AUR's `libnode`). Until then use the portable build.
- **Flatpak**: [WE Manager](https://github.com/WallpaperEngineLover/we_manager)'s Flatpak comes with the engine.
- **Nix**: `nix run github:WallpaperEngineLover/linux-wallpaperengine-kde -- --help` builds it from source against nixpkgs (the first build also compiles Node.js as a shared library for V8) (`packages.x86_64-linux.generic` without the KDE integration, `overlays.default` adds `linux-wallpaperengine-kde`).

## Tested distributions

Every release is tried in a virtual machine per row below (KVM, a real desktop session, started from a clean disk): the
engine with a scene, a particle scene, a video, a preset and web wallpapers, hotswap, pause behind fullscreen windows,
cleanup after a crash, and WE Manager's install, update and autostart. Several rows also check two monitors, minimizing a
fullscreen window and automute.

- **Supported**: passes the whole run.
- **Work in progress**: a check fails, or the distribution has not been brought through the run yet.
- **Not supported**: not tested, see the note.

| Distribution | Desktop (session) | Status | Notes |
|--------------|-------------------|--------|-------|
| Fedora 44 | KDE Plasma (Wayland) | Supported | |
| Fedora 44 | GNOME (Wayland) | Supported | |
| Ubuntu 24.04 | GNOME (Wayland) | Supported | |
| Debian 13 | GNOME (Wayland) | Supported | |
| Debian 13 | XFCE (X11) | Supported | |
| openSUSE Tumbleweed | KDE Plasma (Wayland) | Supported | |
| openSUSE Leap 16 | KDE Plasma (Wayland) | Supported | |
| openSUSE Tumbleweed | XFCE (X11) | Supported | |
| Arch Linux | Hyprland (Wayland) | Supported | |
| Arch Linux | XFCE (X11) | Supported | |
| Manjaro | KDE Plasma (Wayland) | Supported | |
| Manjaro | XFCE (X11) | Supported | |
| CachyOS | KDE Plasma (Wayland) | Supported | |
| CachyOS | Hyprland (Wayland) | Supported | |
| Garuda Linux | KDE Plasma (Wayland) | Supported | |
| EndeavourOS | KDE Plasma (Wayland) | Supported | |
| EndeavourOS | XFCE (X11) | Supported | |
| Linux Mint 22.2 | Cinnamon (X11) | Supported | |
| MX Linux 25.3 | XFCE (X11) | Supported | |
| Zorin OS | GNOME (Wayland and Xorg) | Supported | |
| Pop!_OS | COSMIC | Supported | |
| Bazzite | KDE Plasma (Wayland), image based | Supported | portable build or Flatpak |
| SteamOS | KDE Plasma (Wayland), read-only root | Supported | system libraries cannot be installed on the read-only root, so that step is not part of the test |
| Steam Deck (SteamOS on Deck hardware) | KDE Plasma (Wayland), read-only root | Supported | tested in a VM shaped like the Deck (1280x800, fake battery), system libraries cannot be installed on the read-only root, so that step is not part of the test |
| elementary OS | Pantheon (Wayland) | Supported | Gala has no `wlr-layer-shell`, the engine draws through XWayland and the wallpaper takes the clicks on the desktop. Fullscreen windows pause it (X11 ones, native Wayland ones through Gala's D-Bus window list), maximized native ones don't |
| ChimeraOS | gamescope, GNOME | Not supported | not tested: its Mesa cannot draw in a VM, and gamescope's game session has no desktop to put a wallpaper on. The GNOME desktop mode could theoretically work |

Other distributions and desktops that are not listed (other Ubuntu flavours, ...; sway, niri, KDE on X11 and more are covered by the headless desktop lab, not by real sessions) may well work,
they are just not part of the release test.

## Build

```bash
git clone --recurse-submodules <this repo's url>
cd linux-wallpaperengine-kde
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE='Release' ..
make
```

The binary and support files end up in `build/output`.

The KDE Plasma integration (plasma-shell desktop surfaces, cursor tracking through KWin, fullscreen detection) is built in by default and only kicks in inside a Plasma session, other desktops use the generic Wayland/X11 paths. It needs the D-Bus development files. To build without it:

```bash
cmake -DCMAKE_BUILD_TYPE='Release' -DDISABLE_KDE_FEATURES=ON ..
```

On KDE Plasma the engine loads KWin scripts of its own for the cursor position and for the windows that pause it (maximized, fullscreen, and focused with `--pause-on-focus`), per screen like Wallpaper Engine; nothing needs installing.

On a Wayland compositor that offers no layer shell and isn't GNOME (Gala on elementary OS) the engine falls back to XWayland: it draws into a desktop type background window of its own, like `--screen-root` does on an X11 desktop. Wallpapers, scripts and the pointer work as usual, X11 windows pause it, and on Gala native Wayland windows too, through its `org.pantheon.gala.DesktopIntegration` D-Bus interface (it gives sizes but no fullscreen or maximized flag, so a window at least as large as a screen counts as fullscreen and maximized ones don't pause). The wallpaper window takes the clicks that would reach the desktop.

On sway, Hyprland, niri, labwc and other compositors with the wlr foreign toplevel protocol a fullscreen window pauses only the screen it is on (maximized ones too with `--pause-on-maximized`). sway before 1.12 doesn't say which screen a window that opens fullscreen is on, so such a window pauses every screen there.

## Assets

You need Wallpaper Engine installed through Steam - this is where the backgrounds and shared assets come from. The binary looks for it automatically under:

```
~/.steam/steam/steamapps/common
~/.local/share/Steam/steamapps/common
~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common
~/snap/steam/common/.local/share/Steam/steamapps/common
```

If it isn't found there, either copy the `assets` folder from Wallpaper Engine's install directory (Steam -> Wallpaper Engine -> Manage -> Browse local files) next to the `linux-wallpaperengine` binary, or point at it directly:

```bash
linux-wallpaperengine --assets-dir /path/to/assets
```

### Fonts

Some wallpapers show emoji in their text layers and expect Windows' emoji font, Segoe UI Emoji (`seguiemj.ttf`). It is not part of this repo (it is Microsoft's font), but installing it makes those wallpapers look exactly like on Windows: copy `C:\Windows\Fonts\seguiemj.ttf` from a Windows install to `~/.local/share/fonts/` and run `fc-cache -f`. Without it the emoji come from the Twemoji font in Wallpaper Engine's assets. Wallpapers using `systemfont_*` text likewise look closest with the Windows fonts they name (Arial, Segoe UI, ...) or their metric-compatible clones installed.

Fonts for wallpapers can also be put in `~/.local/share/linux-wallpaperengine/fonts/` (`LWE_FONT_DIR` changes the folder): the text renderer sees them next to the system's fonts without installing them anywhere else, so a Segoe UI, Arial or Segoe UI Emoji file there is used for the wallpapers that ask for it. `linux-wallpaperengine --list-fonts <wallpaper>` lists what a wallpaper's texts need that this system lacks (a Windows font replaced by another one, emoji without Segoe UI Emoji), the engine also says so in its log when a text loads, and we_manager shows the same warning with an "Add font" button.

## Usage

```bash
linux-wallpaperengine [options] <background_id or path>
```

The background can be a Steam Workshop ID (`1845706469`) or a path to a background folder.

### Options

| Option | Description |
|--------|-------------|
| `--silent` | Mute background audio |
| `--volume <val>` | Set audio volume |
| `--automute` | Mute when other apps play audio |
| `--noautomute` | Don't mute when other apps play audio (default) |
| `--no-audio-processing` | Disable audio reactive features |
| `--fps <val>` | Limit frame rate |
| `--window <XxYxWxH>` | Run in windowed mode with custom size/position |
| `--window-normal` | With `--window`: a regular window with a title bar that can be moved, resized, closed and covered by other windows, instead of the borderless window that stays on top |
| `--window-background` | X11 only, with `--window`: the window acts as the desktop background (desktop type, below every other window, on all workspaces, off the taskbar, never focused, clicks go through, desktop icons stay above it). For desktops that don't show a root window wallpaper, e.g. some Cinnamon setups |
| `--no-desktop-icons` | X11 only: leave the icons of an opaque desktop window (xfdesktop, caja, pcmanfm-qt) under the wallpaper instead of drawing them above it; the desktop's background is not touched. Same as `LWE_DESKTOP_ICONS=off` |
| `--screen-root <screen>` | Set as background for a specific screen |
| `--screen-span <screen-1>,<screen-2>,...` | Stretch a single wallpaper across multiple screens |
| `--screen-slot <screen>` | Add a slot to a screen, once per wallpaper, so several share it side by side (stacked on a portrait screen); the following `--bg`, `--scaling`, etc. apply to that slot, called `<screen>#<n>` |
| `--split-layout aspect\|equal\|center` | How the previous `--screen-slot`'s screen is shared: sized from each wallpaper's aspect ratio (default), equal slots, or the wallpapers' own shapes centered with bars around them |
| `--bg <id/path>` | Assign a background to a screen (used after `--screen-root`/`--screen-span`/`--screen-slot`) |
| `--playlist <file>` | Cycle through a Wallpaper Engine playlist from `config.json` |
| `--scaling <mode>` | `stretch`, `fit`, `fill`, `center`, `free`, or `default` |
| `--zoom <factor>` | Manual zoom on top of `--scaling`, e.g. `1.5` in, `0.5` out |
| `--render-scale <factor>` | Render this output below its native size, `0.1` to `1` (`0.5` draws a quarter of the pixels), and let the compositor scale it up. Wayland with `wp_viewporter` only; for secondary or very large outputs. Applies to the previous `--screen-root`, every screen of the previous `--screen-span` group or the screen of the previous `--screen-slot` |
| `--offset <X,Y>` | Move the visible crop window of a cropping `--scaling`/`--zoom`, each axis in [-1, 1] |
| `--alignment-position <0-100>` | Wallpaper Engine's alignment position: where `fill`/`default` crop and `fit` letterboxes (default 50) |
| `--alignment-x <0-100>`, `--alignment-y <0-100>` | Placement for `center` and `free` (default 50, 0 = left/top) |
| `--alignment-zoom <0-200>` | Zoom for `free` (default 100 = native size) |
| `--clamp <mode>` | Texture clamping: `clamp` (edge), `border`, `repeat`. Default `border` |
| `--corner-color <hex>` | Color outside the wallpaper's bounds when `--clamp border`, as `RRGGBB`/`RRGGBBAA`. Default `000000` |
| `--image-filter <name>`, `--image-filter-strength <0-100>` | Wallpaper Engine's image filters (LUTs from the assets, e.g. `lutx32_amber`), scene and video wallpapers |
| `--brightness`, `--contrast`, `--saturation`, `--hue <0-100>` | Wallpaper Engine's color options, 50 leaves the image unchanged |
| `--color-options on/off` | Turn the color options (also ones a preset carries) on or off |
| `--flip on/off` | Mirror the wallpaper horizontally |
| `--transition <kind>` | Animation when another wallpaper takes over (playlist, hotswap): `none` (default), `fade`, `slide-left`, `slide-right`, `wipe-left`, `wipe-right` or `zoom`. Control file: `transition=` |
| `--transition-duration <ms>` | Length of the transition, 100 to 5000, default 600. Control file: `transition-duration=` |
| `--hdr` | Wayland only: HDR (PQ, BT.2020) output to monitors running in HDR mode, HDR videos in HDR |
| `--layer <layer>` | Wayland only: `wlr-layer-shell` layer (`background`, `bottom`, `top`, `overlay`). Default `bottom`, on KDE `background` can end up under plasmashell's desktop window |
| `--speed <factor>` | Global playback speed multiplier |
| `--hwdec <backend>` | mpv hardware decoding for videos: `auto` (default), `no`, `vaapi-copy`, `nvdec-copy`, ... Try `no` if videos crash on NVIDIA |
| `--web-frames <auto\|shm\|dmabuf>` | how web wallpapers hand their frames over: `dmabuf` shares the browser's GPU buffers (Wayland and X11, much less CPU at high resolutions), `shm` reads every frame back, `auto` (default) picks dmabuf where it works and falls back to shm |
| `--frame-limiter <budget\|fixed>` | how the FPS limit paces frames: `budget` (default, like Wallpaper Engine) shares a one second budget between the frames of that second, `fixed` sleeps the same time after every frame. Only matters for things that count whole frames, like rope trails |
| `--video-start <time>`, `--video-end <time>` | Video wallpapers only: loop just this part of the video, in seconds, `m:ss` or `h:mm:ss` (either one alone leaves that side at the video's start/end) |
| `--video-segments <start-end,...>` | Video wallpapers only: play these parts one after the other and skip everything between them, e.g. `"2:00-3:00,4:00-5:00"`; an empty side is the video's start/end (`"4:00-"`) |
| `--control-file <path>` | File the `SIGUSR1` hotswap request is read from, one per engine when several run. Default `$XDG_RUNTIME_DIR/lwe-control`, `/tmp/lwe-control-<uid>` without it |
| `--post-processing <mode>` | `disabled` (no bloom), `enabled` (default), `ultra` (HDR rendering and HDR bloom for scenes with bloom + hdr) or `displayhdr` (highlights up to the HDR output's peak, with `--hdr`) |
| `--shadows <quality>`, `--volumetrics <quality>` | `disabled`, `low`, `medium` (default), `high`, `ultra` |
| `--msaa <none/x2/x4/x8>` | Wallpaper Engine's anti-aliasing, only for scenes with 3D models like in Wallpaper Engine |
| `--ssaa <none/x2/x3/x4>` | Supersampling: render the scene that many times larger per axis and average it down |
| `--expand-canvas` | Show image layers that reach past the camera in full instead of cropped |
| `--disable-animations` | Freeze scripts, particles, effects and puppets at their current frame |
| `--allow-parallax-overflow` | Let parallax move images past their own edges |
| `--assets-dir <path>` | Custom assets path |
| `--screenshot <file>` | Save a screenshot (PNG/JPEG/BMP) |
| `--screenshot-delay <n>` | Frames to wait before the screenshot (default 5) |
| `--list-properties` | List a wallpaper's customizable properties |
| `--set-property name=value` | Override a property |
| `--list-objects` | List every object/layer, with id, name and type |
| `--disable-object <id/name>` | Hide an object/layer, repeatable |
| `--enable-object <id/name>` | Force an object/layer to show, repeatable |
| `--list-effects` | List every effect with id, editor name and category |
| `--list-fonts` | List the fonts a wallpaper's texts need that this system doesn't have, and what is drawn instead |
| `--disable-effect <id/name>`, `--enable-effect <id/name>` | Turn an effect off, or force a hidden one on, repeatable |
| `--disable-particles` | Disable particles |
| `--disable-mouse` | Disable mouse interaction |
| `--input-file <path>` | Wayland: file a desktop shell writes the left button to as `{"leftDown": true}`, refreshed while it is held (ignored after 2 seconds). For shells that cover the background and swallow its pointer events |
| `--disable-parallax` | Disable parallax |
| `--no-fullscreen-pause` | Don't pause while an app is fullscreen |
| `--pause-on-focus` | Also pause while another application has the focus (Wallpaper Engine's "Other application focused: Pause"; on Wayland only that monitor) |
| `--pause-on-maximized` | wlroots compositors (sway, Hyprland, niri, labwc...): also pause the monitor a maximized window is on. KDE, GNOME and X11 desktops always do, like Wallpaper Engine |
| `--fps-battery <fps>` | FPS limit while the computer runs on battery (UPower, else `/sys/class/power_supply`), `0` pauses |
| `--pause-on-battery` | Pause while on battery, same as `--fps-battery 0` |
| `--idle-pause <minutes>` | Pause after this many minutes without keyboard or mouse input (ext-idle-notify-v1 on Wayland, the X11 screen saver extension, GNOME's idle monitor); anything inhibiting idle, like a playing video, keeps it running |
| `--no-lock-pause` | Keep rendering while the session is locked. By default the wallpaper pauses while logind's `LockedHint` is set or a screen saver (`org.freedesktop.ScreenSaver`, GNOME, Cinnamon, MATE) reports itself active, and while an X server's DPMS or GNOME's `PowerSaveMode` has the display blanked. Other Wayland compositors are left to their frame callbacks, which most stop for a blanked output |
| `--fullscreen-pause-only-active` | Wayland only: pause only when the fullscreen window is active |
| `--fullscreen-pause-ignore-appid <val>` | Wayland only: ignore fullscreen windows whose app_id contains `<val>`, repeatable |
| `--audio-screen <screen>` | Only this screen's background produces audio |
| `--ambient-volume <val>` | Separate volume for non-video backgrounds; video keeps using `--volume` |
| `--hrtf <auto\|on\|off>` | Headphones (HRTF) rendering of scene sounds like Wallpaper Engine: `auto` uses it while the default output is headphones or a headset |
| `--sound-volume <id/name>=<val>` | Per-object volume (0-1), repeatable, `*` for unmatched objects |
| `--audio-sensitivity <id/name>=<mult>` | Scale an object's audio-reactive swing; `0` disables it, `1` is default; repeatable, `*` for unmatched objects |
| `--list-audio-objects` | List objects whose script reacts to music |
| `--leds`, `--leds-server <host[:port]>` | Send the scene's colors to the RGB devices of an OpenRGB server (start it with `openrgb --server`, default `127.0.0.1:6742`). Layers with `ledsource` are used (the first one in the scene), else the whole scene |
| `--leds-brightness <percent>`, `--leds-no-boost`, `--leds-reduce-fps` | Brightness factor, leave the colors as they are instead of raising each led's strongest channel to full (WE's "Boost LED colors"), update 15 times a second instead of 30 |
| `--dump-structure`, `--render-debug <mode>` | Debugging output, see `--help` |
| `--no-shader-cache` | Translate and link every shader from scratch instead of using `~/.cache/linux-wallpaperengine/shaders` |

### Examples

Run by Workshop ID:
```bash
linux-wallpaperengine 1845706469
```

Run from a local folder:
```bash
linux-wallpaperengine ~/backgrounds/1845706469/
```

Different background per monitor:
```bash
linux-wallpaperengine \
  --scaling stretch --screen-root eDP-1 --bg 2667198601 \
  --scaling fill --screen-root HDMI-1 --bg 2667198602
```

One wallpaper across multiple monitors:
```bash
linux-wallpaperengine --scaling fill --screen-span HDMI-A-1,DP-2,DP-3 --bg 1845706469
```

Three wallpapers on one monitor, e.g. two portrait ones around a landscape one (the first slot plays audio unless
`--audio-screen DP-1#2` picks another; a control file with `screen=DP-1#2` hotswaps just that slot):
```bash
linux-wallpaperengine --screen-slot DP-1 --bg 3663326998 --screen-slot DP-1 --bg 1846362380 --scaling fit \
  --screen-slot DP-1 --bg 3486109050 --split-layout aspect
```

Windowed:
```bash
linux-wallpaperengine --window 0x0x1280x720 1845706469
```

Capped FPS:
```bash
linux-wallpaperengine --fps 30 1845706469
```

Screenshot (useful as input for pywal-style color extraction):
```bash
linux-wallpaperengine --screenshot ~/wallpaper.png 1845706469
```

Inspect and override properties:
```bash
linux-wallpaperengine --list-properties 2370927443
```
```
barcount - slider
	Description: Bar Count
	Value: 64
	Minimum value: 16
	Maximum value: 64
	Step: 1

bloom - boolean
	Description: Bloom
	Value: 0
```
```bash
linux-wallpaperengine --set-property bloom=1 2370927443
```

List and toggle layers (not every wallpaper exposes a property for each one):
```bash
linux-wallpaperengine --list-objects 2370927443
```
```
Objects for default:
  1 - Background (image)
  2 - Clock (text)
  3 - Rain (particle)
```
```bash
linux-wallpaperengine --disable-object Clock --disable-object 3 2370927443
```

## Wayland and X11

- Wayland: needs a compositor with `wlr-layer-shell-unstable` and `xdg-output-unstable-v1` (the latter for accurate monitor positioning with `--screen-span`). On KDE Plasma the engine uses KWin's `plasma-shell` surfaces and KWin's cursor position on top of that.
- GNOME on Wayland: Mutter has no layer-shell, so the engine needs its GNOME Shell extension (`gnome-shell-extension/`, installed next to the binary; GNOME 45 to 50, on 45-48 it starts its `linux-wallpaperengine-gnome-helper` from its own folder). Desktop Icons NG works on top of it, clicks on the empty desktop reach the wallpaper like on Windows. Copy `linux-wallpaperengine@wallpaperenginelover.github.io` to `~/.local/share/gnome-shell/extensions/`, run `gnome-extensions enable linux-wallpaperengine@wallpaperenginelover.github.io` and log out and back in once (we_manager offers to do it). Outputs are the connector names (`DP-1`, `HDMI-A-1`).
- X11: needs XRandr; target monitors with `--screen-root <name>` as reported by `xrandr`. Desktops whose compositor paints its own background over the root window (GNOME, Pantheon, Cinnamon) get a background window of the engine's own automatically; `LWE_X11_BACKGROUND_WINDOW=1` does the same on any other desktop and `0` draws on the root window again.
- XFCE (xfdesktop) draws an opaque desktop window over the root window, which the wallpaper goes above, so its icons would be hidden. The engine keeps them: while it runs, xfdesktop's background is set to black (xfconf, `image-style`/`color-style`/`rgba1` of every monitor and workspace, the old values are saved and put back on exit, also after a crash), the window's pixels are read through the Composite extension whenever it changes and everything that is not black is drawn over the wallpaper frame. xfdesktop keeps handling every click. Needs the Composite and Damage extensions (always there on Xorg); `LWE_DESKTOP_ICONS=off` leaves xfdesktop alone and the icons under the wallpaper. A desktop that never picked a wallpaper gets `xfce-shapes.svg` (or the first of xfce's own pictures) afterwards, xfdesktop's built-in default can't be set again. MATE's caja and LXQt's pcmanfm-qt get the same treatment (caja's background through gsettings, pcmanfm-qt's with `pcmanfm-qt --wallpaper-mode`, both restored on exit); `--no-desktop-icons` or `LWE_DESKTOP_ICONS=off` leaves all of them alone.

## Limitations

The scene renderer follows Wallpaper Engine 2.8 closely, but not everything is there yet.

Platform and setup:

- HDR output (`--hdr`) is Wayland only and needs a compositor with the color-management protocol (`wp_color_manager_v1`). X11 has no HDR, neither the server nor any X11 compositor can show it, so that stays the way it is. Checked on a real HDR monitor under KDE Plasma only.
- On Wayland, web wallpapers and scene wallpapers with click handlers (e.g. bone dragging) take all mouse clicks on the desktop, so the right click menu of the desktop doesn't open while they are shown. A Wayland input region can't be limited to one mouse button, and a click can't be passed on to the surface below. Wallpapers without click handlers don't take clicks where the compositor gives the pointer position (KDE Plasma, Hyprland, GNOME).
- KDE Plasma 5 on Wayland doesn't pause behind windows: KWin 5 has no `workspace.windowList` in its scripting API (Plasma 6 is fine).
- elementary OS (Pantheon) has no `wlr-layer-shell`, so the engine draws through XWayland. XWayland only reports the pointer over its own windows, so the wallpaper keeps its input region to follow the cursor (parallax, cursor effects) and takes all clicks on the desktop: the desktop's right click menu doesn't open. That is a limit of XWayland, not something the engine can work around. Windows that cover an output pause it, maximized windows don't.
- Web wallpapers pass their frames to the engine as GPU buffers (dmabuf) where that was checked: AMD on Mesa and the VM matrix. With the NVIDIA proprietary driver `--web-frames auto` stays on shared memory frames, which costs more CPU, since dmabuf is not verified there. On Intel and with amdgpu user mode queues `auto` uses dmabuf but it was not checked for tearing, `--web-frames shm` switches back if a page shows stale frames.
- Windows fonts (`systemfont_*`) are replaced by the closest installed match through fontconfig, so text can look slightly different if Arial, Segoe UI etc. or their metric-compatible clones aren't installed.
- RGB lighting (`--leds`) goes through an OpenRGB server only (no iCUE or Razer Chroma SDK) and only scene wallpapers feed it, not video or web ones. OpenRGB has no led positions, so a keyboard keeps its grid and the other zones become rows; Wallpaper Engine places every led by its real position.
- The Arch distribution build needs `libnode` (a Node.js built with `--shared`, Arch's `nodejs` has no shared library). The AUR has it, and the CI keeps a prebuilt package as an asset of the [`libnode-arch` release](https://github.com/WallpaperEngineLover/linux-wallpaperengine-kde/releases/tag/libnode-arch): `sudo pacman -U libnode-*-x86_64.pkg.tar.zst`. we_manager's build section has an "Install libnode" button on Arch that does exactly this. The portable build and `-bin` bring their own V8.

Text:

- Without Segoe UI Emoji installed (see Fonts), emoji come from the Twemoji font in Wallpaper Engine's assets and are drawn as their colour bitmap; Wallpaper Engine itself draws nothing for them without that font.

## Credits

- [RePKG](https://github.com/notscuffed/repkg) - texture flag insights
- [RenderDoc](https://github.com/baldurk/renderdoc) - OpenGL debugging
- [OpenAL Soft](https://github.com/kcat/openal-soft) (LGPL) - the HRTF renderer for sound objects is a port of its 1.21.1 code, with its built-in data set based on the [MIT KEMAR](http://sound.media.mit.edu/resources/KEMAR.html) measurements (`src/WallpaperEngine/Audio/resources/default_hrtf.mhr`)
