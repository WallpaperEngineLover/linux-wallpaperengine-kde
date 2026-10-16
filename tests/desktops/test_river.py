"""river 0.4 closes every layer surface while no window manager runs: the engine has to wait for one, not crash or quit."""
import time

import pytest

from conftest import changed_fraction
from test_kde import _engine


def _wait_log(log, text, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if text in log.read_text(errors="replace"):
            return True
        time.sleep(0.5)
    return False


@pytest.mark.parametrize("desktop", ["river"], indirect=True)
def test_waits_for_a_window_manager(desktop, engine, tmp_path):
    desktop.stop_window_manager()
    before = tmp_path / "before.png"
    desktop.shot(before)
    log = tmp_path / "engine.log"
    process = _engine(desktop, engine, log, desktop.output_names()[0])
    try:
        assert _wait_log(log, "The compositor closed the wallpaper surface", 30), log.read_text()[-1500:]
        assert "No output shows a wallpaper" in log.read_text()
        time.sleep(5)
        assert process.poll() is None, f"the engine exited with {process.returncode}:\n{log.read_text()[-1500:]}"

        desktop.start_window_manager()
        assert _wait_log(log, "the compositor took the wallpaper surface again", 45), log.read_text()[-1500:]
        after = tmp_path / "after.png"
        time.sleep(3)
        desktop.shot(after)
        fraction = changed_fraction(before, after)
        assert fraction >= 0.03, f"{fraction:.0%} of the screen changed once the window manager runs"
        assert process.poll() is None
    finally:
        process.terminate()
        process.wait(10)
        if desktop.window_manager is None:
            desktop.start_window_manager()
