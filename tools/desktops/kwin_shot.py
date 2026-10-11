#!/usr/bin/env python3
"""Screenshot of a KWin session over org.kde.KWin.ScreenShot2 (kwin started with KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1)."""
import os
import sys
import threading

import dbus
from PIL import Image

bus = dbus.SessionBus()
iface = dbus.Interface(bus.get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2"), "org.kde.KWin.ScreenShot2")
read_end, write_end = os.pipe()
chunks = []


def drain():
    while True:
        block = os.read(read_end, 1 << 20)
        if not block:
            return
        chunks.append(block)


reader = threading.Thread(target=drain)
reader.start()
meta = iface.CaptureWorkspace({"native-resolution": True, "include-cursor": False}, dbus.types.UnixFd(write_end))
os.close(write_end)
reader.join()
data = b"".join(chunks)
width, height, stride = int(meta["width"]), int(meta["height"]), int(meta["stride"])
image = Image.frombuffer("RGBA", (width, height), data, "raw", "BGRA", stride, 1)
image.convert("RGB").save(sys.argv[1])
print(width, height, len(data), dict((str(k), str(v)) for k, v in meta.items() if k not in ("width", "height", "stride")))
