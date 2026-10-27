"""A session that reports itself locked pauses the engine, and unlocking resumes it."""
import os
import subprocess
import time

import pytest

from test_kde import REPO, _engine, _renders


@pytest.fixture
def screen_saver(desktop, tmp_path):
    state = tmp_path / "screen-saver-active"
    process = desktop.spawn("screen-saver", ["python3", str(REPO / "tools/desktops/screen_saver_owner.py"), str(state)])
    time.sleep(2)

    def send(active):
        if active:
            state.touch()
        else:
            state.unlink(missing_ok=True)

    yield send
    process.terminate()


@pytest.mark.parametrize("desktop", ["sway", "labwc"], indirect=True)
def test_screen_saver_signal_pauses(desktop, engine, screen_saver, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(desktop, engine, log, desktop.output_names()[0])
    try:
        time.sleep(8)
        assert _renders(log), "the engine does not render"
        screen_saver(True)
        time.sleep(3)
        assert not _renders(log), "still rendering while the screen saver is active"
        assert "Paused: lock" in log.read_text(errors="replace")
        screen_saver(False)
        time.sleep(3)
        assert _renders(log), "rendering did not resume after the screen saver ended"
    finally:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", ["sway"], indirect=True)
def test_no_lock_pause_keeps_rendering(desktop, engine, screen_saver, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(desktop, engine, log, desktop.output_names()[0], "--no-lock-pause")
    try:
        time.sleep(8)
        screen_saver(True)
        time.sleep(3)
        assert _renders(log), "--no-lock-pause still paused"
    finally:
        process.terminate()
        process.wait(10)


@pytest.fixture
def logind(tmp_path):
    dbusmock = pytest.importorskip("dbusmock")
    bus = dbusmock.DBusTestCase
    bus.start_system_bus()
    bus.setUpClass()
    server, mock = bus.spawn_server_template("logind", {}, stdout=subprocess.DEVNULL)
    import dbus

    manager = dbus.Interface(bus.get_dbus(True).get_object("org.freedesktop.login1", "/org/freedesktop/login1"),
                             "org.freedesktop.DBus.Mock")
    session = manager.AddSession("c1", "seat0", 1000, "lwe", True)
    yield os.environ["DBUS_SYSTEM_BUS_ADDRESS"], session
    server.terminate()
    server.wait()
    bus.tearDownClass()


@pytest.mark.parametrize("desktop", ["sway"], indirect=True)
def test_logind_locked_hint_pauses(desktop, engine, logind, tmp_path):
    import dbus

    address, session = logind
    log = tmp_path / "engine.log"
    process = desktop.client(
        [engine["binary"], "--assets-dir", engine["assets"], "--screen-root", desktop.output_names()[0], "--silent",
         "--fps", "30", engine["wallpaper"]],
        log=log, env={"LWE_FRAME_STATS": "1", "DBUS_SYSTEM_BUS_ADDRESS": address, "XDG_SESSION_ID": "c1"})
    hint = dbus.Interface(dbus.bus.BusConnection(address).get_object("org.freedesktop.login1", session),
                          "org.freedesktop.login1.Session")
    try:
        time.sleep(8)
        assert _renders(log), "the engine does not render"
        hint.SetLockedHint(True)
        time.sleep(3)
        assert not _renders(log), "still rendering with LockedHint set"
        hint.SetLockedHint(False)
        time.sleep(3)
        assert _renders(log), "rendering did not resume when LockedHint cleared"
    finally:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", ["i3", "xfce"], indirect=True)
def test_x11_dpms_off_pauses(desktop, engine, tmp_path):
    display = {"DISPLAY": f":{desktop.display}"}
    probe = desktop.run(["xset", "q"], display)

    if "does not have the DPMS" in probe.stdout:
        pytest.skip(f"the X server of {desktop.name} has no DPMS extension")

    log = tmp_path / "engine.log"
    process = desktop.client(
        [engine["binary"], "--assets-dir", engine["assets"], "--screen-root", desktop.output_names()[0], "--silent",
         "--fps", "30", engine["wallpaper"]], log=log, env={"LWE_FRAME_STATS": "1"})
    try:
        desktop.run(["xset", "+dpms", "s", "off"], display)
        time.sleep(8)
        assert _renders(log), "the engine does not render"
        desktop.run(["xset", "dpms", "force", "off"], display)
        time.sleep(4)
        assert not _renders(log), "still rendering with the display in DPMS off"
        assert "Paused: display off" in log.read_text(errors="replace")
        desktop.run(["xset", "dpms", "force", "on"], display)
        time.sleep(4)
        assert _renders(log), "rendering did not resume after DPMS on"
    finally:
        process.terminate()
        process.wait(10)


def _power_save(desktop, mode):
    result = desktop.run(["busctl", "--user", "set-property", "org.gnome.Mutter.DisplayConfig",
                          "/org/gnome/Mutter/DisplayConfig", "org.gnome.Mutter.DisplayConfig", "PowerSaveMode", "i",
                          str(mode)])
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("desktop", ["gnome"], indirect=True)
def test_mutter_power_save_mode_pauses(desktop, engine, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(desktop, engine, log, desktop.output_names()[0])
    try:
        time.sleep(10)
        assert _renders(log), "the engine does not render"
        _power_save(desktop, 3)
        time.sleep(4)
        assert not _renders(log), "still rendering with PowerSaveMode off"
        assert "Paused: display off" in log.read_text(errors="replace")
        _power_save(desktop, 0)
        time.sleep(4)
        assert _renders(log), "rendering did not resume after PowerSaveMode on"
    finally:
        process.terminate()
        process.wait(10)
