#!/bin/bash
# compare.sh [work dir]: renders every scenario with OpenAL Soft 1.21.1 and with the engine's port and checks that
# the outputs are bit-identical. OpenAL runs with an empty HOME/XDG dirs: it would otherwise pick up the system's
# alsoft.conf or /usr/share/openal/hrtf (a newer OpenAL's data set) instead of the built-in one WE uses
set -uo pipefail
W=${1:-$HOME/.cache/lwe-openal-1.21.1}
cd "$W" || exit 1
mkdir -p empty out
status=0
for s in moving center impulse stereo; do
    env -i HOME=$W/empty XDG_DATA_DIRS=$W/empty XDG_CONFIG_DIRS=$W/empty ./ref $s out/ref_$s.f32 2> out/ref_$s.log
    ./ours $s out/ours_$s.f32 2> out/ours_$s.log
    python3 - "$s" <<'PY' || status=1
import sys, numpy as np
s = sys.argv[1]
a = np.fromfile(f'out/ref_{s}.f32', np.float32).reshape(-1, 2)
b = np.fromfile(f'out/ours_{s}.f32', np.float32).reshape(-1, 2)
# OpenAL stops a voice whose data ran out, ours keeps mixing the zeros after it, compare up to there
n = 60 * 1024 if s == 'stereo' else len(a)
d = np.abs(a[:n] - b[:n]).max()
print(f'{s:8} peak {np.abs(a).max():.5f}  max diff {d:.3g}  {"identical" if d == 0 else "DIFFERENT"}')
sys.exit(0 if d == 0 else 1)
PY
done
exit $status
