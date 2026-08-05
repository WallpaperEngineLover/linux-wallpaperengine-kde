# linux-wallpaperengine-kde

A fork of [Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine), reworked for KDE Plasma and Wayland. It plays Wallpaper Engine (Steam app 431960) live wallpapers on Linux: scene/parallax backgrounds, video (via mpv), and web/HTML backgrounds (via CEF), rendered with a from-scratch OpenGL reimplementation of Wallpaper Engine's renderer.

No GUI. This is a command-line tool driven entirely by flags - see Usage below.

## What's different from upstream

- Native Wayland output via `wlr-layer-shell`, in addition to the original X11 path.
- KDE-specific fullscreen-pause detection over the `plasma-shell` protocol, since KWin doesn't implement `wlr-foreign-toplevel-management` like other wlroots compositors. Still experimental (see Limitations).
- Multi-monitor handling: per-screen backgrounds (`--screen-root`), one wallpaper spanning several monitors (`--screen-span`), per-screen scaling/clamping/zoom/corner color, and Workshop playlists (`--playlist`).
- A global playback speed multiplier (`--speed`), separate from the FPS cap.
- More granular audio: restrict sound to a single screen (`--audio-screen`), a separate ambient volume for non-video backgrounds (`--ambient-volume`), per-object sound volume (`--sound-volume`), and a tunable multiplier on audio-reactive properties (`--audio-sensitivity`, with `--list-audio-objects` to see what's wired up).
- Live hotswap of the running wallpaper via `SIGUSR1`, no process restart.
- Layer introspection/toggling (`--list-objects`, `--disable-object`, `--enable-object`) for layers the wallpaper author didn't expose as a configurable property.

## Requirements

- OpenGL 3.3
- CMake
- LZ4, Zlib
- SDL2
- FFmpeg
- X11 or Wayland (Xrandr on X11)
- GLFW3, GLEW, GLUT, GLM
- MPV
- PulseAudio
- FFTW3

### Ubuntu 22.04
```bash
sudo apt-get update
sudo apt-get install build-essential cmake libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libglew-dev freeglut3-dev libsdl2-dev liblz4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libxxf86vm-dev libglm-dev libglfw3-dev libmpv-dev mpv libmpv1 libpulse-dev libpulse0 libfftw3-dev libfreetype-dev
```

### Ubuntu 24.04
```bash
sudo apt-get update
sudo apt-get install build-essential cmake libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libglew-dev freeglut3-dev libsdl2-dev liblz4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libxxf86vm-dev libglm-dev libglfw3-dev libmpv-dev mpv libmpv2 libpulse-dev libpulse0 libfftw3-dev libfreetype-dev
```

### Fedora 42
```bash
sudo dnf update
sudo dnf install gcc g++ cmake libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel glew-devel freeglut-devel SDL2-devel lz4-devel ffmpeg ffmpeg-free-devel libXxf86vm-devel glm-devel glfw-devel mpv mpv-devel pulseaudio-libs-devel fftw-devel gmp-devel
```

### ALT Linux
```bash
sudo epm update
sudo epm install gcc-c++ make cmake libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libGL-devel libGLEW-devel freeglut-devel libSDL2-devel liblz4-devel libavcodec-devel libavformat-devel libavutil-devel libswscale-devel libXxf86vm-devel libglm-devel libglfw3-devel libmpv-devel mpv libpulseaudio-devel libpulseaudio libfftw3-devel libpng-devel libffi-devel libswresample-devel libgmpxx-devel
```

## Build

```bash
git clone --recurse-submodules <this repo's url>
cd linux-wallpaperengine-kde
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE='Release' ..
make
```

The binary and support files end up in `build/output`.

If you want the "pause on fullscreen" feature on KDE Plasma, install the [KWin Maximize Detector](https://github.com/LS-FCEFyN/Maximize-Detector) script separately, and build with:

```bash
cmake -DCMAKE_BUILD_TYPE='Release' -DENABLE_KDE_EXPERIMENTAL_FEATURES=ON ..
```

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
| `--scaling <mode>` | `stretch`, `fit`, `fill`, `center`, or `default` |
| `--zoom <factor>` | Manual zoom on top of `--scaling`, e.g. `1.5` in, `0.5` out |
| `--clamp <mode>` | Texture clamping: `clamp` (edge), `border`, `repeat`. Default `border` |
| `--corner-color <hex>` | Color outside the wallpaper's bounds when `--clamp border`, as `RRGGBB`/`RRGGBBAA`. Default `000000` |
| `--layer <layer>` | Wayland only: `wlr-layer-shell` layer (`background`, `bottom`, `top`, `overlay`) |
| `--speed <factor>` | Global playback speed multiplier |
| `--assets-dir <path>` | Custom assets path |
| `--screenshot <file>` | Save a screenshot (PNG/JPEG/BMP) |
| `--screenshot-delay <n>` | Frames to wait before the screenshot (default 5) |
| `--list-properties` | List a wallpaper's customizable properties |
| `--set-property name=value` | Override a property |
| `--list-objects` | List every object/layer, with id, name and type |
| `--disable-object <id/name>` | Hide an object/layer, repeatable |
| `--enable-object <id/name>` | Force an object/layer to show, repeatable |
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

- Wayland: needs a compositor with `wlr-layer-shell-unstable` and `xdg-output-unstable-v1` (the latter for accurate monitor positioning with `--screen-span`).
- X11: needs XRandr; target monitors with `--screen-root <name>` as reported by `xrandr`. Doesn't work if something else (GNOME, KDE, Nautilus) is already drawing the desktop background/compositing it.

## Limitations

- Light and VolumeLight scene objects are parsed but not rendered - wallpapers that depend on them for lighting will look different from the Windows original.
- Passthrough image effects aren't implemented.
- Text objects can't sample the background behind them (no copybackground-style effects on `Text`).
- KDE fullscreen-pause detection is experimental and requires the separate KWin Maximize Detector script plus `-DENABLE_KDE_EXPERIMENTAL_FEATURES=ON`.
- On X11, a compositor or DE drawing its own background will block the wallpaper. Disabling the compositor is currently the only fix.
- Some NVIDIA setups hit GLFW/OpenGL init failures; try `__GL_THREADED_OPTIMIZATIONS=0 linux-wallpaperengine` if you run into this.

## Credits

- [RePKG](https://github.com/notscuffed/repkg) - texture flag insights
- [RenderDoc](https://github.com/baldurk/renderdoc) - OpenGL debugging
