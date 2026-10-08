#!/usr/bin/env python3
"""Checks that user property changes apply live: for every user property of every scene wallpaper,
one render launches with the property already changed (--set-property) and one launches with the
default and changes it through the control file on an early frame (LWE_HOTSWAP_AT_FRAME). Both
screenshots should match. A third render with nothing changed tells whether the property shows on
that frame at all.

    tools/regression/liveprops.py build/output/linux-wallpaperengine runs/live --gpu
    tools/regression/liveprops.py build/output/linux-wallpaperengine runs/live --gpu --force --ids 2937687740

--force sets LWE_FORCE_LIVE_PROPERTIES, which applies every change live even where the engine would
still reload, so it lists the uses that don't follow a change yet. Without it a change that needs a
reload restarts the wallpaper and the frames differ for that reason, the log line says which way it
went. Results go to <out>/results.json and a summary is printed.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import regress

SKIPPED_TYPES = {'file', 'directory', 'scenetexture', 'usershortcut', 'text', 'group'}


def alternative (prop):
    """A value different from the property's default, as the control file / --set-property take it."""
    kind = str (prop.get ('type', '')).lower ()
    value = prop.get ('value')

    if kind == 'bool':
        return 'false' if value in (True, 1, 'true', '1') else 'true'

    if kind == 'slider':
        low, high = float (prop.get ('min', 0)), float (prop.get ('max', 100))
        current = float (value if value is not None else low)
        target = high if abs (current - high) > abs (current - low) else low

        if target == current:
            target = (low + high) / 2

        if not prop.get ('fraction', False):
            target = round (target)

        return f'{target:g}'

    if kind == 'combo':
        for option in prop.get ('options', []):
            if str (option.get ('value')) != str (value):
                return str (option.get ('value'))
        return None

    if kind == 'color':
        return '0 0 1' if str (value).split ()[:1] != ['0'] else '1 0 0'

    if kind == 'textinput':
        return 'LIVE PROPERTY TEST'

    return None


def available_memory_gb ():
    with open ('/proc/meminfo') as f:
        for line in f:
            if line.startswith ('MemAvailable:'):
                return int (line.split ()[1]) / 1024 ** 2
    return 0.0


def engine_processes (binary_name):
    """(pid, rss in GB, command line) of every running engine started from this binary."""
    found = []

    for pid in os.listdir ('/proc'):
        if not pid.isdigit ():
            continue
        try:
            with open (f'/proc/{pid}/cmdline', 'rb') as f:
                command = f.read ().split (b'\0')
            if not command or Path (command[0].decode (errors = 'replace')).name != binary_name:
                continue
            with open (f'/proc/{pid}/statm') as f:
                rss = int (f.read ().split ()[1]) * os.sysconf ('SC_PAGE_SIZE') / 1024 ** 3
            found.append ((int (pid), rss, b' '.join (command).decode (errors = 'replace')))
        except (OSError, ValueError, IndexError):
            continue

    return found


def watchdog (binary_name, minimum_gb, log_path, stop):
    """Kills the biggest engine whenever the machine runs low on memory, a runaway render must not take the desktop
    down with it (this sandbox shares the host's memory). Every kill and every engine past 2GB is logged."""
    seen = set ()

    while not stop.is_set ():
        processes = engine_processes (binary_name)

        for pid, rss, command in processes:
            if rss > 2 and pid not in seen:
                seen.add (pid)
                with open (log_path, 'a') as log:
                    log.write (f'{time.strftime ("%H:%M:%S")} big {rss:.1f}GB pid {pid}: {command}\n')

        if processes and available_memory_gb () < minimum_gb:
            pid, rss, command = max (processes, key = lambda p: p[1])
            try:
                os.kill (pid, signal.SIGKILL)
            except OSError:
                pass
            with open (log_path, 'a') as log:
                log.write (f'{time.strftime ("%H:%M:%S")} KILLED {rss:.1f}GB pid {pid}: {command}\n')

        stop.wait (0.5)


def cases (workshop, ids):
    for folder in sorted (workshop.iterdir ()):
        if not folder.is_dir () or (ids and folder.name not in ids):
            continue

        project = regress.read_project (folder) or {}

        if str (project.get ('type', '')).lower () != 'scene':
            continue

        properties = project.get ('general', {}).get ('properties', {}) or {}

        for name, prop in properties.items ():
            if not isinstance (prop, dict) or str (prop.get ('type', '')).lower () in SKIPPED_TYPES:
                continue

            value = alternative (prop)

            if value is not None:
                yield folder, name, str (prop.get ('type')), value


def main ():
    parser = argparse.ArgumentParser (description = __doc__, formatter_class = argparse.RawDescriptionHelpFormatter)
    parser.add_argument ('binary', type = Path)
    parser.add_argument ('out', type = Path)
    parser.add_argument ('--workshop', type = Path, default = regress.env_path ('LWE_WORKSHOP_DIR', regress.DEFAULT_WORKSHOP))
    parser.add_argument ('--assets', type = Path, default = regress.env_path ('LWE_ASSETS_DIR', regress.DEFAULT_ASSETS))
    parser.add_argument ('--ids', nargs = '*')
    parser.add_argument ('--gpu', nargs = '?', const = 'auto', metavar = 'RENDER_NODE')
    parser.add_argument ('--jobs', type = int, default = 4)
    parser.add_argument ('--memory-limit', type = float, default = 4, help = 'GB of data per engine (RLIMIT_DATA)')
    parser.add_argument ('--min-available', type = float, default = 6,
                         help = 'GB of available memory below which the biggest engine is killed')
    parser.add_argument ('--frame', type = int, default = 20, help = 'frame to screenshot (default: 20)')
    parser.add_argument ('--change-frame', type = int, default = 0, help = 'frame the live change happens on')
    parser.add_argument ('--step', type = float, default = 0.1)
    parser.add_argument ('--clock', type = int, default = 1767268800)
    parser.add_argument ('--width', type = int, default = 960)
    parser.add_argument ('--height', type = int, default = 540)
    parser.add_argument ('--timeout', type = float, default = 120)
    parser.add_argument ('--threshold', type = int, default = 16)
    parser.add_argument ('--tolerance', type = float, default = 0.2, help = 'percent of changed pixels still a match')
    parser.add_argument ('--force', action = 'store_true', help = 'set LWE_FORCE_LIVE_PROPERTIES')
    args = parser.parse_args ()

    args.binary = args.binary.resolve ()
    args.out = args.out.resolve ()
    args.out.mkdir (parents = True, exist_ok = True)
    preload = regress.build_shims (args.out)
    todo = list (cases (args.workshop, set (args.ids or [])))
    stop_watchdog = threading.Event ()
    threading.Thread (
        target = watchdog, args = (args.binary.name, args.min_available, args.out / 'memory.log', stop_watchdog),
        daemon = True
    ).start ()

    # one bus for the run, dbus-run-session per render left engines unreaped
    bus = subprocess.Popen (['dbus-daemon', '--session', '--nofork', '--print-address', '--address',
                             f'unix:path={args.out}/bus'], stdout = subprocess.PIPE, stdin = subprocess.DEVNULL, text = True)
    address = bus.stdout.readline ().strip ()

    def private_env ():
        return dict (regress.render_env (args, preload, None), DBUS_SESSION_BUS_ADDRESS = address)

    if not todo:
        sys.exit ('no user properties found')

    def base (folder):
        target = args.out / folder.name
        target.mkdir (parents = True, exist_ok = True)
        shot = target / 'default.png'

        if not shot.exists ():
            env = private_env ()
            regress.render_once (args, env, folder, shot, target / 'default.txt', [])

        return shot

    def job (case):
        folder, name, kind, value = case
        target = args.out / folder.name
        safe = ''.join (c if c.isalnum () else '_' for c in name)[:40] + '_' + hashlib.sha1 (name.encode ()).hexdigest ()[:8]
        env = private_env ()
        launched = target / f'{safe}.launch.png'
        live = target / f'{safe}.live.png'
        control = target / f'{safe}.control'
        control.write_text (f'property={name}={value}\n')

        regress.render_once (args, env, folder, launched, target / f'{safe}.launch.txt', [f'{name}={value}'])

        live_env = dict (env, LWE_HOTSWAP_AT_FRAME = str (args.change_frame))
        if args.force:
            live_env['LWE_FORCE_LIVE_PROPERTIES'] = '1'

        regress.render_once (
            args, live_env, folder, live, target / f'{safe}.live.txt', [], ['--control-file', str (control)]
        )

        log = (target / f'{safe}.live.txt').read_text (errors = 'replace') if (target / f'{safe}.live.txt').exists () else ''
        result = {'id': folder.name, 'property': name, 'type': kind, 'value': value,
                  'mode': 'live' if 'applied property' in log else ('reload' if 'Hotswapping' in log else 'none')}

        if not launched.exists () or not live.exists ():
            result['status'] = 'render failed'
            return result

        default = base (folder)
        result['effect'] = round (regress.changed_fraction (default, launched, args.threshold), 3) if default.exists () else None
        result['difference'] = round (regress.changed_fraction (launched, live, args.threshold), 3)
        result['status'] = 'match' if result['difference'] <= args.tolerance else 'DIFFERENT'

        if result['effect'] is not None and result['effect'] <= args.tolerance and result['status'] == 'match':
            result['status'] = 'no visible effect'

        return result

    # the default first, the other jobs compare against it
    with concurrent.futures.ThreadPoolExecutor (args.jobs) as pool:
        list (pool.map (base, sorted ({case[0] for case in todo})))

    results = []

    with concurrent.futures.ThreadPoolExecutor (args.jobs) as pool:
        for done, result in enumerate (pool.map (job, todo), 1):
            results.append (result)
            print (f'[{done}/{len (todo)}] {result["id"]} {result["property"]} ({result["type"]}={result["value"]}) '
                   f'{result["mode"]} {result["status"]} effect={result.get ("effect")} diff={result.get ("difference")}',
                   flush = True)

    bus.terminate ()
    stop_watchdog.set ()
    (args.out / 'results.json').write_text (json.dumps (results, ensure_ascii = False, indent = 1))
    different = [r for r in results if r['status'] == 'DIFFERENT']
    print (f'\n{len (results)} properties, {len (different)} different:')

    for r in different:
        print (f'  {r["id"]} {r["property"]} ({r["type"]}) effect={r.get ("effect")} diff={r["difference"]} mode={r["mode"]}')


if __name__ == '__main__':
    main ()
