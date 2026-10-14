#!/usr/bin/env python3
# videoseam.py <frames.txt>... - loop seam statistics of grabbed videotex frame numbers ("<seconds> <frame>" lines from
# grab_frames.py / grab_ours.py, the kit's test video counts 0..N-1 at 30 fps). Frame numbers are unwrapped at every
# loop, fitted against time, and each seam is compared with the rest: the frames stepped across it, how long the
# frame before it stayed up, and how far the video fell behind (residual after the seam minus before, in frames).
import statistics, sys

def runs(path):
    r = [(float(t), int(f)) for t, f in (l.split() for l in open(path))]
    out = []
    for t, f in r:
        if f < 0:
            continue
        if not out or out[-1][1] != f:
            out.append([t, f, t])
        else:
            out[-1][2] = t
    return out

for path in sys.argv[1:]:
    seg = runs(path)
    period = max(f for _, f, _ in seg) + 1
    unwrapped, base, seams = [], 0, []
    for i, (t, f, _) in enumerate(seg):
        if i and f < seg[i - 1][1] - period // 2:
            base += period
            seams.append(i)
        unwrapped.append((t, f + base))
    ts = [t for t, _ in unwrapped]
    fs = [f for _, f in unwrapped]
    mt, mf = statistics.mean(ts), statistics.mean(fs)
    slope = sum((t - mt) * (f - mf) for t, f in unwrapped) / sum((t - mt) ** 2 for t in ts)
    res = [f - (mf + slope * (t - mt)) for t, f in unwrapped]
    steps = [fs[i] - fs[i - 1] for i in range(1, len(fs))]
    holds = [seg[i][0] - seg[i - 1][0] for i in range(1, len(seg))]
    seam_set = set(seams)
    normal_steps = [s for i, s in enumerate(steps, 1) if i not in seam_set]
    normal_holds = [h for i, h in enumerate(holds, 1) if i not in seam_set]
    print('%s: %d updates, %.3f video frames/s, %d seams, loop every %.3f s' % (
        path.split('/')[-1], len(seg), slope, len(seams), period / slope))
    print('  away from seams: step %s, hold median %.0f ms max %.0f ms' % (
        sorted(set(normal_steps)), statistics.median(normal_holds) * 1000, max(normal_holds) * 1000))
    for i in seams:
        before = statistics.mean(res[max(0, i - 6):i])
        after = statistics.mean(res[i:i + 6])
        print('  seam at %7.3f s: %3d -> %-3d step %d, held %3.0f ms, lost %+.2f frames' % (
            seg[i][0], seg[i - 1][1], seg[i][1], steps[i - 1], holds[i - 1] * 1000, before - after))
