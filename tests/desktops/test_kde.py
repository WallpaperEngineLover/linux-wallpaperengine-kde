"""KDE Plasma: the KWin window-state script pauses only the affected output."""
import time
from pathlib import Path

import pytest

from conftest import REPO
from desktops.runner import terminate


def _engine(desktop, engine, log, output, *extra):
    return desktop.client([engine["binary"], "--assets-dir", engine["assets"], "--screen-root", output, "--silent",
                           "--fps", "30", *extra, engine["wallpaper"]], log=log, env={"LWE_FRAME_STATS": "1"})


def _renders(log, seconds=5):
    before = Path(log).read_text(errors="replace").count("FRAME-STATS")
    time.sleep(seconds)
    return Path(log).read_text(errors="replace").count("FRAME-STATS") > before


def _window(desktop, mode):
    return desktop.spawn(f"{mode}-probe", ["python3", str(REPO / "tools/desktops/wayland_window.py"), mode])


@pytest.fixture
def kde(desktop_factory):
    return desktop_factory("kde")


@pytest.mark.parametrize("mode", ["maximized", "fullscreen"])
def test_covering_window_pauses_rendering(kde, engine, tmp_path, mode):
    log = tmp_path / "engine.log"
    process = _engine(kde, engine, log, kde.output_names()[0])
    probe = None
    try:
        time.sleep(8)
        assert _renders(log), "the engine does not render on KDE"
        probe = _window(kde, mode)
        time.sleep(4)
        assert not _renders(log), f"still rendering under a {mode} window"
        terminate([probe.pid])
        probe = None
        time.sleep(4)
        assert _renders(log), f"rendering did not resume after the {mode} window"
    finally:
        if probe is not None:
            terminate([probe.pid])
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("pause_on_focus", [False, True])
def test_focused_window(kde, engine, tmp_path, pause_on_focus):
    log = tmp_path / "engine.log"
    process = _engine(kde, engine, log, kde.output_names()[0], *(["--pause-on-focus"] if pause_on_focus else []))
    probe = None
    try:
        time.sleep(8)
        probe = _window(kde, "normal")
        time.sleep(4)
        if pause_on_focus:
            assert not _renders(log), "--pause-on-focus kept rendering while another application had the focus"
        else:
            assert _renders(log), "a plain window paused the wallpaper"
    finally:
        if probe is not None:
            terminate([probe.pid])
        process.terminate()
        process.wait(10)


def test_pause_is_per_output(desktop_factory, engine, tmp_path):
    kde = desktop_factory("kde-dual")
    outputs = kde.output_names()
    assert len(outputs) == 2, outputs
    logs = [tmp_path / f"engine-{output}.log" for output in outputs]
    # one engine per screen, like we_manager
    processes = [_engine(kde, engine, log, output) for log, output in zip(logs, outputs)]
    probe = None
    try:
        time.sleep(8)
        assert all(_renders(log) for log in logs)
        probe = _window(kde, "maximized")
        time.sleep(4)
        rendering = [_renders(log) for log in logs]
        assert rendering.count(False) == 1, f"a maximized window should pause exactly its own output: {rendering}"
    finally:
        if probe is not None:
            terminate([probe.pid])
        for process in processes:
            process.terminate()
            process.wait(10)


def test_plasma5(desktop_factory, engine, tmp_path):
    """KWin 5.27 runs both scripts (/<id>, not /Scripting/Script<id>); the window script says it needs Plasma 6"""
    kde5 = desktop_factory("kde5")
    log = tmp_path / "engine.log"
    process = _engine(kde5, engine, log, kde5.output_names()[0])
    probe = None
    try:
        time.sleep(8)
        assert _renders(log), "the engine does not render on Plasma 5"
        text = log.read_text(errors="replace")
        assert "could not load its KWin script" not in text
        assert "KDE window detection needs Plasma 6" in text
        probe = _window(kde5, "maximized")
        time.sleep(4)
        assert _renders(log), "a maximized window paused the wallpaper without window detection"
    finally:
        if probe is not None:
            terminate([probe.pid])
        process.terminate()
        process.wait(10)
