"""GNOME on Wayland via the Shell extension: desktop window, per monitor pause, desktop menu, extension removal."""
import time
from pathlib import Path

import pytest

from conftest import REPO, changed_fraction
from desktops.runner import terminate

UUID = "linux-wallpaperengine@wallpaperenginelover.github.io"
WALLPAPER = "global.get_window_actors().map(a => a.meta_window).find(w => w.get_wm_class() === 'linux-wallpaperengine')"
POINTER = ("const C = imports.gi.Clutter, G = imports.gi.GLib;"
           "if (!global._lweLabPointer) global._lweLabPointer = C.get_default_backend().get_default_seat()"
           ".create_virtual_device(C.InputDeviceType.POINTER_DEVICE);"
           "const pointer = global._lweLabPointer, now = () => G.get_monotonic_time();")


CLICK_PROBE = REPO / "tests/desktops/fixtures/clickprobe"


def _engine(desktop, engine, log, *extra, wallpaper=None):
    return desktop.client([engine["binary"], "--assets-dir", engine["assets"], "--screen-root",
                           desktop.output_names()[0], "--silent", "--fps", "30", *extra,
                           str(wallpaper or engine["wallpaper"])],
                          log=log, env={"LWE_FRAME_STATS": "1", "LWE_SCRIPT_LOG": "1"})


def _point(desktop, x, y):
    """moves the lab's virtual pointer, again until the Shell has it (it sometimes keeps an older position)"""
    # the hot corner at the start position opens the overview
    desktop.eval(POINTER + f"pointer.notify_absolute_motion(now(), {x}, {y}); 1")
    time.sleep(0.5)
    desktop.eval("Main.overview.hide(); 1")
    time.sleep(0.5)
    for _ in range(20):
        desktop.eval(POINTER + f"pointer.notify_absolute_motion(now(), {x}, {y}); 1")
        time.sleep(0.25)
        if desktop.eval("global.get_pointer().slice(0, 2).join(',')") == f'"{x},{y}"':
            return
    raise AssertionError(f"the virtual pointer never got to {x},{y}")


def _click(desktop, button=1):
    desktop.eval(POINTER + f"pointer.notify_button(now(), {button}, C.ButtonState.PRESSED); 1")
    time.sleep(0.3)
    desktop.eval(POINTER + f"pointer.notify_button(now(), {button}, C.ButtonState.RELEASED); 1")


def _frame_lines(log):
    return Path(log).read_text(errors="replace").count("FRAME-STATS")


def _window(desktop, mode, name=None):
    """a GTK probe window, focused like a user would: Mutter maps it without the focus when the session never saw
    any input (focus stealing prevention), and WE counts only the focused window as fullscreen"""
    process = desktop.spawn(name or f"{mode}-probe", ["python3", str(REPO / "tools/desktops/wayland_window.py"), mode])
    title = f"lwe-{mode}-probe"
    for _ in range(40):
        time.sleep(0.25)
        if desktop.eval(f"const w = global.display.list_all_windows().find(w => w.get_title() === '{title}');"
                        "if (w) w.activate(global.display.get_current_time_roundtrip()); !!w") == "true":
            break
    return process


def _renders(log, seconds=5):
    before = _frame_lines(log)
    time.sleep(seconds)
    return _frame_lines(log) > before


# 49/50: Meta.Window.set_type, 46-48: helper's Meta.WaylandClient, 45: bottom normal-layer window
@pytest.fixture(params=["gnome", "gnome49", "gnome48", "gnome47", "gnome46", "gnome45"])
def gnome(request, desktop_factory):
    return desktop_factory(request.param)


def test_wallpaper_is_a_desktop_window(gnome, engine, tmp_path):
    process = _engine(gnome, engine, tmp_path / "engine.log")
    try:
        time.sleep(8)
        assert process.poll() is None, (tmp_path / "engine.log").read_text()[-1500:]
        state = gnome.eval(f"const w = {WALLPAPER}; const r = w.get_frame_rect();"
                           "[w.get_window_type(), w.skip_taskbar, w.is_fullscreen(), r.width, r.height].join(',')")
        width, height = gnome.size
        if gnome.name == "gnome45":
            # 45: normal window hidden from the window lists, kept in the work area
            window_type, skip, fullscreen, frame_width, frame_height = state.strip('"').split(",")
            assert (window_type, skip, fullscreen, frame_width) == ("0", "true", "false", str(width))
            assert int(frame_height) >= height - 64
        else:
            # Meta.WindowType.DESKTOP
            assert state == f'"1,true,false,{width},{height}"'
        assert gnome.eval("global.display.get_monitor_in_fullscreen(0)") == "false"
        assert gnome.eval("Shell.AppSystem.get_default().get_running().length") == "0"
    finally:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("mode", ["maximized", "fullscreen"])
def test_covering_window_pauses_rendering(gnome, engine, tmp_path, mode):
    log = tmp_path / "engine.log"
    process = _engine(gnome, engine, log)
    probe = None
    try:
        time.sleep(8)
        assert _renders(log), "the engine does not render on GNOME"

        probe = _window(gnome, mode)
        time.sleep(4)
        assert not _renders(log), f"still rendering under a {mode} window"

        terminate([probe.pid])
        probe = None
        time.sleep(4)
        assert _renders(log), f"rendering did not resume after the {mode} window"

        normal = _window(gnome, "normal")
        try:
            time.sleep(4)
            assert _renders(log), "a plain window paused the wallpaper"
        finally:
            terminate([normal.pid])
    finally:
        if probe is not None:
            terminate([probe.pid])
        process.terminate()
        process.wait(10)


def test_desktop_menu_opens_over_the_wallpaper(gnome, engine, tmp_path):
    process = _engine(gnome, engine, tmp_path / "engine.log")
    try:
        width, height = gnome.size
        gnome.eval(POINTER + f"pointer.notify_absolute_motion(now(), {width // 2}, {height // 2}); 1")
        time.sleep(0.5)
        gnome.eval("Main.overview.hide(); 1")
        time.sleep(8)
        # the lab pointer sometimes lags, resend until the Shell has it
        target = f"{width // 2},{height // 2}"
        for _ in range(20):
            gnome.eval(POINTER + f"pointer.notify_absolute_motion(now(), {width // 2}, {height // 2}); 1")
            time.sleep(0.25)
            if gnome.eval("global.get_pointer().slice(0, 2).join(',')") == f'"{target}"':
                break
        gnome.shot(tmp_path / "before.png")
        gnome.eval(POINTER + "pointer.notify_button(now(), 3, C.ButtonState.PRESSED);"
                   "pointer.notify_button(now() + 20000, 3, C.ButtonState.RELEASED); 1")
        time.sleep(2)
        opened = gnome.eval("Main.layoutManager._bgManagers.some(m => m.backgroundActor._backgroundMenu?.isOpen)")
        gnome.shot(tmp_path / "after.png")
        gnome.eval("Main.layoutManager._bgManagers.forEach(m => m.backgroundActor._backgroundMenu?.close()); 1")
        assert opened == "true", "a right click on the wallpaper did not reach GNOME's desktop menu"
        assert changed_fraction(tmp_path / "before.png", tmp_path / "after.png") > 0.01
    finally:
        process.terminate()
        process.wait(10)


def test_engine_stops_without_the_extension(gnome, engine, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(gnome, engine, log)
    try:
        time.sleep(6)
        assert process.poll() is None
        gnome.run(["gnome-extensions", "disable", UUID])
        process.wait(10)
        assert "GNOME Shell extension went away" in log.read_text(errors="replace")
        deadline = time.monotonic() + 10
        while gnome.eval(f"!!{WALLPAPER}") != "false" and time.monotonic() < deadline:
            time.sleep(0.5)
        assert gnome.eval(f"!!{WALLPAPER}") == "false", "the wallpaper window outlived the extension"

        refused = _engine(gnome, engine, tmp_path / "refused.log")
        assert refused.wait(20) != 0
        assert "without the GNOME Shell extension" in (tmp_path / "refused.log").read_text(errors="replace")
    finally:
        gnome.run(["gnome-extensions", "enable", UUID])
        if process.poll() is None:
            process.terminate()
            process.wait(10)


def test_overview_shows_the_live_wallpaper(gnome, engine, tmp_path):
    log = tmp_path / "engine.log"
    gnome.eval("Main.overview.show(); 1")
    time.sleep(3)
    gnome.shot(tmp_path / "bare.png")
    gnome.eval("Main.overview.hide(); 1")
    time.sleep(2)

    process = _engine(gnome, engine, log)
    try:
        time.sleep(8)
        gnome.eval("Main.overview.show(); 1")
        time.sleep(3)
        gnome.shot(tmp_path / "overview.png")
        # only the clones keep frame callbacks coming in the overview
        assert _renders(log), "the wallpaper stopped rendering in the overview"
        assert changed_fraction(tmp_path / "bare.png", tmp_path / "overview.png") > 0.2, \
            "the overview still shows GNOME's own background"
    finally:
        gnome.eval("Main.overview.hide(); 1")
        process.terminate()
        process.wait(10)


def test_focused_window_pauses_with_pause_on_focus(gnome, engine, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(gnome, engine, log, "--pause-on-focus")
    probe = None
    try:
        time.sleep(8)
        assert _renders(log), "the engine does not render on GNOME"
        probe = _window(gnome, "normal", "focus-probe")
        time.sleep(4)
        assert not _renders(log), "--pause-on-focus kept rendering while another application had the focus"
        terminate([probe.pid])
        probe = None
        time.sleep(4)
        assert _renders(log), "rendering did not resume once the focused window was gone"
    finally:
        if probe is not None:
            terminate([probe.pid])
        process.terminate()
        process.wait(10)


# DING: own desktop window on GNOME 50, bottom-layer normal one on Ubuntu 46
@pytest.fixture(params=["gnome-ding", "gnome46-ding", "gnome45-ding"])
def ding(request, desktop_factory):
    return desktop_factory(request.param)


def test_wallpaper_stays_under_desktop_icons(ding, engine, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(ding, engine, log, "--pause-on-focus", wallpaper=CLICK_PROBE)
    try:
        time.sleep(8)
        order = ding.eval("global.display.sort_windows_by_stacking(global.display.list_all_windows())"
                          ".map(w => w.get_gtk_application_id() || w.get_wm_class()).join('|')").strip('"').split("|")
        icons = order.index("com.rastersoft.ding")
        wallpaper = order.index("linux-wallpaperengine")
        assert wallpaper < icons, f"the wallpaper is above the desktop icons: {order}"

        # WE counts the icon view as the desktop: clicks reach the wallpaper, focusing it isn't another app
        width, height = ding.size
        _point(ding, width // 2, height // 2)
        _click(ding)
        time.sleep(1.5)
        text = log.read_text(errors="replace")
        assert "LWE-CLICK down" in text and "LWE-CLICK up" in text, "the click on the desktop never reached the wallpaper"
        assert ding.eval("global.display.focus_window?.get_gtk_application_id() ?? ''") == '"com.rastersoft.ding"'
        assert _renders(log), "focusing the desktop icons paused the wallpaper"
    finally:
        process.terminate()
        process.wait(10)
