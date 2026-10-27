#!/usr/bin/env python3
"""screen_saver_owner.py <state file>: owns org.freedesktop.ScreenSaver on the session bus of DBUS_SESSION_BUS_ADDRESS,
the way a locker does, but has no GetActive (calls get an error back). ActiveChanged(true) is sent when the state file
appears, ActiveChanged(false) when it is gone again"""
import sys
from pathlib import Path

import dbus
import dbus.lowlevel
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib


def main():
    state = Path(sys.argv[1])
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus(private=True)
    name = dbus.service.BusName("org.freedesktop.ScreenSaver", bus)
    active = [False]

    def poll():
        if state.exists() != active[0]:
            active[0] = not active[0]
            message = dbus.lowlevel.SignalMessage("/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver",
                                                  "ActiveChanged")
            message.append(dbus.Boolean(active[0]), signature="b")
            bus.send_message(message)

        return True

    GLib.timeout_add(100, poll)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
