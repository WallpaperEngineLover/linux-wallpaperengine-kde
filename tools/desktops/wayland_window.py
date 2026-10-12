#!/usr/bin/env python3
"""GTK 4 Wayland window (normal/maximized/fullscreen) until killed, for labs without Xwayland."""
import os
import signal
import sys

os.environ["GDK_BACKEND"] = "wayland"

import gi  # noqa: E402

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402

mode = sys.argv[1] if len(sys.argv) > 1 else "fullscreen"


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title=f"lwe-{mode}-probe")
    window.set_default_size(640, 480)
    if mode == "maximized":
        window.maximize()
    elif mode == "fullscreen":
        window.fullscreen()
    window.present()


app = Gtk.Application(application_id="org.lwe.Probe")
app.connect("activate", activate)
try:
    from gi.repository import GLibUnix  # noqa: E402

    GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, app.quit)
except (ImportError, AttributeError):
    # older PyGObject (Ubuntu 24.04) has only the GLib one
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, app.quit)
app.run([])
