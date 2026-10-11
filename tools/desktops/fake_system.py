#!/usr/bin/env python3
"""A private D-Bus system bus with stand-ins for the services a desktop expects from systemd and friends.
Usage: fake_system.py <address-file>   (stays in the foreground, writes the bus address first)"""
import os
import signal
import sys

import dbus
import dbusmock


class Lab(dbusmock.DBusTestCase):
    pass


def custom(name, path, interface, properties):
    Lab.spawn_server(name, path, interface, system_bus=True)
    obj = dbus.Interface(Lab.get_dbus(True).get_object(name, path), dbusmock.MOCK_IFACE)
    obj.AddProperties(interface, dbus.Dictionary(properties, signature="sv"))
    return obj


def main():
    Lab.start_system_bus()
    address = os.environ["DBUS_SYSTEM_BUS_ADDRESS"]
    for template in ("logind", "upower", "networkmanager", "polkitd", "timedated", "power_profiles_daemon"):
        Lab.spawn_server_template(template, {}, stdout=open(os.devnull, "w"))
    custom(
        "org.freedesktop.locale1", "/org/freedesktop/locale1", "org.freedesktop.locale1",
        {"X11Layout": "us", "X11Model": "pc105", "X11Variant": "", "X11Options": "", "VConsoleKeymap": "us",
         "Locale": dbus.Array(["LANG=C.UTF-8"], signature="s")},
    )
    custom(
        "org.freedesktop.hostname1", "/org/freedesktop/hostname1", "org.freedesktop.hostname1",
        {"Hostname": "lwe-test", "StaticHostname": "lwe-test", "PrettyHostname": "", "IconName": "computer"},
    )
    with open(sys.argv[1], "w") as out:
        out.write(address + "\n")
    signal.pause()


signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
main()
