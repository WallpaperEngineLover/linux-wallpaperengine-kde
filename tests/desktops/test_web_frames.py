import os
import signal
import time
from pathlib import Path

import pytest
from PIL import Image

from conftest import Unsupported

PAGE = Path(__file__).parent / "fixtures/webframes"
# the same page with red and blue swapped
SWAPPED = Path(__file__).parent / "fixtures/webframes_swapped"

# the engine's web host renders on the session's Wayland display and hands its GPU buffers over (--web-frames dmabuf)
SESSIONS = ["sway", "labwc", "wayfire", "niri", "hyprland", "kde", "gnome"]

RED, GREEN, BLUE = (255, 0, 0), (0, 255, 0), (0, 0, 255)
BROWSER = "--ignore-gpu-blocklist --disable-dev-shm-usage"


def _env(engine, switches=BROWSER):
    # a container has no setuid chrome-sandbox, and Chromium's close() wrapper needs libcef.so ahead of libc
    return {
        "LWE_CEF_NO_SANDBOX": "1",
        "LWE_WEB_HOST_PRELOAD": str(Path(engine["binary"]).resolve().parent / "libcef.so"),
        "LWE_WEB_STATS": "1",
        "LWE_CEF_SWITCHES": switches,
    }


def _page_drawn(path, swapped=False):
    """the fixture's three coloured corners, as the page paints them on a screen of any size"""
    image = Image.open(path).convert("RGB")
    width, height = image.size
    found = (
        image.getpixel((width // 10, height // 10)),
        image.getpixel((width * 9 // 10, height // 10)),
        image.getpixel((width // 10, height * 9 // 10)),
    )
    colours = (BLUE, GREEN, RED) if swapped else (RED, GREEN, BLUE)
    return all(max(abs(a - b) for a, b in zip(pixel, expected)) <= 3 for pixel, expected in zip(found, colours))


def _run(desktop, engine, tmp_path, frames, switches=BROWSER, until=None, timeout=60, extra=()):
    log = tmp_path / "engine.log"
    process = desktop.client(
        [engine["binary"], "--assets-dir", engine["assets"], "--screen-root", desktop.output_names()[0], "--silent",
         "--fps", "30", "--web-frames", frames, *extra, str(PAGE)],
        log=log, env=_env(engine, switches),
    )
    shot = tmp_path / "shot.png"
    deadline = time.monotonic() + timeout
    drawn = False
    while time.monotonic() < deadline:
        time.sleep(2)
        assert process.poll() is None, f"the engine exited with {process.returncode}:\n{log.read_text()[-1500:]}"
        try:
            desktop.shot(shot)
        except Unsupported as error:
            pytest.skip(str(error))
        drawn = _page_drawn(shot)
        if drawn and (until is None or until in log.read_text(errors="replace")):
            break
    return process, log, shot, drawn


@pytest.mark.parametrize("desktop", SESSIONS, indirect=True)
def test_dmabuf_frames_draw_the_page(desktop, engine, tmp_path):
    process, log, shot, drawn = _run(desktop, engine, tmp_path, "dmabuf", until="frames arrive as dmabufs")
    try:
        text = log.read_text(errors="replace")
        assert "frames arrive as dmabufs" in text, f"the host sent no GPU buffer on {desktop.name}:\n{text[-1500:]}"
        assert "restarting the web host" not in text
        assert drawn, "the page's colours are not on the screen"
    finally:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", ["sway"], indirect=True)
def test_shared_memory_frames_still_draw_the_page(desktop, engine, tmp_path):
    process, log, shot, drawn = _run(desktop, engine, tmp_path, "shm")
    try:
        assert "frames arrive as dmabufs" not in log.read_text(errors="replace")
        assert drawn
    finally:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", ["sway"], indirect=True)
def test_a_browser_without_gpu_buffers_falls_back_to_shared_memory(desktop, engine, tmp_path):
    """headless Chromium cannot allocate shareable buffers: the host restarts with shared memory frames"""
    process, log, shot, drawn = _run(
        desktop, engine, tmp_path, "dmabuf", switches=BROWSER + " --ozone-platform=headless --use-angle=gl-egl",
        until="restarting the web host", timeout=90,
    )
    try:
        assert "restarting the web host with shared memory frames" in log.read_text(errors="replace")
        assert drawn, "the page did not show after the fallback"
    finally:
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", ["sway"], indirect=True)
def test_dmabuf_frames_follow_an_output_resize(desktop, engine, tmp_path):
    process, log, shot, drawn = _run(desktop, engine, tmp_path, "dmabuf", until="frames arrive as dmabufs")
    output = desktop.output_names()[0]
    try:
        assert drawn
        desktop.run(["wlr-randr", "--output", output, "--custom-mode", "1280x720"])
        deadline = time.monotonic() + 30
        size = None
        while time.monotonic() < deadline:
            time.sleep(2)
            desktop.shot(shot)
            size = Image.open(shot).size
            if size == (1280, 720) and _page_drawn(shot):
                break
        assert size == (1280, 720), f"the output is {size}"
        assert _page_drawn(shot), "the page is not drawn at the new size"
        assert "restarting the web host" not in log.read_text(errors="replace")
    finally:
        desktop.run(["wlr-randr", "--output", output, "--custom-mode", "1920x1080"])
        process.terminate()
        process.wait(10)


@pytest.mark.parametrize("desktop", ["sway"], indirect=True)
def test_dmabuf_frames_survive_a_hotswap_between_web_wallpapers(desktop, engine, tmp_path):
    control = tmp_path / "control"
    process, log, shot, drawn = _run(
        desktop, engine, tmp_path, "dmabuf", until="frames arrive as dmabufs", extra=("--control-file", str(control))
    )
    try:
        assert drawn
        control.write_text(f"path={SWAPPED}\n")
        os.kill(process.pid, signal.SIGUSR1)
        deadline = time.monotonic() + 40
        swapped = False
        while time.monotonic() < deadline and not swapped:
            time.sleep(2)
            desktop.shot(shot)
            swapped = _page_drawn(shot, swapped=True)
        assert swapped, "the second web wallpaper never replaced the first"
        text = log.read_text(errors="replace")
        assert text.count("frames arrive as dmabufs") == 2, "the new host did not send GPU buffers"
        assert "restarting the web host" not in text
    finally:
        process.terminate()
        process.wait(10)
