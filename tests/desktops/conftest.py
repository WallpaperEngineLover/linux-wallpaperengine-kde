"""Integration tests that run the engine inside real desktop sessions, each one headless and private to the test.

Needs the engine binary (LWE_BINARY, default build/output/linux-wallpaperengine), Wallpaper Engine's assets
(LWE_ASSETS_DIR) and a scene wallpaper (LWE_TEST_WALLPAPER, a workshop item folder). A session whose programs are not
installed here is skipped. Tests only ever talk to the sessions they start: never to the desktop of the machine they
run on.
"""
import os
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

from desktops.session import KINDS, Session, Unsupported  # noqa: E402

STEAM_ROOTS = (
    Path("/workspace/SteamLibrary"),
    Path.home() / ".local/share/Steam",
    Path.home() / ".steam/steam",
)


def _first_existing(paths):
    return next((path for path in paths if path.exists()), None)


@pytest.fixture(scope="session")
def engine():
    binary = Path(os.environ.get("LWE_BINARY", REPO / "build/output/linux-wallpaperengine"))
    assets = os.environ.get("LWE_ASSETS_DIR") or _first_existing(
        [root / "steamapps/common/wallpaper_engine/assets" for root in STEAM_ROOTS]
    )
    wallpaper = os.environ.get("LWE_TEST_WALLPAPER") or _first_existing(
        [root / "steamapps/workshop/content/431960/1846362380" for root in STEAM_ROOTS]
    )
    missing = [what for what, value in (("the engine binary", binary if binary.exists() else None),
                                        ("the assets", assets), ("a test wallpaper", wallpaper)) if not value]
    if missing:
        pytest.skip(f"missing {', '.join(missing)}, see tests/desktops/conftest.py")
    return {"binary": str(binary), "assets": str(assets), "wallpaper": str(wallpaper)}


@pytest.fixture(scope="session")
def desktop_factory(tmp_path_factory):
    """one session at a time, shared by the tests of that session: a full desktop is several hundred MB"""
    started = {}
    available = Session.available()

    def get(name):
        if available.get(name) is None:
            pytest.skip(f"{name} is not installed here and has no rootfs, run: python3 tools/desktop_session.py setup")
        for other in [key for key in started if key != name]:
            started.pop(other).stop()
        if name not in started:
            session = Session.of(name)
            try:
                session.start()
            except Exception as error:
                pytest.fail(f"{name} did not start: {error}")
            started[name] = session
        return started[name]

    yield get
    for session in started.values():
        session.stop()


@pytest.fixture
def desktop(request, desktop_factory):
    return desktop_factory(request.param)


def changed_fraction(before, after, threshold=24):
    """share of the pixels that differ by more than the threshold in any channel"""
    import numpy
    from PIL import Image

    a = numpy.asarray(Image.open(before).convert("RGB"), dtype=numpy.int16)
    b = numpy.asarray(Image.open(after).convert("RGB"), dtype=numpy.int16)
    if a.shape != b.shape:
        return 1.0
    return float((numpy.abs(a - b).max(axis=2) > threshold).mean())
