#!/usr/bin/env python3
"""Runtime analysis of the engine on headless renders: sanitizer builds, heaptrack, Valgrind.

Usage:
    runtime_analysis.py build asan|tsan [-j jobs]
    runtime_analysis.py run plain|asan|tsan|heaptrack|memcheck|massif [-s scenario]... [-t seconds] [-o out dir]
    runtime_analysis.py swap plain|asan|tsan|heaptrack [-r rounds] [-o out dir]
    runtime_analysis.py fuzz build|seeds|run [target] [-t seconds]
    runtime_analysis.py list

build makes build-asan (AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer) or build-tsan
(ThreadSanitizer) next to build/ through the LWE_SANITIZE CMake option. The sanitizer runtimes are looked up in
LWE_SANITIZER_LIBS (a lib64 folder) when the compiler's own package does not ship them.

run renders the scenarios one after the other (never in parallel, a sanitizer build of a scene with V8 and CEF is
heavy) with the same isolation as headless_render.sh: private XDG_RUNTIME_DIR and session bus without service
activation, dead audio and system bus, no desktop variables, no core dumps, headless EGL driver. A watchdog kills the
run when its process group passes --rss-limit or the machine runs below --min-available. Each scenario runs for
--seconds, then gets SIGTERM so the leak checks and heaptrack's final write happen.

swap loads a wallpaper and hotswaps through a list of others through the control file, --rounds times, to look at
what the texture cache, CPass rebuilds and script engines leave behind.

fuzz builds the libFuzzer harnesses of tools/fuzz/ (clang, AddressSanitizer + UndefinedBehaviorSanitizer) in
build-fuzz/: the parser sources are compiled with coverage instrumentation next to each harness, the rest of the
engine is left out (symbols the parsers never reach stay unresolved). seeds fills build-fuzz/corpus/<target> from the
installed workshop items and run starts one target (-t seconds, -j workers) with crashes written to
build-fuzz/crashes/<target>.

Results go to <out dir> (default build/runtime-analysis/<tool>), one folder per scenario with the engine log, the
sanitizer reports (<tool>.<pid>) or heaptrack's summary, and summary.txt with one line per scenario.
Suppressions are in tools/runtime_analysis/.
"""
import argparse
import os
import re
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUPP = ROOT / 'tools' / 'runtime_analysis'
SHIM_SRC = ROOT / 'tools' / 'dbus_noop_shim.c'
NOCORE_SRC = SUPP / 'nocore.c'
WORKSHOP = Path(os.environ.get('LWE_WORKSHOP_DIR', Path.home() / '.local/share/Steam/steamapps/workshop/content/431960'))
ASSETS = Path(os.environ.get('LWE_ASSETS_DIR', Path.home() / '.local/share/Steam/steamapps/common/wallpaper_engine/assets'))
SANITIZER_LIBS = Path(os.environ.get('LWE_SANITIZER_LIBS', Path.home() / '.local/share/sanitizers/root/usr/lib64'))

SCENARIOS = {
    'scene-audio': ('3299228616', []),
    'scene-collage': ('2250930375', []),
    'scene-tiny': ('2828779339', []),
    'puppet': ('3509578940', []),
    'particles': ('1995089753', []),
    'text': ('3577513994', []),
    'model-3d': ('3777414605', []),
    'scripts': ('3737268876', []),
    'video-texture': ('3410266156', []),
    'video': ('1367456719', []),
    'splat': ('3752541815', []),
    'web': ('3496522892', []),
}
SWAP_ITEMS = ['3299228616', '3509578940', '1995089753', '3577513994', '3777414605', '2250930375']

BUILDS = {
    'asan': ('build-asan', 'address;undefined'),
    'tsan': ('build-tsan', 'thread'),
}
SANITIZER_TOOLS = ('asan', 'tsan')


def build_dir(tool):
    return ROOT / ('build-tsan' if tool == 'tsan' else 'build-asan' if tool == 'asan' else 'build') / 'output'


def build(tool, jobs):
    folder, sanitize = BUILDS[tool]
    target = ROOT / folder
    link = f'-L{SANITIZER_LIBS} -Wl,-rpath,{SANITIZER_LIBS}' if SANITIZER_LIBS.is_dir() else ''
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(target), '-DCMAKE_BUILD_TYPE=Release',
                    f'-DLWE_SANITIZE={sanitize}', f'-DCMAKE_EXE_LINKER_FLAGS={link}',
                    f'-DCMAKE_SHARED_LINKER_FLAGS={link}'], check=True)
    subprocess.run(['nice', 'cmake', '--build', str(target), '-j', str(jobs)], check=True)


def available_mb():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) // 1024
    return 1 << 30


def group_rss_mb(pgid):
    total = 0
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / 'stat').read_text()
            if int(stat[stat.rindex(')') + 2:].split()[3]) != pgid:
                continue
            for line in (entry / 'status').read_text().splitlines():
                if line.startswith('VmRSS:'):
                    total += int(line.split()[1])
        except (OSError, ValueError):
            continue
    return total // 1024


def mapping_rss_kb(pgid):
    """resident kB of a process group per kind of mapping: the heap, anonymous memory, each file or device"""
    totals = {}
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / 'stat').read_text()
            if int(stat[stat.rindex(')') + 2:].split()[3]) != pgid:
                continue
            lines = (entry / 'smaps').read_text().splitlines()
        except (OSError, ValueError):
            continue
        name = '[anon]'
        for line in lines:
            words = line.split(None, 5)
            if len(words) >= 2 and '-' in words[0] and not words[0].endswith(':'):
                name = words[5] if len(words) == 6 else '[anon]'
                if name.startswith('/memfd:') or name.startswith('/dev/shm'):
                    name = name.split(' (')[0]
            elif line.startswith('Rss:'):
                totals[name] = totals.get(name, 0) + int(line.split()[1])
    return totals


def print_mapping_growth(rounds, rows=8):
    """which mappings grew between the first and the last round of a swap run"""
    # a round after the engine died has no mappings to compare
    seen = [(number, snapshot) for number, snapshot in enumerate(rounds, 1) if snapshot]
    if len(seen) < 2:
        return
    (first_round, first), (last_round, last) = seen[1] if len(seen) > 2 else seen[0], seen[-1]
    growth = sorted(((last.get(name, 0) - first.get(name, 0), name) for name in set(first) | set(last)), reverse=True)
    print(f'mapping growth, round {first_round} -> {last_round} (MB):')
    for delta, name in growth[:rows]:
        if delta > 1024:
            print(f'  {delta / 1024:+8.1f}  {name}')


class Watchdog(threading.Thread):
    def __init__(self, pgid, rss_limit, min_available, log):
        super().__init__(daemon=True)
        self.pgid, self.rss_limit, self.min_available, self.log = pgid, rss_limit, min_available, log
        self.stop_flag = threading.Event()
        self.peak = 0
        self.killed = None
        self.samples = []

    def run(self):
        while not self.stop_flag.wait(0.5):
            rss = group_rss_mb(self.pgid)
            self.peak = max(self.peak, rss)
            self.samples.append((time.time(), rss))
            free = available_mb()
            if rss > self.rss_limit or free < self.min_available:
                self.killed = f'rss {rss} MB, {free} MB available'
                with open(self.log, 'a') as f:
                    f.write(f'\nWATCHDOG: killed at {self.killed}\n')
                try:
                    os.killpg(self.pgid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                return


BUS_CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=%s</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


def start_bus(runtime):
    # no <servicedir>: nothing gets activated, so nothing is left behind to outlive the bus
    config = runtime / 'bus.conf'
    config.write_text(BUS_CONFIG % (runtime / 'bus'))
    proc = subprocess.Popen(['dbus-daemon', '--config-file', str(config), '--nofork'], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if (runtime / 'bus').exists():
            break
        time.sleep(0.05)
    return proc


def free_display():
    for number in range(90, 200):
        if not Path(f'/tmp/.X11-unix/X{number}').exists():
            return number
    raise RuntimeError('no free X display')


def sanitizer_env(tool, out):
    reports = out / tool
    if tool == 'asan':
        return {
            'ASAN_OPTIONS': f'log_path={reports}:handle_abort=1:verify_asan_link_order=0:detect_leaks=1:detect_stack_use_after_return=1:'
                            'allocator_may_return_null=1:strict_string_checks=1:check_initialization_order=1',
            'LSAN_OPTIONS': f'suppressions={SUPP / "lsan.supp"}:print_suppressions=0',
            'UBSAN_OPTIONS': f'log_path={reports}:print_stacktrace=1:suppressions={SUPP / "ubsan.supp"}',
        }
    if tool == 'tsan':
        return {'TSAN_OPTIONS': f'log_path={reports}:handle_abort=1:suppressions={SUPP / "tsan.supp"}:second_deadlock_stack=1:'
                                'history_size=4'}
    return {}


def wrapper(tool, out):
    if tool == 'heaptrack':
        return ['heaptrack', '--record-only', '-o', str(out / 'heaptrack')]
    if tool == 'memcheck':
        return ['valgrind', '--tool=memcheck', '--leak-check=full', '--num-callers=30', '--error-limit=no',
                f'--log-file={out / "memcheck.%p"}', f'--suppressions={SUPP / "valgrind.supp"}']
    if tool == 'massif':
        return ['valgrind', '--tool=massif', f'--massif-out-file={out / "massif.%p"}', '--time-unit=ms']
    return []


class Run:
    def __init__(self, tool, binary_dir, out, args, web=False, rss_limit=8192, min_available=6144):
        self.tool, self.out = tool, out
        out.mkdir(parents=True, exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix='lwe-rt-'))
        self.scratch = scratch
        self.runtime = scratch / 'run'
        self.runtime.mkdir(mode=0o700)
        shim = scratch / 'dbus_noop_shim.so'
        subprocess.run(['gcc', '-shared', '-fPIC', '-o', str(shim), str(SHIM_SRC), '-ldl'], check=True)
        nocore = scratch / 'nocore.so'
        subprocess.run(['gcc', '-shared', '-fPIC', '-o', str(nocore), str(NOCORE_SRC)], check=True)
        # test processes must not carry the engine's name, we_manager's shutdown pkill -f matches it
        link = scratch / 'lwk'
        link.symlink_to((binary_dir / 'linux-wallpaperengine').resolve())
        self.bus = start_bus(self.runtime)
        self.xvfb = None
        env = {k: v for k, v in os.environ.items() if k not in (
            'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'KDE_FULL_SESSION', 'KDE_SESSION_VERSION', 'DESKTOP_SESSION',
            'XDG_CURRENT_DESKTOP', 'XDG_SESSION_DESKTOP', 'DISPLAY', 'PULSE_SERVER', 'PIPEWIRE_REMOTE')}
        env.update({
            'XDG_RUNTIME_DIR': str(self.runtime),
            'DBUS_SESSION_BUS_ADDRESS': f'unix:path={self.runtime}/bus',
            'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent-system-bus',
            'PULSE_SERVER': 'unix:/nonexistent-pulse',
            'PIPEWIRE_REMOTE': '/nonexistent-pipewire',
            'ALSA_CONFIG_PATH': '/dev/null',
            'SDL_AUDIODRIVER': 'dummy',
            'XDG_SESSION_TYPE': 'headless',
            'XDG_CACHE_HOME': str(scratch / 'cache'),
            'LD_LIBRARY_PATH': str(binary_dir) + (f':{SANITIZER_LIBS}' if SANITIZER_LIBS.is_dir() else ''),
            # the host's core_pattern pipes to a crash handler that runs whatever RLIMIT_CORE says: a crash exits here instead
            'LD_PRELOAD': f'{shim}:{nocore}' if tool not in SANITIZER_TOOLS else str(shim),
            'LWE_FRAME_STATS': '1',
        })
        if web:
            display = free_display()
            self.xvfb = subprocess.Popen(['Xvfb', f':{display}', '-screen', '0', '1920x1080x24', '-nolisten', 'tcp'],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)
            env.update({
                'DISPLAY': f':{display}', 'LWE_CEF_NO_SANDBOX': '1',
                'LWE_WEB_HOST_PRELOAD': str(binary_dir / 'libcef.so'),
                'LWE_CEF_SWITCHES': '--ozone-platform=headless --use-gl=angle --use-angle=gl-egl '
                                    '--ignore-gpu-blocklist --disable-dev-shm-usage',
            })
        env.update(sanitizer_env(tool, out))
        self.log = out / 'engine.log'
        self.control = scratch / 'control'
        command = [*wrapper(tool, out), str(link), '--window', '0x0x1280x720', '--silent', '--assets-dir', str(ASSETS),
                   '--control-file', str(self.control), *args]
        self.logfile = open(self.log, 'wb')
        self.started = time.time()
        self.proc = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL, stdout=self.logfile,
                                     stderr=subprocess.STDOUT, preexec_fn=self.child_setup)
        self.watchdog = Watchdog(self.proc.pid, rss_limit, min_available, self.log)
        self.watchdog.start()

    @staticmethod
    def child_setup():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        os.setsid()

    def alive(self):
        return self.proc.poll() is None

    def engine_pid(self):
        # behind a wrapper script the engine is a child of the process this class started
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                stat = (entry / 'stat').read_text()
                if int(stat[stat.rindex(')') + 2:].split()[3]) != self.proc.pid:
                    continue
                if (entry / 'exe').resolve().name == 'linux-wallpaperengine' \
                        and b'--web-host' not in (entry / 'cmdline').read_bytes():
                    return int(entry.name)
            except (OSError, ValueError):
                continue
        return self.proc.pid

    def hotswap(self, text):
        self.control.write_text(text)
        os.kill(self.engine_pid() if self.tool == 'heaptrack' else self.proc.pid, signal.SIGUSR1)

    def finish(self, grace=120):
        if self.alive():
            # the wrappers (heaptrack, valgrind) have to pass it on, so the whole group gets it
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(grace)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.watchdog.stop_flag.set()
        self.logfile.close()
        self.bus.kill()
        self.bus.wait(5)
        if self.xvfb:
            self.xvfb.kill()
            self.xvfb.wait(5)
        shutil.rmtree(self.scratch, ignore_errors=True)
        return self.proc.returncode


REPORT_MARKERS = {
    'asan': [('AddressSanitizer', 'ERROR: AddressSanitizer'), ('LeakSanitizer', 'ERROR: LeakSanitizer'),
             ('UBSan', 'runtime error:')],
    'tsan': [('races', 'WARNING: ThreadSanitizer: data race'), ('other', 'WARNING: ThreadSanitizer'),
             ('deadlock', 'lock-order-inversion')],
    'memcheck': [('invalid', 'Invalid '), ('uninit', 'uninitialised'), ('leaks', 'definitely lost')],
}


def rss_growth(samples):
    # RSS of the process group over the last 70% of the run (the first 30% is loading), in MB per minute
    if len(samples) < 20:
        return None
    steady = samples[len(samples) * 3 // 10:]
    n = len(steady)
    mean_t = sum(t for t, _ in steady) / n
    mean_r = sum(r for _, r in steady) / n
    spread = sum((t - mean_t) ** 2 for t, _ in steady)
    return 60 * sum((t - mean_t) * (r - mean_r) for t, r in steady) / spread if spread else 0.0


def summarize(tool, out, run, code):
    text = run.log.read_text(errors='replace')
    reports = ''
    for path in sorted(out.glob(f'{tool}*')) + sorted(out.glob('memcheck.*')):
        if path.is_file():
            reports += path.read_text(errors='replace')
    counts = []
    for label, marker in REPORT_MARKERS.get(tool, []):
        counts.append(f'{label}={(reports + text).count(marker)}')
    stats = len(re.findall(r'FRAME-STATS', text))
    growth = rss_growth(run.watchdog.samples)
    parts = [f'exit={code}', f'peak={run.watchdog.peak}MB',
             f'growth={growth:+.1f}MB/min' if growth is not None else 'growth=n/a', f'stats-windows={stats}', *counts]
    if run.watchdog.killed:
        parts.append(f'WATCHDOG({run.watchdog.killed})')
    if tool == 'heaptrack':
        dumps = sorted(out.glob('heaptrack*.zst')) + sorted(out.glob('heaptrack*.gz'))
        if dumps:
            summary = subprocess.run(['heaptrack_print', '-n', '25', str(dumps[-1])], capture_output=True, text=True)
            (out / 'heaptrack_summary.txt').write_text(summary.stdout)
            leaks = subprocess.run(['heaptrack_print', '-l', '-n', '25', '-s', '12', str(dumps[-1])],
                                   capture_output=True, text=True)
            (out / 'heaptrack_leaks.txt').write_text(leaks.stdout[leaks.stdout.find('MEMORY LEAKS'):])
            peak = re.search(r'peak heap memory consumption: (\S+)', summary.stdout)
            leaked = re.search(r'total memory leaked: (\S+)', summary.stdout)
            parts += [f'heap-peak={peak.group(1) if peak else "?"}', f'leaked={leaked.group(1) if leaked else "?"}']
    return ' '.join(parts)


def run_scenario(tool, name, args, seconds, web, out_root, rss_limit, min_available):
    binary_dir = build_dir(tool if tool in SANITIZER_TOOLS else 'plain')
    out = out_root / name
    if out.exists():
        shutil.rmtree(out)
    run = Run(tool, binary_dir, out, args, web, rss_limit, min_available)
    end = time.time() + seconds
    while time.time() < end and run.alive():
        time.sleep(0.5)
    code = run.finish()
    return summarize(tool, out, run, code)


def command_run(options):
    if options.tool in SANITIZER_TOOLS and not (build_dir(options.tool) / 'linux-wallpaperengine').exists():
        sys.exit(f'{build_dir(options.tool).parent} is not built, run: runtime_analysis.py build {options.tool}')
    out_root = Path(options.out) if options.out else ROOT / 'build' / 'runtime-analysis' / options.tool
    out_root.mkdir(parents=True, exist_ok=True)
    names = options.scenario or list(SCENARIOS)
    lines = []
    for name in names:
        item, extra = SCENARIOS[name]
        path = WORKSHOP / item
        if not path.is_dir():
            lines.append(f'{name}: SKIP, {item} not in {WORKSHOP}')
            continue
        web = name == 'web'
        seconds = options.seconds * (3 if options.tool in ('memcheck', 'massif') else 1)
        print(f'== {name} ({item}) {options.tool} {seconds}s', flush=True)
        line = f'{name}: ' + run_scenario(options.tool, name, [*extra, str(path)], seconds, web, out_root,
                                          options.rss_limit, options.min_available)
        print(line, flush=True)
        lines.append(line)
    (out_root / 'summary.txt').write_text('\n'.join(lines) + '\n')


def command_swap(options):
    out_root = Path(options.out) if options.out else ROOT / 'build' / 'runtime-analysis' / options.tool
    out = out_root / 'swap'
    binary_dir = build_dir(options.tool if options.tool in SANITIZER_TOOLS else 'plain')
    items = [WORKSHOP / item for item in SWAP_ITEMS if (WORKSHOP / item).is_dir()]
    if len(items) < 2:
        sys.exit('need at least two of the swap wallpapers installed')
    if out.exists():
        shutil.rmtree(out)
    run = Run(options.tool, binary_dir, out, [str(items[0])], False, options.rss_limit, options.min_available)
    time.sleep(options.wait)
    swaps = 0
    snapshots = []
    for round_number in range(options.rounds):
        for item in items:
            if not run.alive():
                break
            run.hotswap(f'path={item}\n')
            swaps += 1
            time.sleep(options.wait)
        print(f'round {round_number + 1}: rss {group_rss_mb(run.proc.pid)} MB, alive={run.alive()}', flush=True)
        snapshots.append(mapping_rss_kb(run.proc.pid))
    print_mapping_growth(snapshots)
    code = run.finish()
    failed = run.log.read_text(errors='replace').count('Hotswap failed')
    line = f'swap: swaps={swaps} failed-swaps={failed} ' + summarize(options.tool, out, run, code)
    print(line)
    (out_root / 'summary.txt').write_text(line + '\n')


FUZZ_DIR = ROOT / 'build-fuzz'
FUZZ_TARGETS = ['texture', 'package', 'mdl', 'sog', 'json']
FUZZ_SOURCES = ['Data/*.cpp', 'Data/Builders/*.cpp', 'Data/Model/*.cpp', 'Data/Parsers/*.cpp', 'Data/Utils/*.cpp',
                'Assets/*.cpp', 'FileSystem/*.cpp', 'FileSystem/Adapters/*.cpp', 'Logging/*.cpp', 'Maths.cpp',
                'Render/Objects/Puppet*.cpp', 'Render/Objects/CMesh.cpp', 'Splat/SogLoader.cpp']
FUZZ_LIBS = ['-llz4', '-lavcodec', '-lavutil', '-lavformat', '-lswscale', '-lswresample', '-lpng', '-lfreetype',
             '-lharfbuzz', '-lGLEW', '-lGL']
FUZZ_FLAGS = ['-std=gnu++20', '-O1', '-g1', '-fno-omit-frame-pointer', '-fno-sanitize=vptr', '-Wno-narrowing', '-w']


def fuzz_compile_flags():
    import json
    import shlex
    for entry in json.loads((ROOT / 'build' / 'compile_commands.json').read_text()):
        if entry['file'].endswith('Parsers/TextureParser.cpp'):
            words = shlex.split(entry['command'])
            flags, i = [], 1
            while i < len(words):
                if words[i].startswith(('-I', '-D')):
                    flags.append(words[i])
                elif words[i] == '-isystem':
                    flags += ['-isystem', words[i + 1]]
                    i += 1
                i += 1
            return flags
    sys.exit('build/compile_commands.json has no TextureParser.cpp, configure build/ first')


def fuzz_build(jobs):
    import concurrent.futures
    objects = FUZZ_DIR / 'obj'
    objects.mkdir(parents=True, exist_ok=True)
    flags = fuzz_compile_flags()
    sources = sorted(path for pattern in FUZZ_SOURCES for path in (ROOT / 'src' / 'WallpaperEngine').glob(pattern))

    def compile_one(source):
        target = objects / (str(source.relative_to(ROOT)).replace('/', '_') + '.o')
        if target.exists() and target.stat().st_mtime > source.stat().st_mtime:
            return target
        subprocess.run(['clang++', *FUZZ_FLAGS, '-fsanitize=fuzzer-no-link,address,undefined', '-fPIC', *flags,
                        '-c', str(source), '-o', str(target)], check=True)
        return target

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        objs = list(pool.map(compile_one, sources))
    globals_file = FUZZ_DIR / 'globals.cpp'
    globals_file.write_text('float g_Time;\nfloat g_TimeLast;\n')
    for name in FUZZ_TARGETS:
        subprocess.run(['clang++', *FUZZ_FLAGS, '-fsanitize=fuzzer,address,undefined', *flags,
                        str(ROOT / 'tools' / 'fuzz' / f'{name}.cpp'), str(globals_file), *map(str, objs),
                        '-o', str(FUZZ_DIR / name), '-Wl,--unresolved-symbols=ignore-all', *FUZZ_LIBS], check=True)
        print(f'built {FUZZ_DIR / name}')


def fuzz_seeds():
    sys.path.insert(0, str(ROOT / 'tools'))
    import struct

    def sized(f):
        return f.read(struct.unpack('<I', f.read(4))[0])

    corpus = FUZZ_DIR / 'corpus'
    for name in FUZZ_TARGETS:
        (corpus / name).mkdir(parents=True, exist_ok=True)
    counts = dict.fromkeys(FUZZ_TARGETS, 0)

    def add(name, data, label):
        if 0 < len(data) <= 1_000_000:
            (corpus / name / label).write_bytes(data)
            counts[name] += 1

    for pkg in sorted(WORKSHOP.glob('*/*.pkg')):
        item = pkg.parent.name
        with open(pkg, 'rb') as f:
            try:
                sized(f)
                entries = [(sized(f).decode('utf-8', 'replace'), *struct.unpack('<II', f.read(8)))
                           for _ in range(struct.unpack('<I', f.read(4))[0])]
            except (struct.error, ValueError):
                continue
            base = f.tell()
            add('package', pkg.read_bytes()[:base + 64], f'{item}_{pkg.stem}_head')
            for entry, offset, length in entries:
                kind = {'.tex': 'texture', '.mdl': 'mdl', '.json': 'json'}.get(Path(entry).suffix)
                if kind and counts[kind] < (1500 if kind == 'json' else 400) and (kind != 'json' or length < 60_000):
                    f.seek(base + offset)
                    add(kind, f.read(length), f'{item}_{entry.replace("/", "_")}')
    for project in sorted(WORKSHOP.glob('*/project.json')):
        add('json', project.read_bytes(), f'{project.parent.name}_project.json')
    for meta in sorted(WORKSHOP.glob('*/imgs/*/meta.json'))[:3]:
        image = min(meta.parent.glob('*.webp'), key=lambda path: path.stat().st_size, default=None)
        if image is not None:
            text = meta.read_bytes()
            add('sog', struct.pack('<H', len(text)) + text + image.read_bytes(), f'{meta.parent.parent.parent.name}_{meta.parent.name}')
    print(', '.join(f'{name}: {count}' for name, count in counts.items()))


def fuzz_run(options):
    binary = FUZZ_DIR / options.target
    crashes = FUZZ_DIR / 'crashes' / options.target
    crashes.mkdir(parents=True, exist_ok=True)
    corpus = FUZZ_DIR / 'corpus' / options.target
    work = FUZZ_DIR / 'work' / options.target
    work.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, 'ASAN_OPTIONS': 'allocator_may_return_null=1:detect_leaks=1:max_allocation_size_mb=2048',
           'UBSAN_OPTIONS': f'print_stacktrace=1:suppressions={SUPP / "ubsan.supp"}'}
    subprocess.run([str(binary), str(work), str(corpus), f'-max_total_time={options.seconds}', '-max_len=1000000',
                    '-rss_limit_mb=3000', '-timeout=20', f'-artifact_prefix={crashes}/', f'-jobs={options.jobs}',
                    f'-workers={options.jobs}', '-print_final_stats=1'], env=env, cwd=work)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build')
    b.add_argument('tool', choices=BUILDS)
    b.add_argument('-j', '--jobs', type=int, default=8)
    for name in ('run', 'swap'):
        r = sub.add_parser(name)
        r.add_argument('tool', choices=['plain', 'asan', 'tsan', 'heaptrack', 'memcheck', 'massif'])
        r.add_argument('-o', '--out')
        r.add_argument('--rss-limit', type=int, default=8192, help='MB for the whole process group')
        r.add_argument('--min-available', type=int, default=6144, help='MB of MemAvailable below which the run is killed')
        if name == 'run':
            r.add_argument('-s', '--scenario', action='append', choices=list(SCENARIOS))
            r.add_argument('-t', '--seconds', type=int, default=40)
        else:
            r.add_argument('-r', '--rounds', type=int, default=3)
            r.add_argument('-w', '--wait', type=float, default=8, help='seconds to let each wallpaper run')
    f = sub.add_parser('fuzz')
    f.add_argument('action', choices=['build', 'seeds', 'run'])
    f.add_argument('target', nargs='?', choices=FUZZ_TARGETS)
    f.add_argument('-t', '--seconds', type=int, default=300)
    f.add_argument('-j', '--jobs', type=int, default=4)
    sub.add_parser('list')
    options = parser.parse_args()
    if options.command == 'build':
        build(options.tool, options.jobs)
    elif options.command == 'run':
        command_run(options)
    elif options.command == 'swap':
        command_swap(options)
    elif options.command == 'fuzz':
        if options.action == 'build':
            fuzz_build(options.jobs)
        elif options.action == 'seeds':
            fuzz_seeds()
        elif options.target is None:
            sys.exit('fuzz run needs a target')
        else:
            fuzz_run(options)
    else:
        for name, (item, extra) in SCENARIOS.items():
            print(f'{name:14s} {item} {" ".join(extra)}')


if __name__ == '__main__':
    main()
