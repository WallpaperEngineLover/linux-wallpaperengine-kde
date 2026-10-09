#!/usr/bin/env python3
"""wavstat.py <file.wav> [window seconds] - per channel peak/RMS and, per window, RMS of each channel.
Reads the float32 or int16 WAVs parec writes (audio_sink.sh record)."""
import struct, sys
import numpy as np


def read_wav(path):
    d = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        tag, size = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + size] if size != 0xFFFFFFFF else d[pos + 8:]
        if tag == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif tag == b'data':
            data = body
            break
        pos += 8 + size + (size & 1)
    code, channels, rate, _, _, bits = fmt
    dtype = '<f4' if code == 3 or (code == 0xFFFE and bits == 32) else '<i2'
    a = np.frombuffer(data[:len(data) // (channels * bits // 8) * channels * bits // 8], dtype=dtype)
    a = a.astype(np.float64) / (32768.0 if dtype == '<i2' else 1.0)
    return a.reshape(-1, channels), rate


if __name__ == '__main__':
    a, rate = read_wav(sys.argv[1])
    print('%s: %.2f s, %d Hz, %d ch' % (sys.argv[1], len(a) / rate, rate, a.shape[1]))
    print('peak', np.abs(a).max(0).round(4), 'rms', np.sqrt((a ** 2).mean(0)).round(4))
    if len(sys.argv) > 2:
        n = int(float(sys.argv[2]) * rate)
        for i in range(0, len(a), n):
            w = a[i:i + n]
            print('%7.2f s  ' % (i / rate) + '  '.join('%.4f' % v for v in np.sqrt((w ** 2).mean(0))))
