#!/usr/bin/env python3
"""Real Wallpaper Engine reference frames under Wine, a fresh WE instance per item.

    render_we.py [ids...]      (default: every item in the scan.py database)
    render_we.py <dir|scene.json|scene.pkg> ...   test scenes, frames go to $REFS/<dir name>

Writes $REFS/<id>/f*.png (default ~/.local/share/we_live/refs; full :98 screen, window at +4+30) + we.log.
Items that already have frames are skipped unless FORCE=1. Instead of fixed sleeps it waits until the window
isn't a flat colour any more (the first rendered frame), then SETTLE seconds, then FRAMES frames INTERVAL
seconds apart (env overrides). -control openWallpaper sent to a running instance doesn't switch wallpapers under
Wine (it only brings the UI window back), hence one instance per item.
Audio is pointed at dead sockets, see README.md. Setup of the Wine prefix: README.md.
"""
import os, subprocess, sys, time
import resource
import numpy as np
import mss
from PIL import Image
from Xlib import display as xdisplay

S = os.environ.get('WE_LIVE_DIR', os.path.expanduser('~/.local/share/we_live'))
HERE = os.path.dirname(os.path.abspath(__file__)) + '/wine'
W = os.environ.get('LWE_WORKSHOP_DIR', '/workspace/SteamLibrary/steamapps/workshop/content/431960')
LIB = os.environ.get('LWE_LIBRARY_DIR', os.path.expanduser('~/.local/share/lwe-library'))
REFS = os.environ.get('REFS', S + '/refs')
D = ':98'
FRAMES = int(os.environ.get('FRAMES', 5))
INTERVAL = float(os.environ.get('INTERVAL', 1.0))
SETTLE = float(os.environ.get('SETTLE', 4.0))
TIMEOUT = float(os.environ.get('SWITCH_TIMEOUT', 30))
RETRIES = int(os.environ.get('RETRIES', 3))

# Wine must only see the isolated :98, not the desktop session (wayland-0, D-Bus) in the shared runtime dir
env = {k: v for k, v in os.environ.items() if k not in ('WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DBUS_SESSION_BUS_ADDRESS')}
env = dict(env, XDG_RUNTIME_DIR=S + '/run', WINEPREFIX=S + '/wineprefix', DISPLAY=D, WINEDEBUG='-all', DXVK_LOG_PATH=S,
           PULSE_SERVER=f'unix:{S}/run/no-pulse', PIPEWIRE_REMOTE=f'{S}/run/no-pipewire', ALSA_CONFIG_PATH=S + '/run/asound-null.conf')
WINE = '/usr/lib/wine/wine64'
we_proc = None
log = None


# no core files: the sandbox shares the desktop's user and its crash handler picks them up
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

def winpath(p):
    return 'Z:' + p.replace('/', '\\')


def grab(m):
    s = m.grab({'left': 0, 'top': 0, 'width': 1920, 'height': 1080})
    return Image.frombytes('RGB', s.size, s.rgb)


def small(img):
    return np.asarray(img.crop((4, 30, 1920, 1080)).resize((240, 131))).astype(np.float32)


def hide_ui():
    d = xdisplay.Display(D)
    root = d.screen().root

    def walk(w):
        # windows can disappear while walking the tree
        try:
            children = w.query_tree().children
        except Exception:
            return
        for c in children:
            yield c
            yield from walk(c)
    for w in walk(root):
        try:
            if w.get_wm_name() == 'Wallpaper UI':
                w.unmap()
        except Exception:
            pass
    try:
        top = root.query_tree().children
    except Exception:
        top = []
    for w in top:
        try:
            g = w.get_geometry()
            if w.get_attributes().map_state == 2 and min(g.width, g.height) <= 20 and (g.width, g.height) != (1, 1):
                w.unmap()
        except Exception:
            pass
    d.sync()
    d.close()


def start_we(first):
    global we_proc, log
    subprocess.run(['/usr/lib/wine/wineserver', '-k'], env=env, stderr=subprocess.DEVNULL)
    time.sleep(1)
    log = open(S + '/we_run_last.log', 'w')
    we_proc = subprocess.Popen([WINE, 'wallpaper64.exe', '-control', 'openWallpaper', '-file', winpath(first),
                                '-monitor', '0', '-playInWindow', '-silent'], cwd=S + '/we28', env=env,
                               stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)


def wait_rendered(m, t0, timeout):
    """until the window isn't a flat colour any more, or timeout"""
    while time.time() - t0 < timeout:
        hide_ui()
        if small(grab(m)).std() > 3:
            return True
        time.sleep(0.3)
    return False


def item_file(id_, info):
    if info.get('type') == 'scene':
        # "file" is scene.json, or gifscene.json for gif scenes, packed into the .pkg of the same name
        name = info.get('file') or 'scene.json'
        pkg = os.path.join(W, id_, os.path.splitext(name)[0] + '.pkg')
        return pkg if os.path.exists(pkg) else os.path.join(W, id_, name)
    return os.path.join(W, id_, 'project.json')


def main():
    import json, glob
    db = json.load(open(LIB + '/features.json'))
    ids = sys.argv[1:] or sorted(db)
    if not os.environ.get('FORCE'):
        ids = [i for i in ids if os.path.exists(i) or not glob.glob(f'{REFS}/{i}/f*.png')]
    out_root = REFS
    files = []
    for n, i in enumerate(ids):
        if os.path.exists(i):
            path = os.path.abspath(i)
            files.append(path + '/scene.json' if os.path.isdir(path) else path)
            ids[n] = os.path.basename(path if os.path.isdir(path) else os.path.dirname(path))
        else:
            files.append(item_file(i, db.get(i, {}).get('info', {})))
    subprocess.run(['bash', HERE + '/we_display.sh'], stdout=subprocess.DEVNULL, check=True)
    with mss.MSS(display=D) as m:
        for n, f in enumerate(files):
            id_ = ids[n]
            out = os.path.join(out_root, id_)
            os.makedirs(out, exist_ok=True)
            for old in os.listdir(out):
                os.remove(os.path.join(out, old))
            t0 = time.time()
            # scenes with video textures sometimes never get past a flat first frame under Wine, a new
            # instance usually does
            for attempt in range(RETRIES):
                start = time.time()
                start_we(f)
                # the killed instance's window must be gone before "not flat" means anything
                while time.time() - start < 10 and small(grab(m)).std() > 3:
                    time.sleep(0.2)
                ok = wait_rendered(m, start, TIMEOUT)
                if ok:
                    break
            time.sleep(SETTLE)
            hide_ui()
            t1 = time.time()
            last = None
            for i in range(FRAMES):
                last = grab(m)
                last.save('%s/f%03d_%.1f.png' % (out, i, time.time() - t1))
                time.sleep(INTERVAL)
            log.flush()
            try:
                with open(S + '/we_run_last.log', errors='replace') as src, open(out + '/we.log', 'w') as dst:
                    dst.write(src.read()[-20000:])
            except OSError:
                pass
            print(f'we {id_} {FRAMES} frames{"" if ok else " (still flat)"} {time.time() - t0:.1f}s', flush=True)
    subprocess.run(['/usr/lib/wine/wineserver', '-k'], env=env, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
