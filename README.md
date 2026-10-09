# linux-wallpaperengine-kde

A fork of [Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine), reworked for KDE Plasma and Wayland. It plays Wallpaper Engine (Steam app 431960) live wallpapers on Linux: scene/parallax backgrounds, video (via mpv), and web/HTML backgrounds (via CEF), rendered with a from-scratch OpenGL reimplementation of Wallpaper Engine's renderer.

The engine itself is a command-line tool driven entirely by flags, see Usage below. For a GUI (Workshop browsing, library, playlists, per-monitor wallpapers, backups) use [WE Manager](https://github.com/WallpaperEngineLover/we_manager), which also downloads and updates the engine for you.

## What's different from upstream

Most of the work since the fork went into making wallpapers look and behave like in Wallpaper Engine 2.8. Where it matters the behavior was taken from the Wallpaper Engine binary itself and checked side by side against the real Wallpaper Engine running under Wine, on close to 200 Workshop wallpapers.

KDE Plasma and Wayland:

- On Plasma the wallpaper is a real desktop surface (`plasma-shell` protocol): not in the taskbar or task switcher, unaffected by Show Desktop.
- Mouse-reactive wallpapers work on KDE Wayland, the cursor position comes from KWin since the desktop never gets pointer events there.
- Fullscreen-pause detection through KWin, since it doesn't implement `wlr-foreign-toplevel-management` like wlroots compositors. Still experimental (see Limitations).
- Unplugging a monitor no longer leaks layer surfaces (that made KWin grow to gigabytes).
- HDR output (`--hdr`) through the Wayland color management protocol, HDR videos included.
- Outside Plasma the KDE parts switch themselves off, and there is a build without them (`-DDISABLE_KDE_FEATURES=ON`).

Scenes:

- 3D scenes: perspective camera, models with skinning, morphs and root motion, camera objects and paths, fog.
- Wallpaper Engine's LightingV1 with shadow maps, legacy point lights, volumetric light, planar reflections, HDR bloom (`--post-processing ultra`), MSAA (`--msaa`) and supersampling (`--ssaa`).
- Scenes render at the output resolution with Wallpaper Engine's scaling and alignment modes, image filters and color options.
- Particles: child systems, everything added in Wallpaper Engine 2.7+ (boids, collisions, remap, HSV colors, color lists, ...), exact turbulence/vortex/attraction formulas, prewarm, ropes through the geometry shader like Wallpaper Engine.
- Text rewritten with HarfBuzz and MSDF: font fallback for missing characters, outlines, blur, drop shadows, wrapping, backgrounds, blend modes, screen anchors. Sizes and positions match Wallpaper Engine.
- Puppets (animated characters): animation layers with Wallpaper Engine's blending, bone physics, IK and rope chains, blend rules, bone alpha, morphs, clipping masks, animated draw order.
- Sound objects with Wallpaper Engine's play modes and OpenAL-style spatialization.
- Presets and other items built on another Workshop item load their base wallpaper, SOG depth-photo wallpapers are drawn natively.

Scripts and audio:

- Scene scripts run on V8 like in Wallpaper Engine, with Wallpaper Engine's own helper classes from its assets and most of the SceneScript API (layer creation and sorting, cursor events, video textures, animations, bones, camera, model data, timers, user shortcuts).
- The audio spectrum is computed like Wallpaper Engine computes it, so visualizers move the same.
- Web wallpapers run CEF in a separate, disposable process and get properties, audio data and Wallpaper Engine's JavaScript API.

Control:

- Multi-monitor handling: per-screen backgrounds (`--screen-root`), one wallpaper spanning several monitors (`--screen-span`), per-screen scaling/zoom/alignment/corner color, and Workshop playlists (`--playlist`).
- Live hotswap of the running wallpaper and its settings via `SIGUSR1`, separately per engine with `--control-file`, no process restart.
- A global playback speed multiplier (`--speed`), separate from the FPS cap.
- Video wallpapers can loop just part of the video (`--video-start`/`--video-end`), e.g. only minute 3 to 4 of a 5 minute clip, or several parts one after the other while skipping the rest (`--video-segments "2:00-3:00,4:00-5:00"`).
- More granular audio: restrict sound to a single screen (`--audio-screen`), a separate ambient volume for non-video backgrounds (`--ambient-volume`), per-object sound volume (`--sound-volume`), and a tunable multiplier on audio-reactive properties (`--audio-sensitivity`, with `--list-audio-objects` to see what's wired up).
- Layer and effect introspection/toggling (`--list-objects`, `--disable-object`, `--list-effects`, `--disable-effect`, ...) for things the wallpaper author didn't expose as a property.

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
sudo apt-get install build-essential cmake libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libglew-dev freeglut3-dev libsdl2-dev liblz4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libxxf86vm-dev libglm-dev libglfw3-dev libmpv-dev mpv libmpv1 libpulse-dev libpulse0 libfftw3-dev libfreetype-dev libharfbuzz-dev libdbus-1-dev libwayland-dev wayland-protocols libegl1-mesa-dev
```

### Ubuntu 24.04
```bash
sudo apt-get update
sudo apt-get install build-essential cmake libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libglew-dev freeglut3-dev libsdl2-dev liblz4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libxxf86vm-dev libglm-dev libglfw3-dev libmpv-dev mpv libmpv2 libpulse-dev libpulse0 libfftw3-dev libfreetype-dev libharfbuzz-dev libdbus-1-dev libwayland-dev wayland-protocols libegl1-mesa-dev libnode-dev
```

### Fedora 44
```bash
sudo dnf install gcc g++ cmake pkg-config libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel mesa-libEGL-devel glew-devel freeglut-devel SDL2-devel lz4-devel ffmpeg-free-devel libXxf86vm-devel glm-devel glfw-devel mpv-devel pulseaudio-libs-devel fftw-devel freetype-devel harfbuzz-devel gmp-devel dbus-devel wayland-devel wayland-protocols-devel nodejs24-devel
```
If you already have Node.js 22, use `nodejs22-devel` instead, Fedora's nodejs -devel packages can't be installed side by side. `ffmpeg` and `ffmpeg-free` conflict, with RPM Fusion's full ffmpeg install its `ffmpeg-devel` instead.

### Arch Linux
```bash
sudo pacman -S --needed git base-devel cmake pkg-config glew freeglut sdl2 lz4 ffmpeg glm glfw mpv libpulse fftw freetype2 harfbuzz dbus libxrandr libxinerama libxcursor libxi libxxf86vm wayland wayland-protocols mesa gmp nss at-spi2-core libcups libxcomposite libxdamage libxkbcommon pango cairo alsa-lib libdrm libxshmfence
```
The second half of that list is what the bundled CEF links against, a desktop install usually has it already. Arch's `nodejs` package has the headers but no `libnode`, the AUR's `libnode` package (a Node.js built with `--shared`) provides it, e.g. `paru -S libnode`.

### ALT Linux
```bash
sudo epm update
sudo epm install gcc-c++ make cmake libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libGL-devel libGLEW-devel freeglut-devel libSDL2-devel liblz4-devel libavcodec-devel libavformat-devel libavutil-devel libswscale-devel libXxf86vm-devel libglm-devel libglfw3-devel libmpv-devel mpv libpulseaudio-devel libpulseaudio libfftw3-devel libpng-devel libffi-devel libswresample-devel libgmpxx-devel libfreetype-devel libharfbuzz-devel libdbus-devel
```

## Installing

Tagged releases on the [releases page](https://github.com/WallpaperEngineLover/linux-wallpaperengine-kde/releases) have ready-to-run builds, each with (`kde`) and without (`generic`) the KDE Plasma integration:

- **Portable** (`linux-wallpaperengine-kde-portable-x86_64.tar.gz`): runs on any x86_64 distribution with glibc 2.35 or newer (Ubuntu 22.04 and newer, Debian 12+, Fedora, openSUSE, Arch, SteamOS, Bazzite, Silverblue/Kinoite and other image-based systems) without installing anything: it carries FFmpeg, mpv, V8 and the rest, and only uses what every desktop has (Mesa, the display server's and audio server's libraries, the GTK stack CEF needs). A few libraries it brings are only used where the system has no library of that name (`lib/fallback`), the `linux-wallpaperengine` script in the folder takes care of that.
- **Distribution builds** for Ubuntu 24.04, Fedora 44 and Arch Linux use the distribution's own FFmpeg, mpv and so on: install the runtime packages from the list above (the non `-dev`/`-devel` ones). The Arch build also needs the AUR's `libnode`.

```bash
tar xzf linux-wallpaperengine-kde-portable-x86_64.tar.gz
./linux-wallpaperengine-kde-portable-x86_64/linux-wallpaperengine --help
```

The folder can live anywhere. Web wallpapers run in Chromium's sandbox when the system allows unprivileged user namespaces or `chrome-sandbox` is setuid root (`sudo chown root:root chrome-sandbox && sudo chmod 4755 chrome-sandbox`), otherwise without it (Ubuntu 24.04+ restricts user namespaces, a tarball in the home folder can't have a setuid file).

Other ways to get it:

- **AUR**: `linux-wallpaperengine-kde-bin` (the portable build in `/opt`) or `linux-wallpaperengine-kde-git` (built from this repository, needs the AUR's `libnode`).
- **Flatpak**: [WE Manager](https://github.com/WallpaperEngineLover/we_manager)'s Flatpak comes with the engine.
- **Nix**: `nix run github:WallpaperEngineLover/linux-wallpaperengine-kde -- --help` builds it from source against nixpkgs (the first build also compiles Node.js as a shared library for V8) (`packages.x86_64-linux.generic` without the KDE integration, `overlays.default` adds `linux-wallpaperengine-kde`).

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

The "pause on fullscreen" feature on KDE Plasma also needs the [KWin Maximize Detector](https://github.com/LS-FCEFyN/Maximize-Detector) script installed separately.

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
| `--screen-root <screen>` | Set as background for a specific screen |
| `--screen-span <screen-1>,<screen-2>,...` | Stretch a single wallpaper across multiple screens |
| `--bg <id/path>` | Assign a background to a screen (used after `--screen-root`/`--screen-span`) |
| `--playlist <file>` | Cycle through a Wallpaper Engine playlist from `config.json` |
| `--scaling <mode>` | `stretch`, `fit`, `fill`, `center`, `free`, or `default` |
| `--zoom <factor>` | Manual zoom on top of `--scaling`, e.g. `1.5` in, `0.5` out |
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
| `--hdr` | Wayland only: HDR (PQ, BT.2020) output to monitors running in HDR mode, HDR videos in HDR |
| `--layer <layer>` | Wayland only: `wlr-layer-shell` layer (`background`, `bottom`, `top`, `overlay`). Default `bottom`, on KDE `background` can end up under plasmashell's desktop window |
| `--speed <factor>` | Global playback speed multiplier |
| `--video-start <time>`, `--video-end <time>` | Video wallpapers only: loop just this part of the video, in seconds, `m:ss` or `h:mm:ss` (either one alone leaves that side at the video's start/end) |
| `--video-segments <start-end,...>` | Video wallpapers only: play these parts one after the other and skip everything between them, e.g. `"2:00-3:00,4:00-5:00"`; an empty side is the video's start/end (`"4:00-"`) |
| `--control-file <path>` | File the `SIGUSR1` hotswap request is read from, one per engine when several run. Default `$XDG_RUNTIME_DIR/lwe-control` |
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
| `--disable-effect <id/name>`, `--enable-effect <id/name>` | Turn an effect off, or force a hidden one on, repeatable |
| `--disable-particles` | Disable particles |
| `--disable-mouse` | Disable mouse interaction |
| `--disable-parallax` | Disable parallax |
| `--no-fullscreen-pause` | Don't pause while an app is fullscreen |
| `--fullscreen-pause-only-active` | Wayland only: pause only when the fullscreen window is active |
| `--fullscreen-pause-ignore-appid <val>` | Wayland only: ignore fullscreen windows whose app_id contains `<val>`, repeatable |
| `--audio-screen <screen>` | Only this screen's background produces audio |
| `--ambient-volume <val>` | Separate volume for non-video backgrounds; video keeps using `--volume` |
| `--sound-volume <id/name>=<val>` | Per-object volume (0-1), repeatable, `*` for unmatched objects |
| `--audio-sensitivity <id/name>=<mult>` | Scale an object's audio-reactive swing; `0` disables it, `1` is default; repeatable, `*` for unmatched objects |
| `--list-audio-objects` | List objects whose script reacts to music |
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
- X11: needs XRandr; target monitors with `--screen-root <name>` as reported by `xrandr`. Doesn't work if something else (GNOME, KDE, Nautilus) is already drawing the desktop background/compositing it.

## Limitations

The scene renderer follows Wallpaper Engine 2.8 closely, but not everything is there yet.

Platform and setup:

- KDE fullscreen-pause detection is experimental and requires the separate KWin Maximize Detector script.
- On X11, a compositor or DE drawing its own background will block the wallpaper. Disabling the compositor is currently the only fix.
- HDR output (`--hdr`) is Wayland only and needs a compositor with the color-management protocol.
- Some NVIDIA setups hit GLFW/OpenGL init failures; try `__GL_THREADED_OPTIMIZATIONS=0 linux-wallpaperengine` if you run into this.
- Windows fonts (`systemfont_*`) are replaced by the closest installed match through fontconfig, so text can look slightly different if Arial, Segoe UI etc. or their metric-compatible clones aren't installed.
- RGB lighting output (iCUE/Chroma `ledsource`) is ignored.
- Arch has `libnode` only in the AUR, so the Arch release build needs it installed from there.

Puppets (2D animated characters):

- Morph modifiers (morph targets driven by a bone) and clipping masks with record flag 8 are not supported.
- `layerimage` particle emitters don't follow puppet layers.

3D scenes:

- Camera parallax is not applied in 3D scenes.

Images, particles and text:

- `layerimage` particle emitters read the layer right away, not from the GPU a frame later like Wallpaper Engine, and don't support text layers or opacity masks.
- Particle `spritesheetrefreshsync`, `alphatocoverage` and rope `uvscrolling`/`uvsmoothing` are not supported.
- Without Segoe UI Emoji installed (see Fonts), emoji come from the Twemoji font in Wallpaper Engine's assets and are drawn as their colour bitmap; Wallpaper Engine itself draws nothing for them without that font.

Sound:

- Spatialized sound objects don't switch to HRTF for headphones like OpenAL on Windows does.

Scripting (SceneScript):

- Not implemented: `setParent`, the effect material calls `getMaterial` and `setMaterialProperty`, blend shape calls, `transformAttachmentToTexture`, and the `resizeScreen` and `mediaStatusChanged` events.
- A few layer members read `undefined` (`sortorder`, `ledsource`, `colorBlendMode`, most light properties, ...).
- Property scripts inside a `createLayer` config aren't registered.
- Particles still update after scripts within a frame, Wallpaper Engine updates them before.

## Credits

- [RePKG](https://github.com/notscuffed/repkg) - texture flag insights
- [RenderDoc](https://github.com/baldurk/renderdoc) - OpenGL debugging
