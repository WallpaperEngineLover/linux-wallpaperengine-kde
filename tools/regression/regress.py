#!/usr/bin/env python3
"""Wallpaper regression runs: render every installed scene wallpaper headlessly with a build,
then compare two runs (usually the last good build against the one being tested).

    tools/regression/regress.py run /path/to/old/linux-wallpaperengine runs/base --repeat 2
    tools/regression/regress.py run build/output/linux-wallpaperengine runs/new
    tools/regression/regress.py compare runs/base runs/new --report runs/report

Runs are reproducible: animation time advances by a fixed step per frame (LWE_FIXED_TIMESTEP),
particles are seeded per object, the wall clock is pinned (fixed_clock.c) and audio input is
off. Each worker gets its own Xvfb display, nothing touches the real screen.

--gpu renders on the GPU instead (the engine's headless EGL driver, XDG_SESSION_TYPE=headless),
no Xvfb and many times faster than llvmpipe. GPU and llvmpipe pixels differ slightly, so compare
runs made the same way; the binary needs the headless driver for this.

compare looks at four things per wallpaper: whether it still renders (crash/timeout), error
lines that appear or disappear in the log, how much of the screenshot changed, and whether gif
and video textures still move (wallpapers shipping them get a second render a few frames later,
a hidden or frozen one shows up as less motion). Video textures play in real time and never
render the same twice, --repeat 2 on the base run marks those pixels so compare ignores them.
It writes an HTML report with side-by-sides of everything flagged and exits 1 if anything
regressed. Both builds need LWE_FIXED_TIMESTEP support, older ones render every run differently.

Needs Xvfb, gcc and Pillow (tools/requirements.txt).
"""

import argparse
import concurrent.futures
import html
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path (__file__).resolve ().parent
TOOLS = HERE.parent

DEFAULT_WORKSHOP = Path.home () / '.local/share/Steam/steamapps/workshop/content/431960'
DEFAULT_ASSETS = Path.home () / '.local/share/Steam/steamapps/common/wallpaper_engine/assets'

# lines worth tracking between builds; everything else in the log is chatter
ERROR_LINE = re.compile (
    r'error|exception|failed|cannot|can\'t|unsupported|not supported|typeerror|referenceerror|'
    r'syntaxerror|abort|segmentation|falling back',
    re.IGNORECASE
)
# driver/X noise that shows up on some machines and says nothing about the engine
IGNORED_LINE = re.compile (r'amdgpu|_XSERVTrans|MESA-LOADER|libEGL warning|ALSA lib|Puppet draw result')
# mpv announcing a video stream, those textures play in real time and never render the same twice
VIDEO_LINE = re.compile (r'^\W*Video\s+--vid=', re.MULTILINE)


def env_path (name, fallback):
    value = os.environ.get (name)
    return Path (value) if value else fallback


def read_project (folder):
    try:
        with open (folder / 'project.json', encoding = 'utf-8-sig') as f:
            return json.load (f)
    except (OSError, ValueError):
        return None


def read_sized (f):
    size = int.from_bytes (f.read (4), 'little')
    return f.read (size).decode ('utf-8', 'replace')


def texture_kinds (header):
    """'gif' for animated (sprite sheet) textures, 'video' for mp4 ones, from a .tex header."""
    if header[:8] != b'TEXV0005' or len (header) < 26:
        return set ()

    flags = int.from_bytes (header[22:26], 'little')
    return ({'gif'} if flags & 4 else set ()) | ({'video'} if flags & 32 else set ())


def animated_textures (folder):
    """What kind of animated textures a wallpaper ships, loose or inside its .pkg files."""
    kinds = set ()

    for path in folder.rglob ('*'):
        if path.suffix == '.mp4':
            kinds.add ('video')
        elif path.suffix == '.tex':
            with open (path, 'rb') as f:
                kinds |= texture_kinds (f.read (64))
        elif path.suffix == '.pkg':
            try:
                with open (path, 'rb') as f:
                    read_sized (f)
                    entries = []
                    for _ in range (int.from_bytes (f.read (4), 'little')):
                        name = read_sized (f)
                        offset = int.from_bytes (f.read (4), 'little')
                        f.read (4)
                        entries.append ((name, offset))
                    base = f.tell ()
                    for name, offset in entries:
                        if name.endswith ('.mp4'):
                            kinds.add ('video')
                        elif name.endswith ('.tex'):
                            f.seek (base + offset)
                            kinds |= texture_kinds (f.read (64))
            except (OSError, ValueError):
                pass

    return kinds


def find_wallpapers (workshop, types, ids, variants):
    """(name, folder, title, properties) per render; variants add renders of the same wallpaper
    with user properties set, named <id>@<variant>."""
    found = []

    for folder in sorted (workshop.iterdir ()):
        if not folder.is_dir () or (ids and folder.name not in ids):
            continue

        project = read_project (folder)
        kind = str ((project or {}).get ('type', '')).lower ()

        if kind in types:
            title = (project or {}).get ('title', '')
            found.append ((folder.name, folder, title, []))

            for variant, properties in variants.get (folder.name, {}).items ():
                found.append ((f'{folder.name}@{variant}', folder, f'{title} [{variant}]', properties))

    return found


def build_shims (out):
    shims = out / '.shims'
    shims.mkdir (parents = True, exist_ok = True)
    built = []

    for source in (TOOLS / 'dbus_noop_shim.c', HERE / 'fixed_clock.c'):
        target = shims / (source.stem + '.so')

        if not target.exists () or source.stat ().st_mtime > target.stat ().st_mtime:
            subprocess.run (['gcc', '-shared', '-fPIC', '-O2', '-o', str (target), str (source), '-ldl'], check = True)

        built.append (str (target))

    return ':'.join (built)


def x_sockets ():
    """Display numbers with a listening X socket, abstract ones included (some sandboxes can't
    write /tmp/.X11-unix, Xvfb then only has the abstract socket)."""
    numbers = set ()

    with open ('/proc/net/unix') as f:
        for line in f:
            match = re.search (r'/tmp/\.X11-unix/X(\d+)$', line.strip ())
            if match:
                numbers.add (int (match.group (1)))

    return numbers


class Display:
    """One Xvfb server per worker so renders can run side by side."""

    process = None

    def __init__ (self, first, width, height):
        number = first

        while True:
            while number in x_sockets () or Path (f'/tmp/.X11-unix/X{number}').exists ():
                number += 1

            self.name = f':{number}'
            self.number = number
            self.process = subprocess.Popen (
                ['Xvfb', self.name, '-screen', '0', f'{width}x{height}x24', '-nolisten', 'tcp'],
                stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL
            )

            for _ in range (100):
                if number in x_sockets ():
                    return
                if self.process.poll () is not None:
                    break
                time.sleep (0.05)

            self.close ()
            number += 1

            if number > first + 50:
                raise RuntimeError ('could not start Xvfb')

    def close (self):
        if self.process is None:
            return

        self.process.terminate ()
        try:
            self.process.wait (5)
        except subprocess.TimeoutExpired:
            self.process.kill ()


def stop (process):
    if process.poll () is not None:
        return

    os.killpg (process.pid, signal.SIGTERM)
    try:
        process.wait (5)
    except subprocess.TimeoutExpired:
        os.killpg (process.pid, signal.SIGKILL)
        process.wait ()


def render_once (args, env, folder, shot, log_path, properties):
    command = [
        str (args.binary), '--window', f'0x0x{args.width}x{args.height}', '--fps', '1000',
        '--silent', '--no-audio-processing', '--screenshot', str (shot), '--screenshot-delay', str (args.frame),
        '--assets-dir', str (args.assets),
    ]

    for value in properties:
        command += ['--set-property', value]

    command.append (str (folder))

    started = time.monotonic ()
    status = 'timeout'

    with open (log_path, 'wb') as log:
        process = subprocess.Popen (
            command, env = env, stdin = subprocess.DEVNULL, stdout = log, stderr = subprocess.STDOUT, start_new_session = True
        )
        last_size = -1

        while time.monotonic () - started < args.timeout:
            code = process.poll ()
            size = shot.stat ().st_size if shot.exists () else 0

            # the engine keeps running after the screenshot, stop it once the file is complete
            if size > 0 and size == last_size:
                status = 'ok'
                break

            if code is not None:
                status = 'ok' if size > 0 else ('crash' if code < 0 else 'noshot')
                break

            last_size = size
            time.sleep (0.25)

        stop (process)

    return status, process.returncode, time.monotonic () - started


def render (args, preload, display, wallpaper):
    wallpaper_id, folder, title, properties = wallpaper
    target = args.out / wallpaper_id
    shutil.rmtree (target, ignore_errors = True)
    target.mkdir (parents = True)

    env = dict (os.environ)
    env.pop ('WAYLAND_DISPLAY', None)
    env.pop ('WAYLAND_SOCKET', None)
    env.pop ('DISPLAY', None)
    env.update ({
        'XDG_SESSION_TYPE': 'x11',
        'LD_LIBRARY_PATH': ':'.join (filter (None, [str (args.binary.parent), os.environ.get ('LD_LIBRARY_PATH')])),
        'LD_PRELOAD': preload,
        'LWE_FIXED_TIMESTEP': str (args.step),
        'LWE_FIXED_CLOCK': str (args.clock),
        'TZ': 'UTC',
    })
    if args.gpu:
        env['XDG_SESSION_TYPE'] = 'headless'
        if args.gpu != 'auto':
            env['LWE_HEADLESS_DEVICE'] = args.gpu
    else:
        env['DISPLAY'] = display.name
        # software rendering (llvmpipe) starts a thread per core in every instance, which just
        # thrashes when several renders run at once
        env.setdefault ('LP_NUM_THREADS', str (max (1, (os.cpu_count () or 1) // args.jobs)))

    status, code, seconds = render_once (args, env, folder, target / 'shot.png', target / 'log.txt', properties)

    # extra renders of the same build show what isn't reproducible (video textures play in real
    # time), compare ignores those pixels
    if status == 'ok':
        for repeat in range (2, args.repeat + 1):
            render_once (args, env, folder, target / f'shot{repeat}.png', target / f'log{repeat}.txt', properties)

    # animated textures have to keep moving: a second, later frame shows whether a gif or video
    # is still drawn and still playing (a hidden or frozen one leaves its area static)
    animated = sorted (animated_textures (folder))
    motion = None

    if status == 'ok' and animated and args.motion_gap > 0:
        later = target / 'shot_motion.png'
        args_later = argparse.Namespace (**{**vars (args), 'frame': args.frame + args.motion_gap})

        if render_once (args_later, env, folder, later, target / 'log_motion.txt', properties)[0] == 'ok':
            motion = changed_fraction (target / 'shot.png', later, 16)

    log = (target / 'log.txt').read_text (errors = 'replace')
    result = {
        'id': wallpaper_id,
        'title': title,
        'status': status,
        'exit': code,
        'seconds': round (seconds, 1),
        'video': bool (VIDEO_LINE.search (log)),
        'animated': animated,
        'motion': motion,
    }
    (target / 'result.json').write_text (json.dumps (result, ensure_ascii = False, indent = 1))

    return result


def cmd_run (args):
    if args.jobs is None:
        cores = os.cpu_count () or 2
        args.jobs = max (1, cores // 2 if args.gpu else min (8, cores // 4))

    args.binary = args.binary.resolve ()
    args.out = args.out.resolve ()
    args.out.mkdir (parents = True, exist_ok = True)

    variants = {}
    if args.variants.exists ():
        variants = {k: v for k, v in json.loads (args.variants.read_text ()).items () if not k.startswith ('_')}

    wallpapers = find_wallpapers (args.workshop, set (args.types.split (',')), set (args.ids or []), variants)

    if not wallpapers:
        sys.exit (f'no wallpapers found in {args.workshop}')

    preload = build_shims (args.out)
    displays = queue.Queue ()
    opened = []

    try:
        for _ in range (args.jobs):
            if args.gpu:
                displays.put (None)
                continue

            display = Display (opened[-1].number + 1 if opened else 90, args.width, args.height)
            opened.append (display)
            displays.put (display)

        def job (wallpaper):
            display = displays.get ()
            try:
                return render (args, preload, display, wallpaper)
            finally:
                displays.put (display)

        (args.out / 'run.json').write_text (json.dumps ({
            'binary': str (args.binary),
            'frame': args.frame,
            'step': args.step,
            'clock': args.clock,
            'size': [args.width, args.height],
            'repeat': args.repeat,
            'renderer': 'gpu' if args.gpu else 'llvmpipe',
            'started': time.strftime ('%Y-%m-%d %H:%M:%S'),
        }, indent = 1))

        done = 0

        with concurrent.futures.ThreadPoolExecutor (args.jobs) as pool:
            for result in pool.map (job, wallpapers):
                done += 1
                print (f'[{done}/{len (wallpapers)}] {result["id"]} {result["status"]} {result["seconds"]}s', flush = True)
    finally:
        for display in opened:
            display.close ()


def error_lines (log):
    if not log.exists ():
        return set ()

    lines = set ()

    for line in log.read_text (errors = 'replace').splitlines ():
        if ERROR_LINE.search (line) and not IGNORED_LINE.search (line):
            # numbers, pointers and temp paths change between runs, the message doesn't
            line = re.sub (r'0x[0-9a-fA-F]+', 'X', line)
            line = re.sub (r'\d+', 'N', line)
            lines.add (line.strip ()[:300])

    return lines


def strongest_difference (a, b):
    from PIL import ImageChops

    if a.size != b.size:
        b = b.resize (a.size)

    # per pixel, the largest channel difference
    channels = ImageChops.difference (a, b).split ()
    return ImageChops.lighter (ImageChops.lighter (channels[0], channels[1]), channels[2])


def changed_fraction (first, second, threshold):
    from PIL import Image

    a = Image.open (first).convert ('RGB')
    histogram = strongest_difference (a, Image.open (second).convert ('RGB')).histogram ()
    return sum (histogram[threshold + 1:]) / (a.size[0] * a.size[1])


def noise_mask (shots, threshold):
    """Pixels that already differ between repeated renders of the same build."""
    from PIL import Image, ImageChops

    images = [Image.open (shot).convert ('RGB') for shot in shots]
    mask = None

    for other in images[1:]:
        changed = strongest_difference (images[0], other).point (lambda v: 255 if v > threshold else 0)
        mask = changed if mask is None else ImageChops.lighter (mask, changed)

    return mask


def image_diff (base, new, threshold, heatmap, noise):
    from PIL import Image, ImageChops

    a = Image.open (base).convert ('RGB')
    b = Image.open (new).convert ('RGB')
    changed_mask = strongest_difference (a, b).point (lambda v: 255 if v > threshold else 0)
    total = a.size[0] * a.size[1]
    ignored = 0

    if noise is not None:
        noise = noise.resize (a.size)
        ignored = noise.histogram ()[255] / total
        changed_mask = ImageChops.subtract (changed_mask, noise)

    changed = changed_mask.histogram ()[255] / total

    if changed > 0 or ignored > 0:
        dim = a.convert ('L').convert ('RGB').point (lambda v: v // 3)
        if noise is not None:
            dim = Image.composite (Image.new ('RGB', a.size, (60, 60, 110)), dim, noise)
        Image.composite (Image.new ('RGB', a.size, (255, 40, 40)), dim, changed_mask).save (heatmap, quality = 85)

    return changed, ignored


def thumbnail (source, target, width = 640):
    from PIL import Image

    image = Image.open (source).convert ('RGB')
    image.thumbnail ((width, width))
    image.save (target, quality = 85)


def load_result (folder):
    try:
        return json.loads ((folder / 'result.json').read_text ())
    except (OSError, ValueError):
        return None


def renderer (folder):
    try:
        return json.loads ((folder / 'run.json').read_text ()).get ('renderer', 'llvmpipe')
    except (OSError, ValueError):
        return '?'


def cmd_compare (args):
    if renderer (args.base) != renderer (args.new):
        print (f'warning: base was rendered with {renderer (args.base)} and new with {renderer (args.new)}, '
               'expect small differences everywhere', file = sys.stderr)

    report = args.report.resolve ()
    shutil.rmtree (report, ignore_errors = True)
    (report / 'img').mkdir (parents = True)

    ids = sorted ({p.name for p in args.base.iterdir () if (p / 'result.json').exists ()}
                  | {p.name for p in args.new.iterdir () if (p / 'result.json').exists ()})
    rows = []

    for wallpaper_id in ids:
        base = load_result (args.base / wallpaper_id)
        new = load_result (args.new / wallpaper_id)
        row = {
            'id': wallpaper_id,
            'title': (new or base or {}).get ('title', ''),
            'base': base['status'] if base else 'missing',
            'new': new['status'] if new else 'missing',
            'added_errors': [],
            'fixed_errors': [],
            'changed': None,
            'verdict': 'same',
        }

        if base and new:
            base_errors = error_lines (args.base / wallpaper_id / 'log.txt')
            new_errors = error_lines (args.new / wallpaper_id / 'log.txt')
            row['added_errors'] = sorted (new_errors - base_errors)
            row['fixed_errors'] = sorted (base_errors - new_errors)

        base_shot = args.base / wallpaper_id / 'shot.png'
        new_shot = args.new / wallpaper_id / 'shot.png'

        row['video'] = bool ((base or {}).get ('video') or (new or {}).get ('video'))
        row['ignored'] = 0

        if base and new and base_shot.exists () and new_shot.exists ():
            heatmap = report / 'img' / f'{wallpaper_id}_diff.jpg'
            repeats = sorted ((args.base / wallpaper_id).glob ('shot[0-9]*.png'))
            repeats += sorted ((args.new / wallpaper_id).glob ('shot[0-9]*.png'))
            noise = None

            if repeats:
                noise = noise_mask ([base_shot] + [r for r in repeats if r.parent == base_shot.parent], args.threshold)
                new_repeats = [r for r in repeats if r.parent == new_shot.parent]
                if new_repeats:
                    from PIL import ImageChops
                    extra = noise_mask ([new_shot] + new_repeats, args.threshold)
                    noise = extra if noise is None else ImageChops.lighter (noise, extra.resize (noise.size))

            row['changed'], row['ignored'] = image_diff (base_shot, new_shot, args.threshold, heatmap, noise)

        row['animated'] = (new or base or {}).get ('animated', [])
        row['motion'] = [(base or {}).get ('motion'), (new or {}).get ('motion')]
        base_motion, new_motion = row['motion']
        # the second frame moved noticeably less than with the base build
        # runs made before the motion check have no "motion" key at all, None means the later render failed
        less_motion = ('motion' in (new or {}) and base_motion is not None and base_motion * 100 >= args.motion_min
                       and (new_motion or 0) < base_motion * 0.5)

        if row['new'] == 'missing':
            row['verdict'] = 'not run'
        elif row['base'] == 'missing':
            row['verdict'] = 'new wallpaper'
        elif row['new'] != 'ok' and row['base'] == 'ok':
            row['verdict'] = 'broken'
        elif row['new'] == 'ok' and row['base'] not in ('ok', 'missing'):
            row['verdict'] = 'fixed'
        elif row['added_errors']:
            row['verdict'] = 'new errors'
        elif less_motion:
            row['verdict'] = 'less motion'
        elif row['changed'] is not None and row['changed'] * 100 > (args.video_tolerance if row['video'] else args.tolerance):
            row['verdict'] = 'changed'
        elif row['fixed_errors']:
            row['verdict'] = 'fewer errors'

        if row['verdict'] not in ('same', 'not run'):
            for side, shot in (('base', base_shot), ('new', new_shot),
                               ('base_motion', args.base / wallpaper_id / 'shot_motion.png'),
                               ('new_motion', args.new / wallpaper_id / 'shot_motion.png')):
                if shot.exists ():
                    thumbnail (shot, report / 'img' / f'{wallpaper_id}_{side}.jpg')

        rows.append (row)

    order = {'broken': 0, 'new errors': 1, 'less motion': 2, 'changed': 3, 'fixed': 4, 'fewer errors': 5,
             'new wallpaper': 6, 'same': 7, 'not run': 8}
    rows.sort (key = lambda r: (order[r['verdict']], -(r['changed'] or 0)))

    (report / 'summary.json').write_text (json.dumps (rows, ensure_ascii = False, indent = 1))
    write_html (report, args, rows)

    counts = {}
    for row in rows:
        counts[row['verdict']] = counts.get (row['verdict'], 0) + 1

    print (', '.join (f'{count} {verdict}' for verdict, count in sorted (counts.items (), key = lambda c: order[c[0]])))

    for row in rows:
        if row['verdict'] not in ('same', 'not run'):
            changed = f' {row["changed"] * 100:.2f}% of pixels' if row['changed'] else ''
            print (f'  {row["verdict"]:12} {row["id"]} {row["title"][:50]}{changed}')

    print (f'report: {report / "index.html"}')

    regressed = any (r['verdict'] in ('broken', 'new errors', 'less motion', 'changed') for r in rows)
    sys.exit (1 if regressed else 0)


def write_html (report, args, rows):
    def esc (value):
        return html.escape (str (value))

    def run_info (folder):
        try:
            return json.loads ((folder / 'run.json').read_text ())
        except (OSError, ValueError):
            return {}

    base_info = run_info (args.base)
    new_info = run_info (args.new)
    parts = [f'''<!doctype html><html><head><meta charset="utf-8"><title>Wallpaper regressions</title>
<style>
body {{ font: 14px system-ui, sans-serif; margin: 24px; background: #16161a; color: #ddd; }}
a {{ color: #8ab4f8; }} h2 {{ margin-top: 40px; }}
table {{ border-collapse: collapse; }} td, th {{ padding: 4px 10px; border-bottom: 1px solid #333; text-align: left; }}
.broken, .new-errors {{ color: #ff6b6b; }} .changed {{ color: #ffb86b; }} .fixed, .fewer-errors {{ color: #7ee787; }}
.pair {{ display: flex; gap: 8px; flex-wrap: wrap; }} .pair figure {{ margin: 0; }} .pair img {{ max-width: 420px; display: block; }}
figcaption {{ color: #999; font-size: 12px; }} pre {{ background: #222; padding: 8px; overflow-x: auto; }}
</style></head><body>
<h1>Wallpaper regressions</h1>
<p>base: {esc (args.base)} ({esc (base_info.get ('binary', '?'))}, {esc (base_info.get ('started', ''))})<br>
new: {esc (args.new)} ({esc (new_info.get ('binary', '?'))}, {esc (new_info.get ('started', ''))})<br>
frame {esc (new_info.get ('frame', '?'))} at {esc (new_info.get ('step', '?'))}s per frame, pixel threshold {args.threshold},
changed above {args.tolerance}% of pixels ({args.video_tolerance}% with video textures)</p>
<p>Heatmaps: red is changed, blue is ignored because it already differs between repeated renders of one build
(<code>run --repeat 2</code>), grey is unchanged.</p>
<table><tr><th>verdict</th><th>id</th><th>title</th><th>base</th><th>new</th><th>pixels changed</th><th>ignored</th><th>motion base / new</th></tr>''']

    for row in rows:
        css = row['verdict'].replace (' ', '-')
        changed = f'{row["changed"] * 100:.2f}%' if row['changed'] is not None else '-'
        ignored = f'{row["ignored"] * 100:.1f}%' if row['ignored'] else ''
        title = esc (row['title']) + (' <small>(video texture)</small>' if row['video'] else '')
        detailed = row['verdict'] not in ('same', 'not run')
        link = f'<a href="#w{esc (row["id"])}">{esc (row["id"])}</a>' if detailed else esc (row['id'])
        motion = ' / '.join ('-' if m is None else f'{m * 100:.2f}%' for m in row['motion']) if row['animated'] else ''
        if row['animated']:
            title += f' <small>({", ".join (row["animated"])})</small>'

        parts.append (f'<tr><td class="{css}">{esc (row["verdict"])}</td><td>{link}</td><td>{title}</td>'
                      f'<td>{esc (row["base"])}</td><td>{esc (row["new"])}</td><td>{changed}</td><td>{ignored}</td><td>{motion}</td></tr>')

    parts.append ('</table>')

    for row in rows:
        if row['verdict'] in ('same', 'not run'):
            continue

        parts.append (f'<h2 id="w{esc (row["id"])}" class="{row["verdict"].replace (" ", "-")}">'
                      f'{esc (row["id"])} - {esc (row["title"])} ({esc (row["verdict"])})</h2><div class="pair">')

        for name, caption in (('base', 'base'), ('new', 'new'), ('diff', 'changed pixels'),
                              ('base_motion', 'base, a few frames later'), ('new_motion', 'new, a few frames later')):
            image = report / 'img' / f'{row["id"]}_{name}.jpg'
            if image.exists ():
                parts.append (f'<figure><img src="img/{image.name}"><figcaption>{caption}</figcaption></figure>')

        parts.append ('</div>')

        if row['added_errors']:
            parts.append ('<p>new in the log:</p><pre>' + esc ('\n'.join (row['added_errors'])) + '</pre>')
        if row['fixed_errors']:
            parts.append ('<p>gone from the log:</p><pre>' + esc ('\n'.join (row['fixed_errors'])) + '</pre>')

    parts.append ('</body></html>')
    (report / 'index.html').write_text ('\n'.join (parts))


def main ():
    parser = argparse.ArgumentParser (description = __doc__, formatter_class = argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers (dest = 'command', required = True)

    run = commands.add_parser ('run', help = 'render every wallpaper with one build')
    run.add_argument ('binary', type = Path, help = 'the linux-wallpaperengine executable to test')
    run.add_argument ('out', type = Path, help = 'folder for the screenshots and logs')
    run.add_argument ('--workshop', type = Path, default = env_path ('LWE_WORKSHOP_DIR', DEFAULT_WORKSHOP))
    run.add_argument ('--assets', type = Path, default = env_path ('LWE_ASSETS_DIR', DEFAULT_ASSETS))
    run.add_argument ('--ids', nargs = '*', help = 'only these workshop ids')
    run.add_argument ('--variants', type = Path, default = HERE / 'variants.json',
                      help = 'extra renders with user properties set (default: variants.json next to this script)')
    run.add_argument ('--types', default = 'scene', help = 'project types to include, comma separated (default: scene)')
    run.add_argument ('--jobs', type = int, help = 'renders at once (default: cores/4 up to 8, cores/2 with --gpu)')
    run.add_argument ('--gpu', nargs = '?', const = 'auto', metavar = 'RENDER_NODE',
                      help = 'render on the GPU without Xvfb, optionally on this /dev/dri/renderD* node')
    run.add_argument ('--frame', type = int, default = 30, help = 'frame to screenshot (default: 30)')
    run.add_argument ('--step', type = float, default = 0.1, help = 'animation seconds per frame (default: 0.1)')
    run.add_argument ('--clock', type = int, default = 1767268800, help = 'pinned wall clock, unix seconds')
    run.add_argument ('--width', type = int, default = 1280)
    run.add_argument ('--height', type = int, default = 720)
    run.add_argument ('--repeat', type = int, default = 1,
                      help = 'renders per wallpaper; use 2+ for the base run so compare can ignore noise')
    run.add_argument ('--motion-gap', type = int, default = 5,
                      help = 'frames between the two renders of wallpapers with gif/video textures (0 disables)')
    run.add_argument ('--timeout', type = float, default = 300, help = 'seconds per wallpaper')
    run.set_defaults (func = cmd_run)

    compare = commands.add_parser ('compare', help = 'compare two runs and write a report')
    compare.add_argument ('base', type = Path)
    compare.add_argument ('new', type = Path)
    compare.add_argument ('--report', type = Path, default = Path ('regression-report'))
    compare.add_argument ('--threshold', type = int, default = 16, help = 'per-channel difference that counts as changed')
    compare.add_argument ('--video-tolerance', type = float, default = 5,
                          help = 'percent of changed pixels allowed with video textures, the motion check covers those')
    compare.add_argument ('--motion-min', type = float, default = 0.05,
                          help = 'percent of pixels that must move in the base run for the motion check to apply')
    compare.add_argument ('--tolerance', type = float, default = 0.1, help = 'percent of changed pixels allowed')
    compare.set_defaults (func = cmd_compare)

    args = parser.parse_args ()
    args.func (args)


if __name__ == '__main__':
    main ()
