#!/usr/bin/env python3
"""Prints the busiest zones of a Tracy capture (tracy-csvexport), and the GPU time of the zones that have it.

    tools/tracy_summary.py <capture.tracy> [rows]
"""
import csv
import io
import statistics
import subprocess
import sys


def export(args):
    result = subprocess.run(["tracy-csvexport", *args], capture_output=True, text=True, check=True)
    return list(csv.DictReader(io.StringIO(result.stdout)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    capture = sys.argv[1]
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 25

    # the aggregated export groups by source location, so zones named at run time all end up as one; each event
    # carries its own name
    times = {}

    for event in export(["-u", capture]):
        times.setdefault(event["name"], []).append(float(event["exec_time_ns"]) / 1e6)

    print(f"{'zone':44} {'count':>8} {'mean ms':>9} {'max ms':>9} {'total ms':>10}")

    for name, values in sorted(times.items(), key=lambda item: -sum(item[1]))[:rows]:
        print(f"{name[:44]:44} {len(values):>8} {statistics.fmean(values):>9.3f} {max(values):>9.3f} {sum(values):>10.1f}")

    gpu = {}

    for zone in export(["-g", capture]):
        gpu.setdefault(zone["name"], []).append(float(zone["GPU execution time"]) / 1e6)

    if gpu:
        print(f"\n{'gpu zone':44} {'count':>8} {'mean ms':>9} {'p99 ms':>9} {'max ms':>9}")

        for name, times in gpu.items():
            times.sort()
            print(
                f"{name[:44]:44} {len(times):>8} {statistics.fmean(times):>9.3f} "
                f"{times[int(len(times) * 0.99)]:>9.3f} {times[-1]:>9.3f}"
            )


main()
