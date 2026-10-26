"""What an X11 desktop needs from the engine besides the picture: its own desktop menu keeps working on top of the
wallpaper, and a fullscreen window pauses rendering."""
import os
import time
from pathlib import Path

import pytest

from conftest import REPO, KINDS, changed_fraction
from desktops.session import X11

X11_SESSIONS = [name for name, kind in KINDS.items() if issubclass(kind, X11)]
# the desktops that draw a desktop window with a context menu of their own
DESKTOP_MENUS = ["xfce", "mate", "lxqt", "cinnamon", "kde-x11"]


def _engine(desktop, engine, log):
    return desktop.client([engine["binary"], "--assets-dir", engine["assets"], "--screen-root",
                           desktop.output_names()[0], "--silent", "--fps", "30", engine["wallpaper"]],
                          log=log, env={"LWE_FRAME_STATS": "1"})


def _x(desktop, *argv):
    return desktop.run(list(argv), {"DISPLAY": f":{desktop.display}"})


def _frame_lines(log):
    return Path(log).read_text(errors="replace").count("FRAME-STATS")


def _menu_opens(desktop, tmp_path, tag):
    """right click on empty desktop, whether a menu showed up next to the pointer"""
    width, height = desktop.size
    x, y = width * 3 // 4, height * 3 // 4
    _x(desktop, "xdotool", "mousemove", str(x), str(y))
    time.sleep(1)
    desktop.shot(tmp_path / f"{tag}-before.png")
    _x(desktop, "xdotool", "click", "3")
    time.sleep(2)
    desktop.shot(tmp_path / f"{tag}-after.png")
    _x(desktop, "xdotool", "key", "Escape")
    time.sleep(1)
    # the wallpaper itself barely moves in a few seconds, a menu is a few percent of the screen
    return changed_fraction(tmp_path / f"{tag}-before.png", tmp_path / f"{tag}-after.png") > 0.01


@pytest.mark.parametrize("desktop", DESKTOP_MENUS, indirect=True)
def test_desktop_menu_opens_over_the_wallpaper(desktop, engine, tmp_path):
    if not _menu_opens(desktop, tmp_path, "bare"):
        pytest.skip(f"{desktop.name} opens no desktop menu in this session even without the engine")
    process = _engine(desktop, engine, tmp_path / "engine.log")
    try:
        time.sleep(8)
        assert _menu_opens(desktop, tmp_path, "engine"), \
            f"the desktop menu of {desktop.name} no longer opens with the wallpaper running"
    finally:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", X11_SESSIONS, indirect=True)
def test_fullscreen_window_pauses_rendering(desktop, engine, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(desktop, engine, log)
    probe = None
    try:
        time.sleep(8)
        before = _frame_lines(log)
        time.sleep(5)
        assert _frame_lines(log) > before, f"the engine does not render on {desktop.name}"

        probe = desktop.spawn("fullscreen-probe", ["python3", str(REPO / "tools/desktops/fullscreen_window.py")],
                              {"DISPLAY": f":{desktop.display}"})
        time.sleep(4)
        paused = _frame_lines(log)
        time.sleep(5)
        assert _frame_lines(log) == paused, f"{desktop.name}: still rendering under a fullscreen window"

        os.killpg(probe.pid, 15)
        probe.wait(10)
        probe = None
        time.sleep(4)
        resumed = _frame_lines(log)
        time.sleep(5)
        assert _frame_lines(log) > resumed, f"{desktop.name}: rendering did not resume after the fullscreen window"
    finally:
        if probe is not None:
            os.killpg(probe.pid, 9)
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", X11_SESSIONS, indirect=True)
def test_maximized_window_pauses_rendering(desktop, engine, tmp_path):
    if desktop.name == "herbstluftwm":
        pytest.skip("herbstluftwm tiles windows and never sets the maximized state")
    log = tmp_path / "engine.log"
    process = _engine(desktop, engine, log)
    probe = None
    try:
        time.sleep(8)
        before = _frame_lines(log)
        time.sleep(5)
        assert _frame_lines(log) > before, f"the engine does not render on {desktop.name}"

        probe = desktop.spawn("maximized-probe", ["python3", str(REPO / "tools/desktops/fullscreen_window.py"), "maximize"],
                              {"DISPLAY": f":{desktop.display}"})
        time.sleep(4)
        paused = _frame_lines(log)
        time.sleep(5)
        assert _frame_lines(log) == paused, f"{desktop.name}: still rendering under a maximized window"

        os.killpg(probe.pid, 15)
        probe.wait(10)
        probe = None
        time.sleep(4)
        resumed = _frame_lines(log)
        time.sleep(5)
        assert _frame_lines(log) > resumed, f"{desktop.name}: rendering did not resume after the maximized window"
    finally:
        if probe is not None:
            os.killpg(probe.pid, 9)
        process.terminate()
        process.wait(10)
