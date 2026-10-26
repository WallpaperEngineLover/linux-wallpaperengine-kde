"""Video wallpapers on virtual screens: the same video on several outputs, different videos on different outputs, and
always a third output that shows a copy of one of the others. A video that several outputs show is decoded once (the
engine logs which outputs share a decoder), whatever the outputs show has to be the right video."""
import json
import shutil
import subprocess
import time

import pytest
from PIL import Image

RED, BLUE = (255, 0, 0), (0, 0, 255)
SIZE = (1280, 720)


@pytest.fixture
def sway(desktop_factory):
    return desktop_factory("sway-dual")


def _swaymsg(sway, *command):
    result = sway.run(["swaymsg", *command], sway.extra)
    assert result.returncode == 0, result.stderr or result.stdout
    return result.stdout


@pytest.fixture
def three_outputs(sway):
    """HEADLESS-1..3 side by side, the third one made here and removed again"""
    names = json.loads(_swaymsg(sway, "-t", "get_outputs", "-r"))
    created = len(names) < 3

    if created:
        _swaymsg(sway, "create_output")

    outputs = [output["name"] for output in json.loads(_swaymsg(sway, "-t", "get_outputs", "-r"))]
    assert len(outputs) >= 3, outputs
    outputs = sorted(outputs)[:3]

    for index, name in enumerate(outputs):
        _swaymsg(sway, f"output {name} enable mode {SIZE[0]}x{SIZE[1]} pos {index * SIZE[0]} 0 scale 1")

    yield outputs

    if created:
        _swaymsg(sway, f"output {outputs[2]} unplug")


def _video(directory, color):
    """a project of one solid colour video that loops"""
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is needed to make the test videos")

    directory.mkdir(parents=True, exist_ok=True)
    made = subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i",
         f"color=c=0x{color[0]:02x}{color[1]:02x}{color[2]:02x}:s=320x180:r=30:d=3", "-pix_fmt", "yuv420p",
         "-c:v", "libx264", str(directory / "video.mp4")], capture_output=True)
    if made.returncode != 0:
        pytest.skip("ffmpeg cannot encode H.264 here: " + made.stderr.decode(errors="replace")[:200])
    (directory / "project.json").write_text(json.dumps({"file": "video.mp4", "type": "video", "title": "solid"}))
    return directory


def _engine(sway, engine, log, outputs, wallpapers):
    argv = [engine["binary"], "--assets-dir", engine["assets"], "--silent", "--fps", "30"]
    for output, wallpaper in zip(outputs, wallpapers):
        argv += ["--screen-root", output, "--bg", str(wallpaper)]
    return sway.client(argv, log=log)


def _colors(sway, tmp_path, outputs):
    path = tmp_path / "shot.png"
    sway.shot(path)
    image = Image.open(path).convert("RGB")
    rects = {output["name"]: output["rect"] for output in json.loads(_swaymsg(sway, "-t", "get_outputs", "-r"))
     if output.get("active", True)}
    left = min(rect["x"] for rect in rects.values())
    top = min(rect["y"] for rect in rects.values())
    return [image.getpixel((rects[name]["x"] - left + SIZE[0] // 2, rects[name]["y"] - top + SIZE[1] // 2))
    for name in outputs]


def _near(color, expected, tolerance=48):
    return all(abs(a - b) <= tolerance for a, b in zip(color, expected))


def _wait_for(sway, tmp_path, process, outputs, expected):
    seen = []

    for _ in range(30):
        assert process.poll() is None, f"the engine exited with {process.returncode}"
        seen = _colors(sway, tmp_path, outputs)

        if all(_near(color, want) for color, want in zip(seen, expected)):
            return
        time.sleep(1)

    pytest.fail(f"the outputs show {seen}, expected {expected}")


def _shared(log):
    return [line for line in log.read_text(errors="replace").splitlines() if "shares the decoder" in line]


def test_same_video_on_two_outputs_and_a_copy_on_the_third(sway, engine, tmp_path, three_outputs):
    red = _video(tmp_path / "red", RED)
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, three_outputs, [red, red, red])

    try:
        _wait_for(sway, tmp_path, process, three_outputs, [RED, RED, RED])
        assert len(_shared(log)) == 2, log.read_text(errors="replace")
        time.sleep(3)
        _wait_for(sway, tmp_path, process, three_outputs, [RED, RED, RED])
    finally:
        process.terminate()
        process.wait(10)


def test_different_videos_and_a_copy_of_the_second_on_the_third(sway, engine, tmp_path, three_outputs):
    red = _video(tmp_path / "red", RED)
    blue = _video(tmp_path / "blue", BLUE)
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, three_outputs, [red, blue, blue])

    try:
        _wait_for(sway, tmp_path, process, three_outputs, [RED, BLUE, BLUE])
        assert len(_shared(log)) == 1, log.read_text(errors="replace")
    finally:
        process.terminate()
        process.wait(10)


def test_different_videos_and_a_copy_of_the_first_on_the_third(sway, engine, tmp_path, three_outputs):
    red = _video(tmp_path / "red", RED)
    blue = _video(tmp_path / "blue", BLUE)
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, three_outputs, [red, blue, red])

    try:
        _wait_for(sway, tmp_path, process, three_outputs, [RED, BLUE, RED])
        assert len(_shared(log)) == 1, log.read_text(errors="replace")
    finally:
        process.terminate()
        process.wait(10)


def test_a_shared_video_survives_its_first_output_going_away(sway, engine, tmp_path, three_outputs):
    red = _video(tmp_path / "red", RED)
    log = tmp_path / "engine.log"
    process = _engine(sway, engine, log, three_outputs, [red, red, red])

    try:
        _wait_for(sway, tmp_path, process, three_outputs, [RED, RED, RED])
        _swaymsg(sway, f"output {three_outputs[0]} disable")
        time.sleep(4)
        assert process.poll() is None, f"the engine exited with {process.returncode}"
        _wait_for(sway, tmp_path, process, three_outputs[1:], [RED, RED])
    finally:
        process.terminate()
        process.wait(10)
