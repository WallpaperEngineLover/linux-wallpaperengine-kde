"""Headless sessions of every desktop environment and compositor, private to the caller.

Each Session owns a directory under BASE: a runtime dir with the Wayland socket, the session bus and the compositor's
own sockets, a home, the logs and state.json. Programs of the session run in its userland (see runner.py), the
programs under test (the engine, we_manager) run on the caller's side with client_env().
"""
import glob
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from .runner import BASE, REPO, Userland, kill_with_runtime_dir, terminate, wait_for

KDE_VARIABLES = ("DISPLAY", "DESKTOP_SESSION", "XDG_SESSION_DESKTOP", "KDE_FULL_SESSION")


class Unsupported(Exception):
    pass


class Session:
    name = ""
    # short, Hyprland's IPC socket path has to fit in 108 bytes
    short = ""
    needs = ()
    userlands = ("native", "fedora", "arch")
    # downloaded by setup
    requires = ()
    sysbus = False
    desktop = ""

    def __init__(self, size=(1920, 1080), outputs=1):
        self.size = tuple(size)
        self.outputs = outputs
        self.dir = BASE / self.short
        self.run_dir = self.dir / "run"
        self.home = self.dir / "home"
        self.userland = None
        self.pids = []
        self.session_env = {}
        # more variables of the session (a compositor's IPC socket), and facts about it that are not variables
        self.extra = {}
        self.info = {}

    # ----- lookup

    @classmethod
    def of(cls, name, **options):
        try:
            return KINDS[name](**options)
        except KeyError:
            raise SystemExit(f"unknown session {name}, one of: {', '.join(KINDS)}")

    @classmethod
    def load(cls, name):
        session = cls.of(name)
        state = json.loads((session.dir / "state.json").read_text())
        session.userland = Userland(state["userland"])
        session.pids = state["pids"]
        session.session_env = state["session_env"]
        session.extra = state["extra"]
        session.info = state["info"]
        session.size = tuple(state["size"])
        session.outputs = state["outputs"]
        return session

    @classmethod
    def available(cls):
        """userland name or None per session name, whether it can be started here"""
        result = {}
        for name, kind in KINDS.items():
            try:
                result[name] = Userland.pick(*kind.needs, prefer=kind.userlands).name
                if not all(Path(path).exists() for path in kind.requires):
                    result[name] = None
            except RuntimeError:
                result[name] = None
        return result

    def is_running(self):
        state = self.dir / "state.json"
        if not state.exists():
            return False
        pids = json.loads(state.read_text())["pids"]
        return bool(pids) and os.path.exists(f"/proc/{pids[0]}")

    # ----- lifecycle

    def start(self):
        if self.is_running():
            return self
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)
        self.run_dir.mkdir(parents=True, mode=0o700)
        self.home.mkdir(parents=True)
        self.userland = Userland.pick(*self.needs, prefer=self.userlands)
        try:
            self._start_bus()
            if self.sysbus:
                self._start_system_bus()
            self._launch()
        except BaseException:
            self.stop()
            raise
        self._save()
        return self

    def stop(self):
        if self.dir.exists() and (self.dir / "state.json").exists():
            self.pids = json.loads((self.dir / "state.json").read_text())["pids"]
        terminate(list(reversed(self.pids)))
        if self.dir.exists():
            kill_with_runtime_dir(self.run_dir)
        self.pids = []
        state = self.dir / "state.json"
        if state.exists():
            state.unlink()

    def _save(self):
        (self.dir / "state.json").write_text(json.dumps({
            "userland": self.userland.name, "pids": self.pids, "session_env": self.session_env, "extra": self.extra, "info": self.info,
            "size": self.size, "outputs": self.outputs,
        }))
        (self.dir / "env").write_text(self.shell_exports())

    # ----- environments

    def env(self):
        """for the programs that run in the session's userland"""
        env = {
            "XDG_RUNTIME_DIR": str(self.run_dir),
            "DBUS_SESSION_BUS_ADDRESS": f"unix:path={self.run_dir}/bus",
            "HOME": str(self.home),
            "XDG_CONFIG_HOME": str(self.home / ".config"),
            "XDG_DATA_HOME": str(self.home / ".local/share"),
            "XDG_CACHE_HOME": str(self.home / ".cache"),
            "XDG_STATE_HOME": str(self.home / ".local/state"),
        }
        address = self.run_dir / "system-bus-address"
        if address.exists():
            env["DBUS_SYSTEM_BUS_ADDRESS"] = address.read_text().strip()
        env.update(self.session_env)
        env.update(self.extra)
        return env

    def client_env(self):
        """for programs under test, started on the caller's side: only what finds this session"""
        env = {
            key: value for key, value in self.env().items()
            if key in ("XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
                       "WAYLAND_DISPLAY", "DISPLAY", "KDE_FULL_SESSION", "KDE_SESSION_VERSION")
        }
        env.update(self.extra)
        # never the desktop's audio server, a sandbox sees it
        env["PULSE_SERVER"] = "unix:/nonexistent"
        env["PIPEWIRE_REMOTE"] = "/nonexistent"
        return env

    def shell_exports(self):
        env = self.client_env()
        lines = [f"export {key}={value}" for key, value in env.items()]
        unset = [key for key in KDE_VARIABLES if key not in env]
        return "\n".join(lines + [f"unset {' '.join(unset)}"]) + "\n"

    # ----- running programs

    def spawn(self, name, argv, env=None, userland=None):
        """a long running program of the session, logged to <dir>/<name>.log"""
        userland = userland or self.userland
        process = userland.spawn(argv, {**self.env(), **(env or {})}, self.dir / f"{name}.log")
        self.pids.append(process.pid)
        return process

    def run(self, argv, env=None, timeout=60):
        """a short program in the session's userland, its CompletedProcess"""
        return self.userland.run(argv, {**self.env(), **(env or {})}, timeout=timeout)

    def client(self, argv, log=None, env=None):
        """a program under test on the caller's side, in the session's environment"""
        base = {"PATH": os.environ.get("PATH", ""), "HOME": os.environ.get("HOME", "/tmp"), "LANG": "C.UTF-8"}
        out = open(log or self.dir / "client.log", "ab")
        return subprocess.Popen(
            argv, env={**base, **self.client_env(), **(env or {})}, stdout=out, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, start_new_session=True,
        )

    def shot(self, path):
        """screenshot to path, taken in the session's userland"""
        scratch = self.dir / "shot.png"
        scratch.unlink(missing_ok=True)
        self._shot(scratch)
        shutil.copyfile(scratch, path)

    def _shot(self, path):
        raise Unsupported(f"no screenshot method for {self.name} yet")

    def output_names(self):
        """the outputs a program can be told to use with --screen-root"""
        if "WAYLAND_DISPLAY" in self.session_env:
            # the socket is reachable from the caller's side too, and not every userland has wayland-info (Arch's)
            userland = Userland("native") if Userland("native").has("wayland-info") else self.userland
            out = userland.run(["wayland-info"], self.env()).stdout
            names = []
            for block in re.split(r"(?=^interface: )", out, flags=re.M):
                if block.startswith("interface: 'wl_output'"):
                    names += [name.strip() for name in re.findall(r"^\s+name: (.+)$", block, re.M)]
            return names
        return self._x_outputs()

    def _x_outputs(self):
        return []

    def windows(self):
        return []

    # ----- buses

    def _start_bus(self):
        self.spawn("dbus", ["dbus-daemon", "--session", "--nofork", "--nopidfile", f"--address=unix:path={self.run_dir}/bus"])
        wait_for("the session bus", (self.run_dir / "bus").exists, 15)

    def _start_system_bus(self):
        """stand-ins for the systemd services Cinnamon and GNOME need at startup"""
        self.spawn("sysbus", ["python3", str(REPO / "tools/desktops/fake_system.py"), str(self.run_dir / "system-bus-address")])
        wait_for("the fake system bus", lambda: (self.run_dir / "system-bus-address").exists(), 30)

    def _bus_name(self, name):
        reply = self.run(["dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus.NameHasOwner", f"string:{name}"])
        return "boolean true" in reply.stdout

    def _launch(self):
        raise NotImplementedError

    # ----- shared helpers

    def _socket(self, pattern):
        for path in sorted(glob.glob(str(self.run_dir / pattern))):
            if not path.endswith(".lock"):
                return path
        return None

    def wait_socket(self, pattern, timeout=40):
        return wait_for(f"{pattern} in {self.run_dir}", lambda: self._socket(pattern), timeout)


class Wlroots(Session):
    """sway, labwc, wayfire and river on wlroots' headless backend"""

    program = ""

    def _wlroots_env(self):
        env = {
            "WLR_BACKENDS": "headless", "WLR_LIBINPUT_NO_DEVICES": "1", "WLR_HEADLESS_OUTPUTS": str(self.outputs),
            # nothing here may reach the desktop's session or take its GPU
            "LIBSEAT_BACKEND": "seatd", "SEATD_SOCK": "/nonexistent",
        }
        for key in ("WLR_RENDER_DRM_DEVICE", "WLR_RENDERER"):
            if key in os.environ:
                env[key] = os.environ[key]
        return env

    def _config(self):
        return []

    def _launch(self):
        argv = [self.program, *self._config()]
        self.spawn(self.program, argv, self._wlroots_env())
        socket = self.wait_socket("wayland-[0-9]*")
        self.session_env.update({
            "WAYLAND_DISPLAY": os.path.basename(socket), "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": self.desktop,
        })
        self._ready()

    def _ready(self):
        # wlroots' headless outputs start at 1280x720
        width, height = self.size
        for index in range(1, self.outputs + 1):
            self.run(["wlr-randr", "--output", f"HEADLESS-{index}", "--custom-mode", f"{width}x{height}"],
                     {"WAYLAND_DISPLAY": self.session_env["WAYLAND_DISPLAY"]})

    def _shot(self, path):
        result = self.run(["grim", str(path)])
        if result.returncode != 0:
            raise RuntimeError(f"grim failed: {result.stderr.strip()}")


class Sway(Wlroots):
    name, short, program, desktop = "sway", "s", "sway", "sway"
    needs = ("sway", "swaymsg", "grim")

    def _config(self):
        width, height = self.size
        (self.dir / "sway.conf").write_text(
            f"output * resolution {width}x{height}\nxwayland disable\ndefault_border none\nswaybg_command -\n"
        )
        return ["-c", str(self.dir / "sway.conf")]

    def _ready(self):
        self.extra["SWAYSOCK"] = wait_for("sway's IPC socket", lambda: self._socket("sway-ipc.*.sock"), 30)


class Labwc(Wlroots):
    name, short, program, desktop = "labwc", "l", "labwc", "labwc"
    needs = ("labwc", "wlr-randr", "grim")

    def _config(self):
        config = self.dir / "config"
        config.mkdir()
        # the compositors that run as a window of this one fill its output
        (config / "rc.xml").write_text(
            '<?xml version="1.0"?>\n<labwc_config>\n  <windowRules>\n'
            + "".join(
                f'    <windowRule identifier="{app}"><action name="ToggleFullscreen"/></windowRule>\n'
                for app in ("Hyprland", "niri", "cosmic-comp")
            )
            + "  </windowRules>\n</labwc_config>\n"
        )
        return ["-C", str(config)]


class Wayfire(Wlroots):
    name, short, program, desktop = "wayfire", "w", "wayfire", "wayfire"
    needs = ("wayfire", "wlr-randr", "grim")


class River(Wlroots):
    name, short, program, desktop = "river", "r", "river", "river"
    needs = ("river", "wlr-randr", "grim")

    def _config(self):
        init = self.dir / "init"
        init.write_text("#!/bin/sh\n")
        init.chmod(0o755)
        return ["-c", str(init)]


class Nested(Session):
    """Hyprland, niri and COSMIC have no headless backend (they would take the GPU through DRM), they run as a window
    of a headless labwc"""

    program = ""

    def _launch(self):
        if Labwc().is_running():
            parent = Session.load("labwc")
        else:
            parent = Labwc(self.size, 1).start()
            parent.info["implicit"] = True
            parent._save()
        socket = f"{parent.run_dir}/{parent.session_env['WAYLAND_DISPLAY']}"
        argv, env = self._command()
        self.spawn(self.program, argv, {"WAYLAND_DISPLAY": socket, "LIBSEAT_BACKEND": "seatd", "SEATD_SOCK": "/nonexistent", **env})
        self.session_env.update({"XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": self.desktop})
        self._wait()

    def _command(self):
        return [self.program], {}

    def _wait(self):
        self.session_env["WAYLAND_DISPLAY"] = os.path.basename(self.wait_socket("wayland-[0-9]*"))

    def stop(self):
        super().stop()
        # the labwc they run in goes with the last of them, unless it was started on its own
        labwc = Labwc()
        if labwc.is_running() and Session.load("labwc").info.get("implicit") and not any(
            kind().is_running() for kind in (Hyprland, Niri, Cosmic)
        ):
            labwc.stop()

    def _shot(self, path):
        result = self.run(["grim", str(path)])
        if result.returncode != 0:
            raise RuntimeError(f"grim failed: {result.stderr.strip()}")


class Hyprland(Nested):
    name, short, program, desktop = "hyprland", "h", "Hyprland", "Hyprland"
    needs = ("Hyprland", "hyprctl", "grim")

    def _command(self):
        width, height = self.size
        lines = [f"monitor = HEADLESS-{i}, {width}x{height}@60, {width * (i - 1)}x0, 1" for i in range(1, self.outputs + 1)]
        (self.dir / "hyprland.conf").write_text("\n".join(lines + ["misc { disable_hyprland_logo = true }", "debug { disable_logs = false }"]) + "\n")
        return [self.program, "--config", str(self.dir / "hyprland.conf")], {}

    def _wait(self):
        super()._wait()
        signature = wait_for("Hyprland's IPC socket", lambda: self._socket("hypr/*/.socket.sock"), 30)
        self.extra["HYPRLAND_INSTANCE_SIGNATURE"] = os.path.basename(os.path.dirname(signature))
        for _ in range(self.outputs):
            self.run(["hyprctl", "output", "create", "headless"], self.extra)


class Niri(Nested):
    name, short, program, desktop = "niri", "n", "niri", "niri"
    needs = ("niri", "grim")

    def _command(self):
        (self.dir / "niri.kdl").write_text("")
        return [self.program, "--config", str(self.dir / "niri.kdl")], {}

    def _wait(self):
        super()._wait()
        self.extra["NIRI_SOCKET"] = wait_for("niri's IPC socket", lambda: self._socket("niri.*.sock"), 30)


class Cosmic(Nested):
    name, short, program, desktop = "cosmic", "c", "cosmic-comp", "COSMIC"
    needs = ("cosmic-comp", "grim")


class Kde(Session):
    """KWin's virtual backend, plasmashell paints nothing on it yet"""

    name, short, desktop = "kde", "k", "KDE"
    needs = ("kwin_wayland", "kscreen-doctor")
    sysbus = True

    def _launch(self):
        width, height = self.size
        env = {
            "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "KDE", "KDE_FULL_SESSION": "true", "KDE_SESSION_VERSION": "6",
            "QT_QPA_PLATFORM": "wayland", "KWIN_SCREENSHOT_NO_PERMISSION_CHECKS": "1",
            # for kwin_fake_input.c
            "KWIN_WAYLAND_NO_PERMISSION_CHECKS": "1",
        }
        self.session_env.update(env)
        self.spawn("kwin", ["kwin_wayland", "--virtual", "--width", str(width), "--height", str(height),
                            "--output-count", str(self.outputs), "--socket", "lwe-kde", "--no-lockscreen",
                            "--xwayland"], env)
        self.session_env["WAYLAND_DISPLAY"] = "lwe-kde"
        self.wait_socket("lwe-kde")
        wait_for("KWin on the session bus", lambda: self._bus_name("org.kde.KWin"), 40)

    def pointer(self, *commands):
        """KWin's pointer through fake input: "move", x, y (logical, global), "down"/"up", evdev button"""
        tool = self.dir / "kwin_fake_input"
        if not tool.exists():
            build = self.dir / "fake-input"
            build.mkdir(exist_ok=True)
            xml = "/usr/share/plasma-wayland-protocols/fake-input.xml"
            for mode, out in (("client-header", "fake-input-client-protocol.h"), ("private-code", "fake-input.c")):
                subprocess.run(["wayland-scanner", mode, xml, str(build / out)], check=True)
            subprocess.run(["cc", "-o", str(tool), str(REPO / "tools/desktops/kwin_fake_input.c"), str(build / "fake-input.c"),
                            f"-I{build}", "-lwayland-client", "-lm"], check=True)
        result = self.run([str(tool), *map(str, commands)])
        if result.returncode != 0:
            raise RuntimeError(f"kwin_fake_input: {result.stderr.strip()}")

    def _shot(self, path):
        result = self.run(["python3", str(REPO / "tools/desktops/kwin_shot.py"), str(path)])
        if result.returncode != 0:
            raise RuntimeError(f"kwin_shot failed: {result.stderr.strip()}")


class KdeDual(Kde):
    """KWin's virtual backend with two outputs, for what happens per output"""

    name, short = "kde-dual", "k2"

    def __init__(self, size=(1280, 720), outputs=2):
        super().__init__(size, outputs)


class Gnome(Session):
    """GNOME Shell as a headless Wayland display server"""

    name, short, desktop = "gnome", "g", "GNOME"
    needs = ("gnome-shell",)
    sysbus = True

    # lab extension enables unsafe mode for Screenshot/Eval; the build's copy has the helper in it
    EXTENSIONS = (REPO / "tools/desktops/gnome-lab-extension/lwe-lab@desktops.test",
                  Path(os.environ.get("LWE_BUILD_OUTPUT", REPO / "build/output"))
                  / "gnome-shell-extension/linux-wallpaperengine@wallpaperenginelover.github.io")

    # (uuid, folder to link, or None if installed in the userland)
    extra_extensions = ()
    # put on ~/Desktop, a trailing / makes a folder
    desktop_files = ()

    def _install_extensions(self):
        target = self.home / ".local/share/gnome-shell/extensions"
        target.mkdir(parents=True, exist_ok=True)
        for source in self.EXTENSIONS:
            (target / source.name).symlink_to(source)
        for uuid, folder in self.extra_extensions:
            if folder is not None:
                (target / uuid).symlink_to(folder)
        for name in self.desktop_files:
            path = self.home / "Desktop" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if name.endswith("/"):
                path.mkdir(exist_ok=True)
            else:
                path.write_text("lab\n")
        uuids = ", ".join([f"'{source.name}'" for source in self.EXTENSIONS] + [f"'{uuid}'" for uuid, _ in self.extra_extensions])
        for key, value in (("enabled-extensions", f"[{uuids}]"), ("disable-user-extensions", "false")):
            result = self.run(["gsettings", "set", "org.gnome.shell", key, value])
            if result.returncode != 0:
                raise RuntimeError(f"gsettings {key}: {result.stderr.strip()}")

    def _launch(self):
        width, height = self.size
        env = {"XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "GNOME", "XDG_SESSION_DESKTOP": "gnome",
               "GNOME_SHELL_SESSION_MODE": "user"}
        self.session_env.update(env)
        self._install_extensions()
        monitors = [arg for _ in range(self.outputs) for arg in ("--virtual-monitor", f"{width}x{height}")]
        # no Xwayland, mutter could pick the real desktop's display number
        self.spawn("shell", ["gnome-shell", "--headless", "--wayland", "--no-x11", *monitors, "--wayland-display",
                             "lwe-gnome"], env)
        self.session_env["WAYLAND_DISPLAY"] = "lwe-gnome"
        self.wait_socket("lwe-gnome")
        wait_for("the lab extension", self._unsafe_mode, 40)
        wait_for("the overview to close", lambda: self.eval("Main.overview.hide(); Main.overview.visible") == "false", 20)

    def _unsafe_mode(self):
        reply = self.eval("global.context.unsafe_mode")
        return reply == "true"

    def eval(self, script):
        """runs JS inside the Shell (unsafe mode only), its JSON result or None"""
        result = self.run(["gdbus", "call", "--session", "--dest", "org.gnome.Shell", "--object-path", "/org/gnome/Shell",
                           "--method", "org.gnome.Shell.Eval", script], timeout=20)
        found = re.match(r"\((true|false), '(.*)'\)$", result.stdout.strip(), re.S)
        if not found or found.group(1) != "true":
            return None
        return found.group(2)

    def _shot(self, path):
        result = self.run(["gdbus", "call", "--session", "--dest", "org.gnome.Shell", "--object-path",
                           "/org/gnome/Shell/Screenshot", "--method", "org.gnome.Shell.Screenshot.Screenshot",
                           "false", "false", str(path)], timeout=30)
        if "(true," not in result.stdout:
            raise RuntimeError(f"Shell screenshot failed: {result.stdout.strip()} {result.stderr.strip()}")


class Gnome46(Gnome):
    """GNOME Shell 46 from the Ubuntu 24.04 rootfs (setup gnome46): no Meta.Window.set_type, the helper path"""

    name, short, desktop = "gnome46", "g4", "GNOME"
    userlands = ("gnome46",)


class Gnome45(Gnome):
    """GNOME Shell 45 (Ubuntu 23.10): no desktop windows for Wayland clients, the wallpaper is a bottom layer window"""

    name, short, desktop = "gnome45", "g5", "GNOME"
    userlands = ("gnome45",)


class Gnome47(Gnome):
    """GNOME Shell 47 (Ubuntu 24.10), the helper path"""

    name, short, desktop = "gnome47", "g7", "GNOME"
    userlands = ("gnome47",)


class Gnome48(Gnome):
    """GNOME Shell 48 (Ubuntu 25.04), the helper path"""

    name, short, desktop = "gnome48", "g8", "GNOME"
    userlands = ("gnome48",)


class Gnome49(Gnome):
    """GNOME Shell 49 (Fedora 43), the first with Meta.Window.set_type"""

    name, short, desktop = "gnome49", "g9", "GNOME"
    userlands = ("gnome49",)


# DING for GNOME 50 comes from `setup ding`
DING_DIR = Path.home() / ".local/share/lwe-lab/ding50/ding@rastersoft.com"


class GnomeDing(Gnome):
    """GNOME 50 with Desktop Icons NG, whose desktop window shares the desktop layer with the wallpaper's"""

    name, short = "gnome-ding", "gd"
    extra_extensions = (("ding@rastersoft.com", DING_DIR),)
    desktop_files = ("notes.txt", "todo.txt", "Pictures/")
    requires = (DING_DIR,)


class Gnome46Ding(Gnome46):
    """GNOME 46 with Ubuntu's Desktop Icons NG package"""

    name, short = "gnome46-ding", "g4d"
    extra_extensions = (("ding@rastersoft.com", None),)
    desktop_files = GnomeDing.desktop_files


class Gnome45Ding(Gnome45):
    """GNOME 45 with Ubuntu's Desktop Icons NG, both windows in the bottom layer"""

    name, short = "gnome45-ding", "g5d"
    extra_extensions = (("ding@rastersoft.com", None),)
    desktop_files = GnomeDing.desktop_files


class X11(Session):
    """a desktop environment or window manager on an X server of its own"""

    server = "xvfb"
    components = ()
    command = ()
    env_extra = {}
    desktop_window = ""
    ready_timeout = 90
    # the panels and the desktop need a moment after their windows exist before they have painted
    settle = 0

    @property
    def display(self):
        return int(self.info["display"])

    def _launch(self):
        self.info["display"] = str(self._free_display())
        self._start_server()
        env = {"DISPLAY": f":{self.display}", "XDG_SESSION_TYPE": "x11", "XDG_CURRENT_DESKTOP": self.desktop or "X-Generic", **self.env_extra}
        self.session_env.update(env)
        if self.command:
            self.spawn("session", list(self.command), env)
        else:
            for index, argv in enumerate(self.components):
                self.spawn(f"component{index}", list(argv), env)
                if index == 0:
                    wait_for("the window manager", self._wm_name, 30)
        wait_for("the window manager", self._wm_name, self.ready_timeout)
        if self.desktop_window:
            wait_for("the desktop window", lambda: re.search(self.desktop_window, self._wmctrl("-lx")), self.ready_timeout)
        time.sleep(self.settle)

    def _free_display(self):
        taken = set()
        host = os.environ.get("DISPLAY", "")
        if host.startswith(":"):
            taken.add(int(host[1:].split(".")[0] or 0))
        unix = Path("/proc/net/unix").read_text()
        for number in range(100, 200):
            lock = f"/tmp/.X{number}-lock"
            rootfs_locks = [root / f"tmp/.X{number}-lock" for root in (Path(p) for p in self._rootfs_paths())]
            if (number in taken or Path(lock).exists() or f"/tmp/.X11-unix/X{number}" in unix
                    or any(path.exists() for path in rootfs_locks)):
                continue
            return number
        raise RuntimeError("no free X display")

    def _rootfs_paths(self):
        from .runner import ROOTFS
        return [path for path in ROOTFS.values() if path.is_dir()]

    def _start_server(self):
        display = f":{self.display}"
        width, height = self.size
        # the real server, /usr/bin/Xorg is a wrapper that only lets console users start it
        xorg = next((p for p in ("/usr/libexec/Xorg", "/usr/lib/xorg/Xorg") if self.userland.exists(p)), None) \
            if self.server == "xorg" else None
        if xorg:
            (self.dir / "xorg.conf").write_text(
                'Section "Device"\n    Identifier "dummy"\n    Driver "dummy"\n    VideoRam 256000\nEndSection\n'
                'Section "Monitor"\n    Identifier "mon"\n    HorizSync 5-1000\n    VertRefresh 5-200\n'
                f'    Modeline "{width}x{height}_60" 173.00 {width} {width + 128} {width + 328} {width + 656} {height} {height + 3} {height + 8} {height + 40} -hsync +vsync\n'
                'EndSection\nSection "Screen"\n    Identifier "screen0"\n    Device "dummy"\n    Monitor "mon"\n    DefaultDepth 24\n'
                f'    SubSection "Display"\n        Depth 24\n        Modes "{width}x{height}_60"\n        Virtual {width} {height}\n    EndSubSection\nEndSection\n'
                'Section "ServerFlags"\n    Option "DontVTSwitch" "true"\n    Option "AutoAddDevices" "false"\n'
                '    Option "AutoEnableDevices" "false"\n    Option "AutoAddGPU" "false"\nEndSection\n'
            )
            # a real refresh rate: muffin and mutter turn Xvfb's mode down
            self.spawn("xserver", [xorg, display, "-config", str(self.dir / "xorg.conf"), "-configdir", "/nonexistent", "-noreset",
                                   "-nolisten", "tcp", "-ac", "+extension", "GLX", "-logfile", str(self.dir / "Xorg.log")])
        else:
            self.spawn("xserver", ["Xvfb", display, "-screen", "0", f"{width}x{height}x24", "-ac", "+extension", "GLX", "+render", "-noreset"])
        wait_for("the X server", lambda: self._x(["xprop", "-root"]).returncode == 0, 30)

    def _x(self, argv):
        return self.run(argv, {"DISPLAY": f":{self.display}"})

    def _wmctrl(self, flag):
        return self._x(["wmctrl", flag]).stdout

    def _wm_name(self):
        out = self._wmctrl("-m")
        match = re.search(r"^Name: (.+)$", out, re.M)
        return match.group(1) if match else None

    def windows(self):
        """(window id, class, title) of the managed windows"""
        windows = []
        for line in self._wmctrl("-lx").splitlines():
            parts = line.split(None, 4)
            if len(parts) >= 3:
                windows.append((parts[0], parts[2], parts[4] if len(parts) > 4 else ""))
        return windows

    def _x_outputs(self):
        return re.findall(r"^(\S+) connected", self._x(["xrandr", "--query"]).stdout, re.M)

    def _shot(self, path):
        result = self._x(["import", "-window", "root", str(path)])
        if result.returncode != 0:
            raise RuntimeError(f"import failed: {result.stderr.strip()}")


def x11(name, short, desktop="", server="xvfb", needs=(), sysbus=False, **attrs):
    return type(name.capitalize(), (X11,), {"name": name, "short": short, "desktop": desktop, "server": server,
                                             "needs": needs, "sysbus": sysbus, **attrs})


Xfce = x11("xfce", "x1", "XFCE", "xorg", ("xfce4-session", "xfwm4", "xfdesktop", "wmctrl", "import"), True,
           command=("xfce4-session",), desktop_window="xfdesktop", settle=4)
Cinnamon = x11("cinnamon", "x2", "X-Cinnamon", "xorg", ("cinnamon-session", "cinnamon", "nemo-desktop", "wmctrl", "import"), True,
               command=("cinnamon-session",), desktop_window="nemo-desktop",
               env_extra={"GDK_BACKEND": "x11", "LIBGL_ALWAYS_SOFTWARE": "1"}, settle=12)
Mate = x11("mate", "x3", "MATE", "xorg", ("mate-session", "marco", "caja", "wmctrl", "import"), True,
           command=("mate-session",), desktop_window="Caja", settle=4)
Lxqt = x11("lxqt", "x4", "LXQt", "xorg", ("openbox", "pcmanfm-qt", "lxqt-panel", "wmctrl", "import"), False,
           components=(("openbox",), ("pcmanfm-qt", "--desktop", "--profile=lxqt"), ("lxqt-panel",)),
           desktop_window="pcmanfm", env_extra={"QT_QPA_PLATFORM": "xcb"}, settle=3)
WINDOW_MANAGERS = {
    "i3": ("i3", "-c", "/etc/i3/config"), "openbox": ("openbox",), "bspwm": ("bspwm",), "awesome": ("awesome",),
    "icewm": ("icewm",), "fluxbox": ("fluxbox",), "herbstluftwm": ("herbstluftwm",),
}
WM_CLASSES = [
    x11(name, f"w{index}", "", "xvfb", (argv[0], "wmctrl", "import"), False, components=(argv,))
    for index, (name, argv) in enumerate(WINDOW_MANAGERS.items())
]

KINDS = {
    kind.name: kind
    for kind in (Sway, Labwc, Wayfire, River, Hyprland, Niri, Cosmic, Kde, KdeDual, Gnome, Gnome45, Gnome46, Gnome47, Gnome48, Gnome49, GnomeDing, Gnome45Ding, Gnome46Ding, Xfce, Cinnamon, Mate, Lxqt, *WM_CLASSES)
}
