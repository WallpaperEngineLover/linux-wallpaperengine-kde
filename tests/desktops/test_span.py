"""--screen-span on two outputs: WE makes a stretch group one monitor with the bounding box of its monitors
(wallpaper64.exe 2.8.42 sub_140061B50), so the cursor is in that box's pixels wherever it is (sub_140110630)."""
import json
import os
import signal
import time

import pytest

from test_fractional_scale import POSITION_PROBE

LAYOUTS = {
    # name: (outputs as (x, y, scale), pointer positions in the layout)
    "side": (((0, 0, 1), (1280, 0, 1)), ((100, 100), (1290, 100), (2550, 710))),
    "gap": (((0, 0, 1), (1480, 200, 1)), ((100, 100), (1490, 300), (2750, 910))),
    "mixed": (((0, 0, 1), (1280, 0, 2)), ((100, 100), (1290, 100), (1910, 350))),
}


@pytest.fixture
def sway(desktop_factory):
    return desktop_factory("sway-dual")


def _swaymsg(sway, *command):
    result = sway.run(["swaymsg", *command], sway.extra)
    assert result.returncode == 0, result.stderr or result.stdout
    return result.stdout


def _layout(sway, outputs):
    for index, (x, y, scale) in enumerate(outputs, 1):
        _swaymsg(sway, f"output HEADLESS-{index} enable mode 1280x720 pos {x} {y} scale {scale}")


def _engine(sway, engine, log, wallpaper, *extra):
    return sway.client([engine["binary"], "--assets-dir", engine["assets"], "--silent", "--fps", "30", *extra,
                        "--screen-span", "HEADLESS-1,HEADLESS-2", "--bg", str(wallpaper)],
                       log=log, env={"LWE_SCRIPT_LOG": "1"})


@pytest.mark.parametrize("layout", LAYOUTS)
def test_cursor_is_in_group_pixels(sway, engine, tmp_path, layout):
    outputs, points = LAYOUTS[layout]
    log = tmp_path / "engine.log"
    pointer = sway.virtual_pointer()
    process = None
    try:
        _layout(sway, outputs)
        rects = [output["rect"] for output in json.loads(_swaymsg(sway, "-t", "get_outputs", "-r"))]
        left, top = min(rect["x"] for rect in rects), min(rect["y"] for rect in rects)
        width = max(rect["x"] + rect["width"] for rect in rects)
        height = max(rect["y"] + rect["height"] for rect in rects)
        scale = max(scale for _, _, scale in outputs)

        process = _engine(sway, engine, log, POSITION_PROBE)
        time.sleep(8)
        for x, y in points:
            expected = f"LWE-POS {(x - left) * scale:.1f} {(y - top) * scale:.1f}"
            seen = None
            for _ in range(10):
                assert process.poll() is None, f"the engine exited with {process.returncode}"
                for dx in (1, 0):
                    pointer.stdin.write(f"abs {x + dx} {y} {width} {height}\n")
                    pointer.stdin.flush()
                    pointer.stdout.readline()
                    time.sleep(0.3)
                lines = [line for line in log.read_text(errors="replace").splitlines() if "LWE-POS" in line]
                seen = lines[-1].strip() if lines else None
                if seen == expected:
                    break
            assert seen == expected, f"pointer at {x},{y} in the {layout} layout"
    finally:
        pointer.stdin.write("quit\n")
        pointer.stdin.flush()
        if process is not None:
            process.terminate()
            process.wait(10)
        _layout(sway, LAYOUTS["side"][0])


def test_group_survives_hotswap_resize_and_unplug(sway, engine, tmp_path):
    _layout(sway, LAYOUTS["side"][0])
    log = tmp_path / "engine.log"
    control = tmp_path / "control"
    process = _engine(sway, engine, log, engine["wallpaper"], "--control-file", str(control))

    def spans():
        return [line for line in log.read_text(errors="replace").splitlines() if line.startswith("Span ")]

    try:
        time.sleep(8)
        steps = [
            ("output HEADLESS-2 mode 1920x1080", "3200x1080 at 0,0, rendered at 1x"),
            ("output HEADLESS-2 mode 1280x720 scale 2", "1920x720 at 0,0, rendered at 2x"),
            ("output HEADLESS-2 scale 1", "2560x720 at 0,0, rendered at 1x"),
            ("output HEADLESS-1 disable", "1280x720 at 1280,0, rendered at 1x"),
            ("output HEADLESS-1 enable", "2560x720 at 0,0, rendered at 1x"),
        ]
        for command, expected in steps:
            _swaymsg(sway, command)
            time.sleep(4)
            assert process.poll() is None, f"the engine exited with {process.returncode} after {command}"
            assert spans() and spans()[-1].endswith(expected), f"after {command}: {spans()[-1:]}"

        # the group's one wallpaper is replaced on both outputs, the old project must not be read afterwards
        control.write_text(f"path={POSITION_PROBE}\n")
        os.kill(process.pid, signal.SIGUSR1)
        time.sleep(6)
        assert process.poll() is None, f"the engine exited with {process.returncode} after the hotswap"
    finally:
        process.terminate()
        process.wait(10)
        _layout(sway, LAYOUTS["side"][0])
