#!/usr/bin/env python3
"""An unmanaged X11 window at x y width height that claims to be maximized (WM_STATE and _NET_WM_STATE set by hand, so a
window manager cannot move it) and stays until killed (tests/desktops: a window on another output than the engine's)."""
import signal
import sys
import time

from Xlib import X, Xatom, display

x, y, width, height = (int(value) for value in sys.argv[1:5])
d = display.Display()
screen = d.screen()
window = screen.root.create_window(x, y, width, height, 0, screen.root_depth, X.InputOutput, X.CopyFromParent,
                                   background_pixel=screen.black_pixel, override_redirect=True)
wm_state = d.intern_atom("WM_STATE")
window.change_property(wm_state, wm_state, 32, [1, 0])
window.change_property(d.intern_atom("_NET_WM_STATE"), Xatom.ATOM, 32,
                       [d.intern_atom("_NET_WM_STATE_MAXIMIZED_VERT"), d.intern_atom("_NET_WM_STATE_MAXIMIZED_HORZ")])
window.map()
d.flush()
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
while True:
    time.sleep(1)
