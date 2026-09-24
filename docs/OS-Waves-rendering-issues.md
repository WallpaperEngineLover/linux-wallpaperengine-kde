# OS Waves (3014738359) - Open Rendering Issue

The earlier investigation of this wallpaper listed six differences from the native
Windows Wallpaper Engine. Script-driven user settings, text objects, the orthographic
"auto" camera and `autosize` are implemented now, and skipping passthrough images
without effects matches the real engine (`CImage.cpp`, `sub_140175830`). One item is
left to confirm on real hardware.

## Audio spectrum response may differ

The pulse effect on the three OS logos (objects 173, 57 and 61) uses:
- `AUDIOPROCESSING=3`
- `audioamount=1.0`, `audiobounds=0.5-1.0`, `audioexponent=0.35`

The spectrum these read now follows the real engine's pipeline, taken from `wallpaper64.exe`
(`Audio/SpectrumAnalyzer` and `Audio/SpectrumProcessor`): same FFT size, band layout, per-group level
tracking, smoothing and separate left/right channels. Still unverified: a side-by-side comparison with
Windows playing the same audio, and whether WE's loopback capture and the Linux monitor source both see
the signal before or after the system volume.

## Reproduction

```bash
linux-wallpaperengine \
  --assets-dir ~/.local/share/Steam/steamapps/common/wallpaper_engine/assets \
  --screen-root DP-2 --screen-root HDMI-A-1 \
  --fps 60 \
  ~/.local/share/Steam/steamapps/workshop/content/431960/3014738359
```
