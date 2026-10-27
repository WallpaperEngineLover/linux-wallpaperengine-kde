#!/usr/bin/env python3
"""mpris_flood.py <count> [bytes]: emits <count> MPRIS PropertiesChanged signals with a title of <bytes> characters on
the session bus of DBUS_SESSION_BUS_ADDRESS, what a player does at every track change, only much faster"""
import sys

import dbus


def main():
    count = int(sys.argv[1])
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 2048
    bus = dbus.SessionBus(private=True)
    changed = dbus.Dictionary({
        "PlaybackStatus": "Playing",
        "Metadata": dbus.Dictionary({"xesam:title": "x" * size, "xesam:artist": dbus.Array(["flood"], signature="s")},
                                    signature="sv"),
    }, signature="sv")
    for _ in range(count):
        message = dbus.lowlevel.SignalMessage("/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
                                              "PropertiesChanged")
        message.append("org.mpris.MediaPlayer2.Player", changed, dbus.Array([], signature="s"), signature="sa{sv}as")
        bus.send_message(message)
    bus.flush()


if __name__ == "__main__":
    main()
