import re
import time
from pathlib import Path

import pytest

from conftest import Unsupported

POSITION_PROBE = Path(__file__).parent / "fixtures/posprobe"

# WE 2.8.42 is per monitor DPI aware and sizes its swap chain from the client rect, so it draws every monitor at its
# native pixel size whatever the scaling; on a 1.5x output that takes wp_fractional_scale_v1 + wp_viewporter
SCALE = 1.5

SESSIONS = ["sway", "labwc", "wayfire", "hyprland", "niri", "kde", "gnome"]
POINTER_SESSIONS = ["sway", "hyprland", "niri"]

BUFFER = re.compile(r"create_immed\(new id wl_buffer#\d+, (\d+), (\d+),")
DESTINATION = re.compile(r"wp_viewport#\d+\.set_destination\((\d+), (\d+)\)")


def _set_scale(desktop):
    try:
        desktop.set_scale(SCALE)
    except Unsupported as error:
        pytest.skip(str(error))
    except Exception:
        # some outputs may have changed before the failure, the session is shared with the next tests
        desktop.set_scale(1)
        raise


@pytest.mark.parametrize("desktop", SESSIONS, indirect=True)
def test_buffer_has_the_output_pixel_size(desktop, engine, tmp_path):
    _set_scale(desktop)

    log = tmp_path / "engine.log"
    process = None
    try:
        process = desktop.client([engine["binary"], "--assets-dir", engine["assets"], "--screen-root",
                                  desktop.output_names()[0], "--silent", "--fps", "30", engine["wallpaper"]],
                                 log=log, env={"WAYLAND_DEBUG": "1"})
        width, height = desktop.size
        logical = (round(width / SCALE), round(height / SCALE))
        deadline = time.monotonic() + 40
        buffers, destinations = [], []
        while time.monotonic() < deadline and not (buffers and destinations):
            time.sleep(2)
            assert process.poll() is None, f"the engine exited with {process.returncode}:\n{log.read_text()[-1500:]}"
            text = log.read_text(errors="replace")
            buffers = [tuple(map(int, found)) for found in BUFFER.findall(text)]
            destinations = [tuple(map(int, found)) for found in DESTINATION.findall(text)]
        assert buffers, f"no buffer was attached on {desktop.name}"
        assert buffers[-1] == (width, height), f"{desktop.name} at {SCALE}x got {buffers[-1]} buffers"
        assert destinations and destinations[-1] == logical, f"viewport destination {destinations[-1:]}"
    finally:
        if process is not None:
            process.terminate()
            process.wait(10)
        desktop.set_scale(1)


@pytest.mark.parametrize("desktop", POINTER_SESSIONS, indirect=True)
def test_pointer_is_in_buffer_pixels(desktop, engine, tmp_path):
    """WE gives scripts the cursor in the window's physical client pixels"""
    _set_scale(desktop)

    log = tmp_path / "engine.log"
    pointer = process = None
    try:
        pointer = desktop.virtual_pointer()
        process = desktop.client([engine["binary"], "--assets-dir", engine["assets"], "--screen-root",
                                  desktop.output_names()[0], "--silent", "--fps", "30", str(POSITION_PROBE)],
                                 log=log, env={"LWE_SCRIPT_LOG": "1"})
        width, height = desktop.size
        logical = (round(width / SCALE), round(height / SCALE))
        expected = f"LWE-POS {width / 4:.1f} {height / 4:.1f}"
        deadline = time.monotonic() + 40
        seen = []
        while time.monotonic() < deadline and expected not in seen:
            assert process.poll() is None, f"the engine exited with {process.returncode}:\n{log.read_text()[-1500:]}"
            # a quarter of the logical output from the top left, nudged so every round is a motion
            for x in (logical[0] // 4 + 1, logical[0] // 4):
                pointer.stdin.write(f"abs {x} {logical[1] // 4} {logical[0]} {logical[1]}\n")
                pointer.stdin.flush()
                pointer.stdout.readline()
                time.sleep(0.3)
            seen = re.findall(r"LWE-POS [-\d.]+ [-\d.]+", log.read_text(errors="replace"))
        assert expected in seen, f"{desktop.name} at {SCALE}x: {seen[-3:]}"
    finally:
        if pointer is not None:
            pointer.stdin.close()
            pointer.wait(10)
        if process is not None:
            process.terminate()
            process.wait(10)
        desktop.set_scale(1)


@pytest.mark.parametrize("desktop", ["sway", "labwc", "niri"], indirect=True)
def test_render_scale_shrinks_the_buffer_only(desktop, engine, tmp_path):
    """--render-scale draws below the output's pixels, the viewport destination stays the logical size"""
    log = tmp_path / "engine.log"
    process = desktop.client([engine["binary"], "--assets-dir", engine["assets"], "--screen-root",
                              desktop.output_names()[0], "--render-scale", "0.5", "--silent", "--fps", "30",
                              engine["wallpaper"]], log=log, env={"WAYLAND_DEBUG": "1"})
    try:
        width, height = desktop.size
        deadline = time.monotonic() + 40
        buffers, destinations = [], []
        while time.monotonic() < deadline and not (buffers and destinations):
            time.sleep(2)
            assert process.poll() is None, f"the engine exited with {process.returncode}:\n{log.read_text()[-1500:]}"
            text = log.read_text(errors="replace")
            buffers = [tuple(map(int, found)) for found in BUFFER.findall(text)]
            destinations = [tuple(map(int, found)) for found in DESTINATION.findall(text)]
        assert buffers and destinations, f"nothing was attached on {desktop.name}"
        assert buffers[-1] == (round(width / 2), round(height / 2)), f"buffer {buffers[-1]}"
        assert destinations[-1] == (width, height), f"viewport destination {destinations[-1]}"
    finally:
        process.terminate()
        process.wait(10)
