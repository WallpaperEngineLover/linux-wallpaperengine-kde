#!/usr/bin/env python3
"""Sound object files at other rates than the output, against OpenAL Soft 1.21.1.

WE hands every sound file to OpenAL at its own rate and OpenAL resamples it in the mixer (linear, 12 bit fixed
point step), mixing in 20 ms updates (960 frames at 48 kHz). This builds one "single" or "loop" sound object scene
per case, renders it with the engine's SDL disk output and the same file with refstream (build.sh), and compares
them from the first non-zero frame: start, length (the held last sample and the 64 sample stop fade) and samples.

usage: resample_compare.py [--hrtf] [work dir]    (default work dir ~/.cache/lwe-openal-1.21.1, build.sh's)

The non-HRTF path differs by about one float ulp from OpenAL's (its panning goes through an ambisonic decode),
HRTF is bit-identical except for files above 48 kHz, where OpenAL mixes an update in several passes.
"""
import json, math, os, shutil, struct, subprocess, sys, wave
import numpy as np

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../..'))
ASSETS = os.environ.get('LWE_ASSETS_DIR', '/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets')
STOP = """
'use strict';
let frames = 0;
export function update(value) {
  frames++;
  if (frames === 12) thisScene.getLayer('snd').stop();
  return value;
}
"""
# name: rate, channels, seconds, playback mode, stopped by a script
CASES = {
    'mono22050': (22050, 1, 0.7, 'single', False),
    'stereo44100': (44100, 2, 0.7, 'single', False),
    'mono16000': (16000, 1, 0.7, 'single', False),
    'mono48000': (48000, 1, 0.7, 'single', False),
    'odd96000': (96000, 2, 0.7013, 'single', False),
    'stop22050': (22050, 1, 3.0, 'loop', True),
    'loop22050': (22050, 1, 0.4321, 'loop', False),
}


def tone(path, rate, secs, channels, repeat=1):
    frames = bytearray()
    n = int(rate * secs)
    for i in range(n):
        t = i / rate
        # a sweep plus a level step, ending away from zero so the held last sample shows
        v = 0.4 * math.sin(2 * math.pi * (300 + 900 * t) * t) + (0.3 if i > n * 0.8 else 0.0)
        s = int(round(v * 32767))
        frames += struct.pack('<h', s) if channels == 1 else struct.pack('<hh', s, -s // 2)
    with wave.open(path, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames) * repeat)


def scene(d, mode, stop):
    objs = [{'id': 1, 'name': 'snd', 'origin': '960 540 0', 'sound': ['sounds/a.wav'], 'volume': 1.0,
             'playbackmode': mode}]
    if stop:
        objs.append({'id': 2, 'name': 'stopper', 'origin': '0 0 0', 'visible': {'script': STOP, 'value': True}})
    json.dump({'file': 'scene.json', 'type': 'scene', 'title': os.path.basename(d), 'general': {'properties': {}}},
              open(d + '/project.json', 'w'))
    json.dump({'camera': {'center': '960 540 0', 'eye': '960 540 1', 'up': '0 1 0'},
               'general': {'orthogonalprojection': {'width': 1920, 'height': 1080}, 'clearcolor': '0 0 0',
                           'bloom': False, 'cameraparallax': False},
               'objects': objs, 'version': 3}, open(d + '/scene.json', 'w'))


def onset_end(x):
    nz = np.any(x != 0, axis=1)
    return int(np.argmax(nz)), len(x) - int(np.argmax(nz[::-1]))


def main():
    args = [a for a in sys.argv[1:] if a != '--hrtf']
    hrtf = '--hrtf' in sys.argv
    work = os.path.abspath(args[0] if args else os.path.expanduser('~/.cache/lwe-openal-1.21.1'))
    out = os.path.join(work, 'resample')
    os.makedirs(out, exist_ok=True)
    # never "linux-wallpaperengine" in a test process' command line, we_manager's shutdown pkill matches it
    binary = os.path.join(REPO, 'build/output/lwe-resample-bin')
    shutil.copy(os.path.join(REPO, 'build/output/linux-wallpaperengine'), binary)
    empty = os.path.join(work, 'empty')
    os.makedirs(empty, exist_ok=True)
    status = 0
    for name, (rate, channels, secs, mode, stop) in CASES.items():
        d = os.path.join(out, name)
        os.makedirs(d + '/sounds', exist_ok=True)
        tone(d + '/sounds/a.wav', rate, secs, channels)
        scene(d, mode, stop)
        ours = os.path.join(out, name + '.ours.f32')
        env = dict(os.environ, SDL_AUDIODRIVER='disk', SDL_DISKAUDIOFILE=ours, HEADLESS_RENDER_KEEP_RUNNING='1',
                   HEADLESS_RENDER_TIMEOUT='4', HEADLESS_RENDER_SIZE='640x360')
        subprocess.run(['bash', os.path.join(REPO, 'tools/headless_render.sh'), binary, os.path.join(out, name + '.png'),
                        '--assets-dir', ASSETS, '--volume', '128', '--hrtf', 'on' if hrtf else 'off', d],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        b = np.fromfile(ours, np.float32).reshape(-1, 2)
        sb, eb = onset_end(b)
        # the script's stop lands on whatever update the frame it runs falls into, give refstream the same one
        stop_update = (eb - sb) // 960 if stop else -1
        src = d + '/sounds/a.wav'
        if mode == 'loop' and not stop:
            src = os.path.join(out, name + '.x8.wav')
            tone(src, rate, secs, channels, 8)
        ref = os.path.join(out, name + '.ref.f32')
        subprocess.run([os.path.join(work, 'refstream'), src, '1.0', '160', ref, str(stop_update), '960',
                        '1' if hrtf else '0'],
                       env={'HOME': empty, 'XDG_DATA_DIRS': empty, 'XDG_CONFIG_DIRS': empty}, check=True)
        a = np.fromfile(ref, np.float32).reshape(-1, 2)
        sa, ea = onset_end(a)
        a, b = a[sa:], b[sb:]
        n = min(len(a), len(b))
        diff = float(np.abs(a[:n] - b[:n]).max())
        lengths = (ea - sa, eb - sb) if mode == 'single' or stop else None
        ok = diff < 2e-7 and (lengths is None or lengths[0] == lengths[1])
        status |= not ok
        print(f'{name:12} max diff {diff:.3g}' + (f'  length ref {lengths[0]} ours {lengths[1]}' if lengths else '')
              + ('' if ok else '  DIFFERENT'))
    sys.exit(status)


if __name__ == '__main__':
    main()
