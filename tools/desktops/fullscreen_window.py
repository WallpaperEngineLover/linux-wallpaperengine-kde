#!/usr/bin/env python3
"""A plain X11 window that asks the window manager for fullscreen and stays until killed (tests/desktops)."""
import signal
import sys

from Xlib import X, display

d = display.Display()
screen = d.screen()
window = screen.root.create_window(0, 0, 640, 480, 0, screen.root_depth, X.InputOutput, X.CopyFromParent,
                                   background_pixel=screen.black_pixel)
window.set_wm_name("lwe-fullscreen-probe")
window.change_property(d.intern_atom("_NET_WM_STATE"), d.intern_atom("ATOM"), 32,
                       [d.intern_atom("_NET_WM_STATE_FULLSCREEN")])
window.map()
d.flush()
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
while True:
    d.next_event()
