"""--screen-slot: several wallpapers side by side on one output, each in its own rect with its own cursor and clicks"""
import json
import os
import re
import signal
import time

import pytest
from PIL import Image

RED, GREEN, BLUE = (255, 0, 0), (0, 255, 0), (0, 0, 255)


@pytest.fixture
def sway(desktop_factory):
    return desktop_factory("sway")


def _probe(directory, tag, size, color):
    """a solid colour scene of the given projection size that logs its cursor and presses with a tag"""
    directory.mkdir(parents=True, exist_ok=True)
    width, height = size
    script = ("'use strict';\n"
              f"export function cursorMove(event) {{ console.log('LWE-POS-{tag} ' + input.cursorScreenPosition.x.toFixed(1)"
              " + ' ' + input.cursorScreenPosition.y.toFixed(1)); }\n"
              f"export function cursorDown(event) {{ console.log('LWE-DOWN-{tag}'); }}\n"
              "export function update(value) { return value; }\n")
    scene = {
        "version": 3,
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"orthogonalprojection": {"width": width, "height": height}, "clearcolor": "0 0 0",
                    "bloom": False, "cameraparallax": False},
        "objects": [{"id": 1, "name": "probe", "image": "models/util/solidlayer.json", "size": f"{width} {height}",
                     "origin": f"{width / 2} {height / 2} 0", "color": " ".join(str(c / 255) for c in color),
                     "visible": True, "alpha": {"script": script, "value": 1.0}}],
    }
    (directory / "scene.json").write_text(json.dumps(scene))
    (directory / "project.json").write_text(json.dumps({"file": "scene.json", "type": "scene", "title": tag,
                                                        "general": {"properties": {}}}))
    return directory


def _engine(sway, engine, log, *slots, after=(), control=None):
    output = sway.output_names()[0]
    argv = [engine["binary"], "--assets-dir", engine["assets"], "--silent", "--fps", "30"]
    if control is not None:
        argv += ["--control-file", str(control)]
    for wallpaper in slots:
        argv += ["--screen-slot", output, "--bg", str(wallpaper)]
    argv += after
    return sway.client(argv, log=log, env={"LWE_SCRIPT_LOG": "1"})


def _pixel(sway, tmp_path, x, y):
    path = tmp_path / "shot.png"
    sway.shot(path)
    return Image.open(path).convert("RGB").getpixel((x, y))


def _wait_for_pixel(sway, tmp_path, process, x, y, color, log):
    seen = None
    for _ in range(20):
        assert process.poll() is None, f"the engine exited with {process.returncode}:\n{log.read_text()[-1500:]}"
        seen = _pixel(sway, tmp_path, x, y)
        if all(abs(a - b) < 8 for a, b in zip(seen, color)):
            return
        time.sleep(1)
    pytest.fail(f"{x},{y} is {seen}, expected {color}")


def _stop(process):
    if process is not None:
        process.terminate()
        process.wait(10)


def test_slots_are_sized_from_the_wallpapers(sway, engine, tmp_path):
    # 1080x1920 + 1920x1080 on 1920x1080: the first slot ends at 462
    portrait = _probe(tmp_path / "a", "A", (1080, 1920), RED)
    landscape = _probe(tmp_path / "b", "B", (1920, 1080), GREEN)
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, portrait, landscape)
    try:
        _wait_for_pixel(sway, tmp_path, process, 455, 540, RED, log)
        _wait_for_pixel(sway, tmp_path, process, 470, 540, GREEN, log)
        _wait_for_pixel(sway, tmp_path, process, 1910, 1070, GREEN, log)
    finally:
        _stop(process)


def test_center_layout_leaves_bars(sway, engine, tmp_path):
    first = _probe(tmp_path / "a", "A", (1080, 1920), RED)
    second = _probe(tmp_path / "b", "B", (1080, 1920), GREEN)
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, first, second, after=("--split-layout", "center"))
    try:
        # two 607.5 px slots from 352.5 to 1567.5
        _wait_for_pixel(sway, tmp_path, process, 340, 540, (0, 0, 0), log)
        _wait_for_pixel(sway, tmp_path, process, 365, 540, RED, log)
        _wait_for_pixel(sway, tmp_path, process, 1555, 540, GREEN, log)
        _wait_for_pixel(sway, tmp_path, process, 1580, 540, (0, 0, 0), log)
    finally:
        _stop(process)


def test_portrait_output_stacks_slots(sway, engine, tmp_path):
    output = sway.output_names()[0]
    first = _probe(tmp_path / "a", "A", (1920, 1080), RED)
    second = _probe(tmp_path / "b", "B", (1920, 1080), GREEN)
    log = tmp_path / "engine.log"
    process = None
    try:
        sway.run(["swaymsg", f"output {output} transform 90"], sway.extra)
        process = _engine(sway, engine, log, first, second)
        # 1080x1920 split in two 1080x960 rows, the first on top
        _wait_for_pixel(sway, tmp_path, process, 540, 940, RED, log)
        _wait_for_pixel(sway, tmp_path, process, 540, 980, GREEN, log)
    finally:
        _stop(process)
        sway.run(["swaymsg", f"output {output} transform normal"], sway.extra)


def test_cursor_and_clicks_belong_to_the_slot_under_the_pointer(sway, engine, tmp_path):
    first = _probe(tmp_path / "a", "A", (1920, 1080), RED)
    second = _probe(tmp_path / "b", "B", (1920, 1080), GREEN)
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, first, second)
    pointer = sway.virtual_pointer()
    width, height = sway.size

    def send(line):
        pointer.stdin.write(line + "\n")
        pointer.stdin.flush()
        pointer.stdout.readline()

    try:
        _wait_for_pixel(sway, tmp_path, process, 100, 100, RED, log)
        # equal 960 px slots, pointer logged in slot pixels, y from the top
        for x, y, tag, expected in ((300, 200, "A", (300, 200)), (1500, 700, "B", (540, 700))):
            seen = None
            for _ in range(10):
                for dx in (1, 0):
                    send(f"abs {x + dx} {y} {width} {height}")
                    time.sleep(0.3)
                lines = re.findall(rf"LWE-POS-{tag} [-\d.]+ [-\d.]+", log.read_text(errors="replace"))
                seen = lines[-1] if lines else None
                if seen == f"LWE-POS-{tag} {expected[0]:.1f} {expected[1]:.1f}":
                    break
            assert seen == f"LWE-POS-{tag} {expected[0]:.1f} {expected[1]:.1f}"

        send(f"abs 1500 700 {width} {height}")
        time.sleep(0.5)
        send("button 272 1")
        time.sleep(0.5)
        send("button 272 0")
        time.sleep(1)
        text = log.read_text(errors="replace")
        assert "LWE-DOWN-B" in text
        assert "LWE-DOWN-A" not in text
    finally:
        pointer.stdin.write("quit\n")
        pointer.stdin.flush()
        pointer.wait(10)
        _stop(process)


def test_hotswap_reaches_only_the_named_slot(sway, engine, tmp_path):
    output = sway.output_names()[0]
    first = _probe(tmp_path / "a", "A", (1920, 1080), RED)
    second = _probe(tmp_path / "b", "B", (1920, 1080), GREEN)
    third = _probe(tmp_path / "c", "C", (1920, 1080), BLUE)
    control = tmp_path / "control"
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, first, second, control=control)
    try:
        _wait_for_pixel(sway, tmp_path, process, 1500, 540, GREEN, log)
        control.write_text(f"screen={output}#2\npath={third}\n")
        os.kill(process.pid, signal.SIGUSR1)
        _wait_for_pixel(sway, tmp_path, process, 1500, 540, BLUE, log)
        _wait_for_pixel(sway, tmp_path, process, 400, 540, RED, log)
    finally:
        _stop(process)
