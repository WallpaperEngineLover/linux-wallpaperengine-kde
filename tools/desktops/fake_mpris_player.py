#!/usr/bin/env python3
"""fake_mpris_player.py: an MPRIS media player on the session bus of DBUS_SESSION_BUS_ADDRESS that changes tracks,
cover art (a local file, an unreachable http address, none), play state and position every second or so, for the media
code of the engine (DBusMediaSource, the cover downloader, the media scripts of scenes). Runs until it is killed."""
import os
import tempfile

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib
from PIL import Image

PLAYER = "org.mpris.MediaPlayer2.Player"
ROOT = "org.mpris.MediaPlayer2"


def cover():
    path = os.path.join(tempfile.mkdtemp(prefix="lwe-fake-mpris-"), "cover.png")
    Image.linear_gradient("L").resize((64, 64)).convert("RGB").save(path)
    return "file://" + path


def metadata(title, artist, album, art, length):
    values = {
        "mpris:trackid": dbus.ObjectPath("/org/lwe/track/" + title.replace(" ", "")),
        "mpris:length": dbus.Int64(length * 1000000),
        "xesam:title": title,
        "xesam:artist": dbus.Array([artist, "Second Artist"], signature="s"),
        "xesam:album": album,
        "xesam:albumArtist": dbus.Array([artist], signature="s"),
        "xesam:genre": dbus.Array(["Test", "Synth"], signature="s"),
    }
    if art:
        values["mpris:artUrl"] = art
    return dbus.Dictionary(values, signature="sv")


class Player(dbus.service.Object):
    def __init__(self, bus, name="org.mpris.MediaPlayer2.lwefake"):
        self.name = name
        self.bus = bus
        super().__init__(dbus.service.BusName(name, bus), "/org/mpris/MediaPlayer2")
        self.art = cover()
        self.tracks = [
            ("First Song", "Artist One", "Album One", self.art, 180),
            ("Second Song", "Artist Two", "Album Two", "http://127.0.0.1:9/cover.jpg", 240),
            ("Third Song", "Artist Three", "", None, 90),
        ]
        self.index = 0
        self.step = 0
        self.props = {
            ROOT: {"Identity": "Fake", "CanQuit": False, "CanRaise": False, "HasTrackList": False},
            PLAYER: {
                "PlaybackStatus": "Playing",
                "Metadata": metadata(*self.tracks[0]),
                "Position": dbus.Int64(0),
                "Volume": dbus.Double(0.8),
                "CanPlay": True,
                "CanPause": True,
                "CanControl": True,
            },
        }
        GLib.timeout_add(900, self.tick)

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.props[str(interface)][str(name)]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.props[str(interface)], signature="sv")

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ssv")
    def Set(self, interface, name, value):
        self.props[str(interface)][str(name)] = value

    @dbus.service.signal(dbus.PROPERTIES_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def change(self, **values):
        self.props[PLAYER].update(values)
        self.PropertiesChanged(PLAYER, dbus.Dictionary(values, signature="sv"), dbus.Array([], signature="s"))

    def tick(self):
        self.step += 1
        position = dbus.Int64(self.step * 3000000)
        self.props[PLAYER]["Position"] = position

        if self.step % 3 == 0:
            self.index = (self.index + 1) % len(self.tracks)
            self.change(Metadata=metadata(*self.tracks[self.index]), Position=dbus.Int64(0))
        elif self.step % 7 == 0:
            self.change(PlaybackStatus="Paused")
        elif self.step % 11 == 0:
            self.change(PlaybackStatus="Stopped", Metadata=dbus.Dictionary({}, signature="sv"))
        elif self.step % 5 == 0:
            self.change(PlaybackStatus="Playing", Metadata=metadata(*self.tracks[self.index]))
        else:
            self.change(Position=position)

        return True


class Churn:
    """LWE_FAKE_MPRIS_CHURN=1: a second player shows up, the first one leaves and comes back, the second one goes, so the
    media source sees several players, a vanished one and a switch"""

    def __init__(self, player):
        self.player = player
        self.second = None
        self.second_bus = None
        self.step = 0
        GLib.timeout_add(1000, self.tick)

    def tick(self):
        self.step += 1

        if self.step == 6:
            self.second_bus = dbus.SessionBus(private=True)
            self.second = Player(self.second_bus, "org.mpris.MediaPlayer2.lweother")
        elif self.step == 12:
            self.player.bus.release_name(self.player.name)
        elif self.step == 16:
            self.player.bus.request_name(self.player.name)
        elif self.step == 20 and self.second_bus is not None:
            self.second_bus.close()
            self.second_bus = None
            self.second = None

        return True


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    player = Player(dbus.SessionBus())

    if os.environ.get("LWE_FAKE_MPRIS_CHURN"):
        Churn(player)

    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
