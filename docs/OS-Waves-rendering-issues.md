# OS Waves (3014738359) - Open Rendering Issue

The earlier investigation of this wallpaper listed six differences from the native
Windows Wallpaper Engine. Script-driven user settings, text objects, the orthographic
"auto" camera and `autosize` are implemented now, and skipping passthrough images
without effects matches the real engine (`CImage.cpp`, `sub_140175830`). One item is
still open.

## Audio spectrum response may differ

The pulse effect on the three OS logos (objects 173, 57 and 61) uses:
- `AUDIOPROCESSING=3`
- `audioamount=1.0`, `audiobounds=0.5-1.0`, `audioexponent=0.35`

Nobody has compared the logo pulse against Windows side by side, so whether our
spectrum analysis (`Audio/SpectrumNormalizer`) drives these parameters the same way is
unknown. Checking it needs the same audio playing on both engines at once.

## Reproduction

```bash
linux-wallpaperengine \
  --assets-dir ~/.local/share/Steam/steamapps/common/wallpaper_engine/assets \
  --screen-root DP-2 --screen-root HDMI-A-1 \
  --fps 60 \
  ~/.local/share/Steam/steamapps/workshop/content/431960/3014738359
```
