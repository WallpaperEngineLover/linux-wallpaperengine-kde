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
| debian-gnome | GNOME Wayland | portable | |
| opensuse-tumbleweed-kde | Plasma Wayland | portable | newest libraries |
| manjaro-kde | Plasma Wayland | portable + AUR | |
| popos-cosmic | COSMIC | portable | layer shell on cosmic-comp |
| bazzite | KDE/Gamescope, immutable | portable + Flatpak | rpm-ostree path of we_manager |

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

## Automation to build

- `tools/linuxvm/profiles/<vm>.sh`: how to reach the session (env vars for `DISPLAY`/`WAYLAND_DISPLAY`/dbus), the
  screenshot command, how to install the candidate, which extra checks apply.
- `tools/linuxvm/release_check.sh [--vm name ...] [--candidate dir]`: loops over the profiles sequentially (RAM, 30 GB,
  fits one or two VMs), runs `guest_test.sh` in each over ssh, then writes `reports/<date>/summary.md` plus the
  screenshots and logs, exit code 1 when anything failed.
- `tools/linuxvm/guest_test.sh`: the per-VM steps above, prints one `PASS/FAIL <check>` line each.
- `tools/linuxvm/check_frame.py`: black/flat/upside-down/reference comparison (Pillow, run with `-I`).
- Retry a failed check once before calling it a failure (first-frame timing in VMs is noisy).

## Running it

I start `release_check.sh` in the background and watch it, roughly 10 minutes per VM, so about two hours for ten.
A failing VM is reported with its screenshot and log excerpt and the matrix carries on with the next one. I do not fix
and re-tag on my own: the report goes to the maintainer first. Cleanup after a run: all VMs powered off, overlays gone.

## Order of work

1. Finish the first batch of VMs (Debian GNOME, openSUSE Tumbleweed KDE, Manjaro KDE, Pop!_OS COSMIC, Bazzite).
2. Give every VM a profile and a golden snapshot, starting with the five that exist.
3. Write `guest_test.sh`, `check_frame.py` and the driver, prove them on Mint and Fedora KDE.
4. Run the full matrix on the current build, fix what it finds, keep that report as the baseline for comparisons.
5. Remaining distros from the TODO join the matrix one by one.
