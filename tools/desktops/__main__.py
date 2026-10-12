import argparse
import os
import subprocess
import sys

from .runner import REPO, ROOTFS, Userland
from .session import KINDS, Session, Unsupported

FEDORA_PACKAGES = REPO / "tools/desktops/rootfs-fedora.txt"
ARCH_PACKAGES = REPO / "tools/desktops/rootfs-arch.txt"
UBUNTU_PACKAGES = REPO / "tools/desktops/rootfs-ubuntu.txt"
FEDORA_GNOME_PACKAGES = REPO / "tools/desktops/rootfs-fedora-gnome.txt"


def _apt(archive):
    """an Ubuntu install, from old-releases once the release is out of support"""
    return ["sh", "-c", f"sed -i 's#http://[a-z]*.ubuntu.com#http://{archive}.ubuntu.com#' /etc/apt/sources.list "
            "/etc/apt/sources.list.d/*.sources 2>/dev/null; apt-get update -qq && "
            'DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@"', "apt"]


def setup_ding():
    """Desktop Icons NG for the GNOME 50 lab, from extensions.gnome.org (not packaged in Fedora)"""
    import json
    import urllib.request
    import zipfile
    from .session import DING_DIR
    base = "https://extensions.gnome.org"
    with urllib.request.urlopen(f"{base}/extension-info/?uuid=ding@rastersoft.com&shell_version=50") as reply:
        info = json.load(reply)
    archive = DING_DIR.parent / "ding.zip"
    archive.parent.mkdir(parents=True, exist_ok=True)
    urllib.request.urlretrieve(base + info["download_url"], archive)
    with zipfile.ZipFile(archive) as extension:
        extension.extractall(DING_DIR)
    subprocess.run(["glib-compile-schemas", str(DING_DIR / "schemas")], check=True)
    print(f"Desktop Icons NG {info.get('version')} in {DING_DIR}")


def setup(which):
    """the rootfs a session runs in where its programs are not installed natively"""
    if which == "ding":
        return setup_ding()
    script = str(REPO / "tools/distro_rootfs.sh")
    image, packages, install = {
        "fedora": ("fedora:44", FEDORA_PACKAGES, ["dnf", "install", "-y", "--setopt=install_weak_deps=False"]),
        "arch": ("archlinux:latest", ARCH_PACKAGES, ["pacman", "-Syu", "--noconfirm", "--needed", "--disable-sandbox"]),
        **{f"gnome{shell}": (f"ubuntu:{release}", UBUNTU_PACKAGES, _apt(archive)) for shell, release, archive in (
            ("45", "23.10", "old-releases"), ("46", "24.04", "archive"), ("47", "24.10", "old-releases"),
            ("48", "25.04", "archive"))},
        "gnome49": ("fedora:43", FEDORA_GNOME_PACKAGES, ["dnf", "install", "-y", "--setopt=install_weak_deps=False"]),
    }[which]
    root = ROOTFS[which]
    if not root.is_dir():
        subprocess.run([script, "fetch", image, str(root)], check=True)
    if which == "arch":
        # proot has no free space to report
        subprocess.run(["sed", "-i", "s/^CheckSpace/#CheckSpace/", str(root / "etc/pacman.conf")], check=True)
    names = packages.read_text().split()
    subprocess.run([script, "run", str(root), *install, *names], check=True)
    # the session bus ignores uids without a passwd entry
    uid, gid = os.getuid(), os.getgid()
    subprocess.run([script, "run", str(root), "sh", "-c",
                    f"getent group {gid} >/dev/null || groupadd -g {gid} lwe; "
                    f"getent passwd {uid} >/dev/null || useradd -u {uid} -g {gid} -M -d /tmp -s /bin/bash lwe"], check=True)

def main():
    parser = argparse.ArgumentParser(prog="desktop_session.py", description="Headless desktop sessions for tests")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="the sessions and where each would run")
    sub.add_parser("setup", help="create the rootfs for sessions that are not installed natively").add_argument(
        "which", choices=("fedora", "arch", "gnome45", "gnome46", "gnome47", "gnome48", "gnome49", "ding"))
    start = sub.add_parser("start", help="start a session")
    start.add_argument("name", choices=sorted(KINDS))
    start.add_argument("--size", default="1920x1080")
    start.add_argument("--outputs", type=int, default=1)
    for name, text in (("env", "shell exports for a program under test"), ("stop", "stop a session or all"),
                       ("status", "whether a session runs")):
        sub.add_parser(name, help=text).add_argument("name", nargs="?" if name == "stop" else None, default="all")
    tool = sub.add_parser("tool", help="run a program in a session's userland: swaymsg, hyprctl, wmctrl, grim")
    tool.add_argument("name")
    tool.add_argument("argv", nargs=argparse.REMAINDER)
    shot = sub.add_parser("shot", help="screenshot of a session")
    shot.add_argument("name")
    shot.add_argument("path")
    args = parser.parse_args()

    if args.command == "list":
        for name, userland in Session.available().items():
            print(f"{name:14} {userland or 'not available (run setup)'}")
    elif args.command == "setup":
        setup(args.which)
    elif args.command == "start":
        width, height = (int(part) for part in args.size.split("x"))
        session = Session.of(args.name, size=(width, height), outputs=args.outputs).start()
        print(f"{args.name} is up in {session.userland.name}, use: eval \"$(python3 tools/desktop_session.py env {args.name})\"")
    elif args.command == "stop":
        for name in (KINDS if args.name in ("all", None) else [args.name]):
            Session.of(name).stop()
    elif args.command == "status":
        print("running" if Session.of(args.name).is_running() else "stopped")
    elif args.command == "env":
        print(Session.load(args.name).shell_exports(), end="")
    elif args.command == "tool":
        session = Session.load(args.name)
        result = session.run(args.argv)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.exit(result.returncode)
    elif args.command == "shot":
        try:
            Session.load(args.name).shot(args.path)
        except Unsupported as error:
            sys.exit(f"{error}")



if __name__ == "__main__":
    main()
