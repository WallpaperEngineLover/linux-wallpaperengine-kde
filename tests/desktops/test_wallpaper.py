import time

import pytest

from conftest import KINDS, Unsupported, changed_fraction

# what is known not to work yet, each reason is a thing to fix: the test starts passing and strict xfail says so
KNOWN_GAPS = {}

# what the lab cannot show, not the engine
LAB_LIMITS = {
    "river": "river 0.4 closes every layer surface unless a window manager client runs, and none is packaged",
}


def _param(name):
    if name in LAB_LIMITS:
        return pytest.param(name, marks=pytest.mark.skip(reason=LAB_LIMITS[name]))
    if name in KNOWN_GAPS:
        return pytest.param(name, marks=pytest.mark.xfail(reason=KNOWN_GAPS[name], strict=True))
    return name


WALLPAPER_SESSIONS = [_param(name) for name in KINDS]


@pytest.mark.parametrize("desktop", WALLPAPER_SESSIONS, indirect=True)
def test_engine_draws_the_wallpaper(desktop, engine, tmp_path):
    before = tmp_path / "before.png"
    try:
        desktop.shot(before)
    except Unsupported as error:
        pytest.skip(str(error))

    outputs = desktop.output_names()
    assert outputs, f"{desktop.name} lists no output"
    process = desktop.client([engine["binary"], "--assets-dir", engine["assets"], "--screen-root", outputs[0],
                              "--silent", "--fps", "30", engine["wallpaper"]], log=tmp_path / "engine.log")
    try:
        deadline = time.monotonic() + 45
        fraction = 0.0
        while time.monotonic() < deadline and fraction < 0.03:
            time.sleep(3)
            assert process.poll() is None, f"the engine exited with {process.returncode}:\n{(tmp_path / 'engine.log').read_text()[-1500:]}"
            after = tmp_path / "after.png"
            desktop.shot(after)
            fraction = changed_fraction(before, after)
        assert fraction >= 0.03, f"{fraction:.0%} of {desktop.name}'s screen changed after the engine started"
    finally:
        process.terminate()
        process.wait(10)
