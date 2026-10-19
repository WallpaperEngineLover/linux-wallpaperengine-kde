# Release testing in the distro VMs

Trigger: when the maintainer says a release is planned, run the whole matrix below, one VM after the other, and report
before anything is tagged. Nothing here runs on its own schedule.

## What is tested

The release candidate, built the way CI builds it: the engine tarballs (portable, plus the Ubuntu/Fedora/Arch
distribution builds, `tools/package_release.sh`, built in the proot rootfs under `~/.local/share/distro-rootfs`) and
we_manager's .deb, AppImage and Flatpak. Wallpapers used everywhere: one scene with parallax (1111948150), one scene with
particles (1120596121), one video, one preset, two web items (1078208425, 834237690).

## The matrix (a VM per row, `tools/linuxvm/linuxvm.sh`)

| VM | Session | Engine path | Extra checks |
|----|---------|-------------|--------------|
| fedora-kde | Plasma Wayland | distribution + Flatpak | KWin script pause (maximized, fullscreen, show desktop), click passthrough |
| fedora-gnome | GNOME Wayland | distribution | shell extension, per-output pause |
| ubuntu-gnome | GNOME Wayland | distribution + .deb we_manager | polkit install/update dialog |
| arch-hypr | Hyprland | portable + AUR packages | layer shell, per-output pause |
| mint-cinnamon | Cinnamon X11 | portable | `--window-background` (icons above, below windows, sticky), root mode |
| debian-gnome | GNOME Wayland | portable + .deb + Flatpak | maximized + fullscreen pause, apt dependency install |
| opensuse-kde | Plasma Wayland | portable | newest libraries, zypper dependency install, maximized pause |
| opensuse-xfce | XFCE X11 | portable | PulseAudio only desktop (no libpipewire), covered root window, zypper dependency install |
| manjaro-kde | Plasma Wayland | portable | pacman dependency install (needs a fully updated system) |
| popos-cosmic | COSMIC | portable + .deb | layer shell and window states (`zcosmic_toplevel_info`) on cosmic-comp |
| bazzite | KDE, immutable | portable + Flatpak | rpm-ostree: dependency install refused, Flatpak |

Rows are added as the VMs of `TODO.md` -> "Real-session distro VMs" get built. A new distro joins the matrix once its
VM has a profile and a golden snapshot.

## One VM run

1. Start from the golden snapshot (a throwaway qcow2 overlay, so every run begins clean and the VM is never changed).
2. Boot headless (`start`), wait for ssh and the desktop session (autologin, inhibit lock/idle as in the profile).
3. Copy the candidate and the test wallpapers in, install it the way a user does (we_manager install or the package).
4. Per wallpaper: start, wait for the first frames, take a screenshot inside the guest (`gnome-screenshot`, `spectacle`,
   `grim` or `cosmic-screenshot` per profile), check it.
5. Behaviour checks: hotswap to another wallpaper, pause while a window is fullscreen (CPU of the engine drops below
   3%) and resume, stop leaves no engine, no web host and no stray window or KWin script, autostart entry works.
6. Collect engine and we_manager logs, powers the VM off, deletes the overlay.

A screenshot passes when the wallpaper area is not black or flat, is not upside down (compare with the reference frame
of the same wallpaper from `~/.local/share/we_live/refs` or the previous run, tolerance per wallpaper) and the desktop
icons/panel are still visible where the profile says they must be. A run fails on: engine exit or crash (core, `SIGSEGV`
in the log), missing frames, black desktop, a leftover process, a log line matching the error list in the profile.

## Automation

All of it lives in `tools/linuxvm/`:

- `profiles/<vm>.sh`: how to reach the session (`SCREEN`, `SHOT_CMD`, `FULLSCREEN_CMD`, `MAXIMIZE_CMD`, `WEM`, `WEM_FLAGS`,
  `PREP`, `LOGIN_PASSWORD` for greeters without autologin). Written for the VMs above, the older VMs (fedora, ubuntu, arch,
  mint) still need theirs checked.
- `linuxvm.sh golden <vm>` / `fresh <vm>`: the VM's disk is frozen as `golden.qcow2` and runs on a throwaway overlay, `fresh`
  throws the overlay away. Done for debian-gnome, opensuse-kde, opensuse-xfce, popos-cosmic, bazzite and manjaro-kde.
- `release_check.sh [--vm name]... [--candidate dir] [--app dir|.deb] [--engine-tarball tgz]`: loops over the profiles
  sequentially, writes `reports/<date>/summary.md` with screenshots and logs per VM, retries the failed checks once,
  powers the VM off (ssh `systemctl poweroff`, ACPI does not reach a locked KDE session) and resets the overlay.
- `guest_test.sh` (one PASS/FAIL/SKIP line per check; BEHAVIOR=1 adds hotswap, fullscreen and maximized pause through
  `pause_check`, SIGKILL cleanup), `run_behavior.sh` (the same by hand), `host_shots.sh` (screenshots through the VM's
  display, and a click on the screen centre so a test window gets the focus), `check_frame.py` (black/flat/unchanged/
  hotswap differs, Pillow, run with `-I`).
- `wem_check.sh` (we_manager install, launch, stop, kill all, autostart entry), `update_check.sh` (`fake_github.py` serving a
  newer app .deb, `bump_deb.py`, and the candidate engine as a newer release), `deps_check.sh` (the "install build
  dependencies" button on the VM's package manager, then the engine's CMake configure on the source tarball),
  `autostart_check.sh` (a one wallpaper autostart playlist, a real reboot, we_manager and the engine back; wallpapers only
  come back through the autostart playlist, a wallpaper launched by hand is not restored), `flatpak_check.sh` (bundle install,
  scene + web wallpaper, stop, kill all, autostart entry).
- Still open: reference frames per wallpaper in `check_frame.py` (upside down, wrong crop), profiles for the older VMs.

## Running it

I start `release_check.sh` in the background and watch it, roughly 10 minutes per VM, so about two hours for ten.
A failing VM is reported with its screenshot and log excerpt and the matrix carries on with the next one. I do not fix
and re-tag on my own: the report goes to the maintainer first. Cleanup after a run: all VMs powered off, overlays gone.

## State (2026-10-19)

Built and used: everything under "Automation". Golden disks and profiles exist for debian-gnome, debian-xfce, opensuse-kde,
opensuse-xfce, opensuse-leap, manjaro-kde, arch-xfce (Arch with XFCE, stands in for EndeavourOS), popos-cosmic and bazzite.
fedora-kde, fedora-gnome and ubuntu-gnome have draft profiles that were not run through `release_check.sh`, arch-hypr and
mint-cinnamon have none yet. Per VM notes: KDE VMs need screen lock and sleep disabled (`kde-inhibit`, in the profile's
PREP), Bazzite's /home is /var/home, Debian's cloud image needs the regular kernel for any graphics, GNOME needs its extension
enabled and one relogin, Bazzite, Leap and COSMIC stop at a greeter (LOGIN_PASSWORD), the COSMIC VM needs
`--disable-gpu-compositing` for we_manager.

Pitfalls found while writing it: a locked or sleeping screen makes every picture identical, leftover test windows keep the
engine paused (a paused engine does not read hotswap requests), the engine's command line shows /var/home on Fedora Atomic
systems, `pkill -f` patterns match the ssh command that carries them (use `pkill -x`), the remote login shell can be zsh (an
unmatched glob aborts the command), two runs on one VM disturb each other, a GNOME fullscreen window only counts when it has
the focus (host_shots.sh clicks it), we_manager's library scan skips symlinked folders, an autostarted we_manager only
brings wallpapers back through its autostart playlist, one or two VMs fit in RAM at a time (8 GB each).

## Order of work

1. Profiles for fedora-kde, fedora-gnome, ubuntu-gnome, arch-hypr and mint-cinnamon, golden disks, and `release_check.sh` runs
   of them.
2. Run the whole matrix on the committed build, fix what it finds, keep that report as the baseline for comparisons
   (reference frames per wallpaper for `check_frame.py` first).
3. Remaining distros from the TODO (CachyOS, SteamOS, Zorin, Manjaro XFCE, Garuda, ChimeraOS, elementary, MX; installer
   ISOs, no cloud images) join the matrix one by one.
