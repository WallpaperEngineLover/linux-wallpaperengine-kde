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


def test_script_ids_stay_unique_after_an_unload(kde, tmp_path):
    """KWin 6.4 numbers scripts by how many are loaded, after an unload the next one got the id of a live script
    (KWinScript works around it); this tells whether the KWin of this lab still does that"""
    scripts = []
    for name in ("a", "b", "c"):
        path = tmp_path / f"{name}.js"
        path.write_text("print('lwe-id-probe');\n")
        scripts.append(path)

    def call(method, *args):
        result = kde.run(["gdbus", "call", "--session", "--dest", "org.kde.KWin", "--object-path", "/Scripting",
                          "--method", f"org.kde.kwin.Scripting.{method}", *args])
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()

    def load(path, name):
        return int(call("loadScript", str(path), name).strip("(), \n"))

    first = load(scripts[0], "lwe-id-a")
    second = load(scripts[1], "lwe-id-b")
    call("unloadScript", "lwe-id-a")
    third = load(scripts[2], "lwe-id-c")
    for name in ("lwe-id-b", "lwe-id-c"):
        call("unloadScript", name)
    if third == second:
        pytest.xfail(f"this KWin hands the id {second} of a live script to the next one (ids {first}, {second}, {third})")


def _cursor_scripts(desktop):
    return sorted(desktop.run_dir.glob("lwe-cursor-input-*.js"))


def _watch_cursor_calls(desktop, name):
    log = desktop.dir / f"{name}.log"
    log.unlink(missing_ok=True)
    monitor = desktop.client(["dbus-monitor", "--session", "type='method_call',member='OnCursorMoved'"], log=log)
    time.sleep(1)
    return monitor, log


def _calls(log):
    """(x, y) of every OnCursorMoved call dbus-monitor saw"""
    lines = Path(log).read_text(errors="replace").splitlines()
    calls = []
    for index, line in enumerate(lines):
        if "member=OnCursorMoved" in line:
            numbers = [int(part.split()[-1]) for part in lines[index + 1:index + 3] if part.strip().startswith("int32")]
            if len(numbers) == 2:
                calls.append(tuple(numbers))
    return calls


def _wiggle(desktop, steps=12):
    for step in range(steps):
        desktop.pointer("move", 300 + step * 20, 300 + (step % 2) * 40)
        time.sleep(0.15)


def test_cursor_script_follows_the_pause(kde, engine, tmp_path):
    """a long pause unloads the engine's cursor script so KWin sends nothing, resuming loads it again (off the render
    thread) and the pointer reaches the engine once more"""
    log = tmp_path / "engine.log"
    process = _engine(kde, engine, log, kde.output_names()[0])
    probe = None
    monitor = None
    try:
        time.sleep(8)
        assert len(_cursor_scripts(kde)) == 1, "the cursor script is not loaded on KDE"
        monitor, calls = _watch_cursor_calls(kde, "before")
        _wiggle(kde)
        assert _calls(calls), "the pointer does not reach the engine"
        terminate([monitor.pid])

        probe = _window(kde, "fullscreen")
        time.sleep(8)
        assert not _renders(log, 3), "still rendering under a fullscreen window"
        assert not _cursor_scripts(kde), "the cursor script is still loaded after a long pause"
        monitor, calls = _watch_cursor_calls(kde, "paused")
        _wiggle(kde)
        assert not _calls(calls), "KWin still sends cursor calls to a paused engine"
        terminate([monitor.pid])

        terminate([probe.pid])
        probe = None
        for _ in range(20):
            if _cursor_scripts(kde):
                break
            time.sleep(1)
        assert len(_cursor_scripts(kde)) == 1, "the cursor script did not come back after the pause"
        monitor, calls = _watch_cursor_calls(kde, "resumed")
        _wiggle(kde)
        assert _calls(calls), "the pointer does not reach the engine after the pause"
        assert "could not load its KWin script again" not in log.read_text(errors="replace")
    finally:
        for stray in (monitor, probe):
            if stray is not None:
                terminate([stray.pid])
        process.terminate()
        process.wait(10)


def test_cursor_script_reports_where_the_pointer_stopped(kde, engine, tmp_path):
    """a fast flick ends on a call within a few ms of the last one, the final position has to be reported anyway"""
    log = tmp_path / "engine.log"
    process = _engine(kde, engine, log, kde.output_names()[0])
    monitor = None
    try:
        time.sleep(8)
        monitor, calls = _watch_cursor_calls(kde, "flick")
        for run in range(5):
            target = (900 + run * 37, 500 + run * 11)
            kde.pointer("move", 400, 300, "move", 500, 340, "move", 600, 380, "move", 700, 420, "move", *target)
            time.sleep(0.5)
            seen = _calls(calls)
            assert seen and seen[-1] == target, f"the engine ended at {seen[-1] if seen else None}, not at {target}"
    finally:
        if monitor is not None:
            terminate([monitor.pid])
        process.terminate()
        process.wait(10)


def test_media_signals_while_paused(kde, engine, tmp_path):
    """MPRIS signals that pile up behind a pause are read and dropped, the engine keeps its media connection, renders
    again afterwards and does not crash"""
    log = tmp_path / "engine.log"
    process = _engine(kde, engine, log, kde.output_names()[0])
    probe = None
    try:
        time.sleep(8)
        probe = _window(kde, "fullscreen")
        time.sleep(5)
        assert not _renders(log, 3), "still rendering under a fullscreen window"
        flood = kde.run(["python3", str(REPO / "tools/desktops/mpris_flood.py"), "3000", "4096"], timeout=120)
        assert flood.returncode == 0, flood.stderr
        time.sleep(3)
        assert process.poll() is None, "the engine died while paused"
        terminate([probe.pid])
        probe = None
        time.sleep(6)
        assert _renders(log), "rendering did not resume after the flood"
        text = log.read_text(errors="replace")
        assert "the session bus dropped the connection" not in text
        assert "Media: reconnected" not in text
    finally:
        if probe is not None:
            terminate([probe.pid])
        process.terminate()
        process.wait(10)


def test_dpms_off_pauses(kde, engine, tmp_path):
    log = tmp_path / "engine.log"
    process = _engine(kde, engine, log, kde.output_names()[0])
    try:
        time.sleep(8)
        assert _renders(log), "the engine does not render on KDE"
        result = kde.run(["kscreen-doctor", "--dpms", "off"])
        assert result.returncode == 0, result.stderr
        time.sleep(3)
        assert not _renders(log), "still rendering with the output in DPMS off"
        assert "Paused: display off" in log.read_text(errors="replace")
        result = kde.run(["kscreen-doctor", "--dpms", "on"])
        assert result.returncode == 0, result.stderr
        time.sleep(3)
        assert _renders(log), "rendering did not resume after DPMS on"
    finally:
        kde.run(["kscreen-doctor", "--dpms", "on"])
        process.terminate()
        process.wait(10)
