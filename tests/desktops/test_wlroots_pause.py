"""wlroots compositors: the foreign toplevel detector pauses only the outputs a window is on."""
import time

import pytest

from conftest import REPO
from test_kde import _engine, _renders
from desktops.runner import Userland, terminate

DUAL_SESSIONS = ["sway-dual", "labwc-dual"]


def _engines(desktop, engine, tmp_path, *extra):
    outputs = desktop.output_names()
    assert len(outputs) == 2, outputs
    logs = [tmp_path / f"engine-{output}.log" for output in outputs]
    # one engine per screen, like we_manager
    return logs, [_engine(desktop, engine, log, output, *extra) for log, output in zip(logs, outputs)]


def _window(desktop, mode):
    # GTK from the image, the Arch rootfs has no PyGObject
    return desktop.spawn(f"{mode}-probe", ["python3", str(REPO / "tools/desktops/wayland_window.py"), mode],
                         userland=Userland("native"))


def _stop(probe, processes):
    if probe is not None:
        terminate([probe.pid])
    for process in processes:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", DUAL_SESSIONS, indirect=True)
def test_fullscreen_pauses_its_output_only(desktop, engine, tmp_path):
    logs, processes = _engines(desktop, engine, tmp_path)
    probe = None
    try:
        time.sleep(8)
        assert all(_renders(log) for log in logs), "the engines do not render"
        probe = _window(desktop, "fullscreen")
        time.sleep(4)
        rendering = [_renders(log) for log in logs]
        assert rendering.count(False) == 1, f"a fullscreen window should pause exactly its own output: {rendering}"
        terminate([probe.pid])
        probe = None
        time.sleep(4)
        assert all(_renders(log) for log in logs), "rendering did not resume after the fullscreen window"
    finally:
        _stop(probe, processes)


@pytest.mark.parametrize("pause_on_maximized", [False, True])
def test_maximized_window(desktop_factory, engine, tmp_path, pause_on_maximized):
    labwc = desktop_factory("labwc-dual")
    logs, processes = _engines(labwc, engine, tmp_path, *(["--pause-on-maximized"] if pause_on_maximized else []))
    probe = None
    try:
        time.sleep(8)
        probe = _window(labwc, "maximized")
        time.sleep(4)
        rendering = [_renders(log) for log in logs]
        if pause_on_maximized:
            assert rendering.count(False) == 1, f"a maximized window should pause exactly its own output: {rendering}"
        else:
            assert all(rendering), f"a maximized window paused without --pause-on-maximized: {rendering}"
    finally:
        _stop(probe, processes)


def test_fullscreen_follows_its_window(desktop_factory, engine, tmp_path):
    sway = desktop_factory("sway-dual")
    logs, processes = _engines(sway, engine, tmp_path)
    outputs = sway.output_names()
    probe = None
    try:
        time.sleep(8)
        probe = _window(sway, "fullscreen")
        time.sleep(4)
        first = [_renders(log) for log in logs]
        assert first.count(False) == 1, first
        other = outputs[first.index(True)]
        moved = sway.run(["swaymsg", "[app_id=org.lwe.Probe] move container to output " + other], sway.extra)
        assert moved.returncode == 0, moved.stderr or moved.stdout
        time.sleep(4)
        second = [_renders(log) for log in logs]
        assert second == [not was for was in first], f"the pause did not move with the window: {first} -> {second}"
    finally:
        _stop(probe, processes)
