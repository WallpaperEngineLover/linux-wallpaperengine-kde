"""fullscreen_window.py - a plain fullscreen window for the pause-on-fullscreen check (GTK 3, else GTK 4).
Runs until it is killed. Exit code 2 when neither GTK is importable."""
import sys

import gi

try:
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk

    window = Gtk.Window(title="lwe-fullscreen-test")
    window.fullscreen()
    window.connect("destroy", Gtk.main_quit)
    window.show_all()
    Gtk.main()
except (ValueError, ImportError):
    try:
        gi.require_version("Gtk", "4.0")
        from gi.repository import GLib, Gtk
    except (ValueError, ImportError):
        sys.exit(2)

    def activate(app):
        window = Gtk.ApplicationWindow(application=app, title="lwe-fullscreen-test")
        window.fullscreen()
        window.present()

    app = Gtk.Application(application_id="test.lwe.Fullscreen")
    app.connect("activate", activate)
    app.run([])
