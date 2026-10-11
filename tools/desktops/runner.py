"""Where a session's programs run: natively when they are installed, in a distribution rootfs under proot otherwise.

Sessions never inherit the caller's environment, a sandbox shares its host's display, audio and D-Bus sockets and a
test must never reach them.
"""
import os
import shutil
import signal
import subprocess
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASE = Path(os.environ.get("LWE_DESKTOPS_DIR", "/tmp/lwe-d"))
ROOTFS_DIR = Path(os.environ.get("LWE_ROOTFS_DIR", Path.home() / ".local/share/distro-rootfs"))
ROOTFS = {
    "fedora": Path(os.environ.get("LWE_FEDORA_ROOTFS", ROOTFS_DIR / "fedora-44-sys")),
    "arch": Path(os.environ.get("LWE_ARCH_ROOTFS", ROOTFS_DIR / "arch-compositors")),
}
BIN_DIRS = ("usr/bin", "usr/sbin", "usr/libexec", "bin", "sbin")
SYSTEM_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"


def proot_binary():
    return os.environ.get("PROOT") or shutil.which("proot") or str(Path.home() / ".local/share/arch-proot/proot")


class Userland:
    """native, or the programs of the fedora or arch rootfs"""

    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return f"Userland({self.name})"

    def has(self, program):
        if self.name == "native":
            return shutil.which(program) is not None
        root = ROOTFS[self.name]
        return any((root / directory / program).exists() for directory in BIN_DIRS)

    def exists(self, path):
        """an absolute path inside the userland"""
        root = Path("/") if self.name == "native" else ROOTFS[self.name]
        return (root / path.lstrip("/")).exists()

    def path(self, program):
        if self.name == "native":
            return shutil.which(program) or next(
                (str(Path("/") / d / program) for d in BIN_DIRS if (Path("/") / d / program).exists()), None
            )
        root = ROOTFS[self.name]
        return next((f"/{d}/{program}" for d in BIN_DIRS if (root / d / program).exists()), None)

    @staticmethod
    def pick(*programs, prefer=("native", "fedora", "arch")):
        """the first userland that has all of the programs"""
        for name in prefer:
            userland = Userland(name)
            if (name == "native" or ROOTFS[name].is_dir()) and all(userland.has(p) for p in programs):
                return userland
        raise RuntimeError(
            f"none of {', '.join(programs)} is installed here and no rootfs has it, "
            f"run: python3 tools/desktop_session.py setup"
        )

    def command(self, argv, env):
        assignments = [f"{key}={value}" for key, value in env.items()]
        if self.name == "native":
            home = env.get("HOME", "/tmp")
            argv = [_without_file_capabilities(argv[0]), *argv[1:]]
            command = ["env", "-i", f"PATH={os.environ.get('PATH', SYSTEM_PATH)}", f"HOME={home}", "LANG=C.UTF-8", *assignments, *argv]
            # a read-only shared /tmp/.X11-unix stops compositors from starting Xwayland
            if not os.access("/tmp/.X11-unix", os.W_OK):
                sockets = BASE / "x11-unix"
                sockets.mkdir(parents=True, exist_ok=True, mode=0o1777)
                command = [proot_binary(), "-b", f"{sockets}:/tmp/.X11-unix", *command]
            return command
        return [str(REPO / "tools/distro_rootfs.sh"), "run", str(ROOTFS[self.name]), "-u", "env", *assignments, *argv]

    def process_env(self):
        env = dict(os.environ)
        if self.name != "native":
            env["BINDS"] = f"-b {BASE}:{BASE} -b {REPO}:{REPO}"
            env["PROOT"] = proot_binary()
        return env

    def spawn(self, argv, env, log):
        """starts a program in a session of its own, so that its whole tree can be taken down"""
        with open(log, "ab") as out:
            return subprocess.Popen(
                self.command(argv, env), stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                env=self.process_env(), start_new_session=True,
            )

    def run(self, argv, env, timeout=60, check=False):
        return subprocess.run(
            self.command(argv, env), capture_output=True, text=True, timeout=timeout, check=check,
            stdin=subprocess.DEVNULL, env=self.process_env(),
        )


def _without_file_capabilities(program):
    """no_new_privs (Docker) refuses to exec files with capabilities (Fedora's sway/kwin_wayland), a copy has none"""
    path = shutil.which(program) or program
    if not os.path.isabs(path):
        return program
    try:
        os.getxattr(path, "security.capability")
    except OSError:
        return program
    with open("/proc/self/status") as status:
        if "NoNewPrivs:\t1" not in status.read():
            return program
    copy = BASE / "nocap" / os.path.basename(path)
    if not copy.exists() or copy.stat().st_mtime < os.stat(path).st_mtime:
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, copy)
        copy.chmod(0o755)
    return str(copy)


def wait_for(what, check, timeout=30.0, interval=0.2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(interval)
    raise TimeoutError(f"timed out after {timeout:.0f}s waiting for {what}")


def terminate(pids, grace=3.0):
    """SIGTERM to the process groups, SIGKILL for what is left after the grace time"""
    for sig, wait in ((signal.SIGTERM, grace), (signal.SIGKILL, 1.0)):
        for pid in pids:
            try:
                os.killpg(pid, sig)
            except (ProcessLookupError, PermissionError):
                pass
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline and any(_alive(pid) for pid in pids):
            time.sleep(0.1)


def _alive(pid):
    try:
        with open(f"/proc/{pid}/stat") as stat:
            return stat.read().rsplit(")", 1)[1].split()[0] != "Z"
    except (FileNotFoundError, ProcessLookupError):
        return False


def kill_with_runtime_dir(run_dir):
    """what changed its session id (daemons started through D-Bus activation) is found by its runtime dir"""
    needle = f"XDG_RUNTIME_DIR={run_dir}".encode() + b"\0"
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            if needle in (entry / "environ").read_bytes():
                os.kill(int(entry.name), signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
