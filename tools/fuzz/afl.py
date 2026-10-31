#!/usr/bin/env python3
"""AFL++ next to the libFuzzer targets: the same parser sources and harnesses (tools/fuzz/*.cpp) built into build-afl/
with clang's trace-pc-guard coverage, ASan and UBSan, linked with AFL++'s afl-compiler-rt (the image's AFL++ has no LLVM
pass, and its GCC plugin was built for another GCC than the installed one). It starts from the corpus libFuzzer built
(tools/runtime_analysis.py fuzz seeds).

    tools/fuzz/afl.py build [-j N]
    tools/fuzz/afl.py run <texture|package|mdl|sog|json> [-t seconds]

AFL++ insists on ASan aborting, and the host's core_pattern pipes to systemd-coredump, which runs even with RLIMIT_CORE 0,
so a crash would land in the user's crash reporter. The driver makes the process non dumpable (PR_SET_DUMPABLE 0), the
kernel then starts no core handler. The targets only parse files, nothing renders.
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import runtime_analysis as ra

ROOT = ra.ROOT
OUT = ROOT / 'build-afl'
RUNTIME = Path('/usr/lib64/afl/afl-compiler-rt-64.o')
SANITIZE = ['-fsanitize=address,undefined', '-fsanitize-coverage=trace-pc-guard']


def build(jobs):
    objects = OUT / 'obj'
    objects.mkdir(parents=True, exist_ok=True)
    flags = ra.fuzz_compile_flags()
    sources = sorted(path for pattern in ra.FUZZ_SOURCES for path in (ROOT / 'src' / 'WallpaperEngine').glob(pattern))
    common = [flag for flag in ra.FUZZ_FLAGS if flag != '-fno-sanitize=vptr'] + ['-fno-sanitize=vptr']

    def compile_one(source):
        target = objects / (str(source.relative_to(ROOT)).replace('/', '_') + '.o')
        if target.exists() and target.stat().st_mtime > source.stat().st_mtime:
            return target
        subprocess.run(['clang++', *common, *SANITIZE, '-fPIC', *flags, '-c', str(source), '-o', str(target)], check=True)
        return target

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        objs = list(pool.map(compile_one, sources))

    globals_file = OUT / 'globals.cpp'
    globals_file.write_text('float g_Time;\nfloat g_TimeLast;\n')
    driver = OUT / 'afl_driver.o'
    subprocess.run(['clang++', *common, '-c', str(HERE / 'afl_driver.cpp'), '-o', str(driver)], check=True)

    for name in ra.FUZZ_TARGETS:
        subprocess.run(['clang++', *common, *SANITIZE, *flags, str(HERE / f'{name}.cpp'), str(globals_file), str(driver),
                        str(RUNTIME), *map(str, objs), '-o', str(OUT / name), '-Wl,--unresolved-symbols=ignore-all',
                        *ra.FUZZ_LIBS], check=True)
        print(f'built {OUT / name}')


def run(target, seconds):
    corpus = ra.FUZZ_DIR / 'corpus' / target
    findings = OUT / 'findings' / target
    findings.parent.mkdir(parents=True, exist_ok=True)
    env = {**os.environ,
           'ASAN_OPTIONS': 'abort_on_error=1:symbolize=0:detect_leaks=0:'
                           'allocator_may_return_null=1:max_allocation_size_mb=2048',
           'UBSAN_OPTIONS': f'print_stacktrace=0:suppressions={ra.SUPP / "ubsan.supp"}',
           'AFL_IGNORE_PROBLEMS': '1', 'AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES': '1',
           'AFL_SKIP_CPUFREQ': '1', 'AFL_NO_AFFINITY': '1', 'AFL_NO_UI': '1'}
    subprocess.run(['sh', '-c', 'ulimit -c 0; exec "$@"', 'sh', 'afl-fuzz', '-i', str(corpus), '-o', str(findings),
                    '-V', str(seconds), '-m', 'none', '-t', '20000', '--', str(OUT / target)], env=env, check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build')
    b.add_argument('-j', '--jobs', type=int, default=6)
    r = sub.add_parser('run')
    r.add_argument('target', choices=ra.FUZZ_TARGETS)
    r.add_argument('-t', '--seconds', type=int, default=600)
    options = parser.parse_args()

    if options.command == 'build':
        build(options.jobs)
    else:
        run(options.target, options.seconds)


if __name__ == '__main__':
    main()
