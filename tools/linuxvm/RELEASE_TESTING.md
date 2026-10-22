# Release testing in the distro VMs

Trigger: when the maintainer says a release is planned, run the whole matrix below and report before anything is tagged.
Nothing here runs on its own schedule.

## What is tested

The release candidate, built the way CI builds it: the engine tarballs (portable, plus the Ubuntu/Fedora/Arch
distribution builds, `tools/package_release.sh`, built in the proot rootfs under `~/.local/share/distro-rootfs`) and
we_manager's .deb, AppImage and Flatpak. Wallpapers used everywhere: one scene with parallax (1111948150), one scene with
particles (1120596121), one video, one preset, two web items (1078208425, 834237690).

Besides the VMs: the PVS-Studio, scan-build and clang-tidy runs against their baselines, the unit tests, the regression sweep
over the installed scenes (`tools/regression/regress.py`), `tools/smoke_test_release.sh` on every tarball, the formatting
check (clang-format 22 on `src`) and a build of both CMake variants (`DISABLE_KDE_FEATURES` on and off).

## The matrix (a VM per row, `tools/linuxvm/linuxvm.sh`)

| VM | Session | Engine path | Extra checks |
|----|---------|-------------|--------------|
| fedora-kde | Plasma Wayland | distribution + Flatpak | KWin script pause (maximized, fullscreen, show desktop), click passthrough |
| fedora-gnome | GNOME Wayland | distribution | shell extension, per-output pause |
| ubuntu-gnome | GNOME Wayland | distribution + .deb we_manager | polkit install/update dialog |
| debian-gnome | GNOME Wayland | portable + .deb + Flatpak | maximized + fullscreen pause, apt dependency install |
| debian-xfce | XFCE X11 | portable + .deb | covered root window, second monitor, apt dependency install |
| zorin-gnome | GNOME Wayland | portable + .deb | Ubuntu 24.04 base; its Xorg session shows nothing in root mode |
| elementary | Pantheon Wayland (X11 selectable) | portable + .deb | the default Wayland session cannot run the engine (Gala has no wlr-layer-shell), the golden disk is that default session; the X11 session needs `--window-background` |
| opensuse-kde | Plasma Wayland | portable | newest libraries, zypper dependency install, maximized pause |
| opensuse-leap | Plasma Wayland (Leap 16, KWin 6.4) | portable | greeter login, KWin 6.4 script ids |
| opensuse-xfce | XFCE X11 | portable | PulseAudio only desktop (no libpipewire), covered root window, second monitor, zypper dependency install |
| arch-hypr | Hyprland | portable + AUR packages | layer shell, per-output pause |
| arch-xfce | XFCE X11 | portable | Arch on a covered root window, second monitor |
| manjaro-kde | Plasma Wayland | portable | pacman dependency install (needs a fully updated system) |
| manjaro-xfce | XFCE X11 | portable | Arch family on a covered root window, second monitor, pacman dependency install after a full update |
| cachyos-kde | Plasma Wayland | portable | x86-64-v3 packages and kernel, Plasma Login Manager autologin, pacman dependency install |
| cachyos-hypr | Hyprland (uwsm, Noctalia shell) | portable | layer shell on Hyprland, greetd autologin, Hello window and keyring prompt closed by PREP |
| garuda-kde | Plasma Wayland | portable | Garuda gaming edition, pacman dependency install after a full update |
| endeavouros | Plasma Wayland | portable | the real EndeavourOS (offline installer, KDE), pacman dependency install after a full update |
| endeavouros-xfce | XFCE X11 | portable | second monitor, the Welcome window is closed by title |
| mint-cinnamon | Cinnamon X11 | portable + .deb | `--window-background` (icons above, below windows, sticky), root mode, second monitor |
| mint22-2 | Cinnamon X11 (Linux Mint 22.2) | portable + .deb | the same checks as mint-cinnamon |
| mx-xfce | XFCE X11 (MX Linux 25.3, Debian 13 based) | portable + .deb | second monitor, we_manager's .deb self update |
| popos-cosmic | COSMIC | portable + .deb | layer shell and window states (`zcosmic_toplevel_info`) on cosmic-comp |
| steamos | Plasma Wayland, read-only root | portable | Steam Deck image on an NVMe disk with Valve SMBIOS, `WEM_LAUNCH` (see below), no dependency install (immutable) |
| steamdeck | Plasma Wayland, read-only root | portable | the steamos disk as its own golden, but shaped like the hardware (`vm.env`: 1280x800, 4 cores, 16G, Zen 2 CPU model, Valve Jupiter SMBIOS) with a fake battery (`test_power` kernel module, seen by UPower); `deck_check.sh` runs the Steam Deck preset's arguments and follows the battery down and up again |
| bazzite | KDE, immutable | portable + Flatpak | rpm-ostree: dependency install refused, Flatpak |

A new distro joins the matrix once its VM has a profile and a golden snapshot. ChimeraOS is not in it: its image installs and
boots in a VM but cannot draw (its Mesa has no virgl, llvmpipe or lavapipe, and the root is read-only), so it needs real
hardware or GPU passthrough.

## One VM run

1. Start from the golden snapshot (a throwaway qcow2 overlay, so every run begins clean and the VM is never changed).
2. Boot headless (`start`), wait for ssh and the desktop session (autologin, inhibit lock/idle as in the profile).
3. Copy the candidate and the test wallpapers in, install it the way a user does (we_manager install or the package).
4. Per wallpaper: start, wait for the first frames, take a screenshot inside the guest (`gnome-screenshot`, `spectacle`,
   `grim` or `cosmic-screenshot` per profile), check it.
5. Behaviour checks: hotswap to another wallpaper, pause while a window is fullscreen (CPU of the engine drops below
   3%) and resume, stop leaves no engine, no web host and no stray window or KWin script, autostart entry works.
6. Collect engine and we_manager logs, power the VM off, delete the overlay.

A screenshot passes when the wallpaper area is not black or flat, is not upside down (compare with the reference frame
of the same wallpaper from `~/.local/share/we_live/refs` or the previous run, tolerance per wallpaper) and the desktop
icons/panel are still visible where the profile says they must be. A run fails on: engine exit or crash (core, `SIGSEGV`
in the log), missing frames, black desktop, a leftover process, a log line matching the error list in the profile.

After a failed first pass the driver repeats only the failed groups in the same boot. A failure that only shows in the retry
is a state problem of the session, not of the first run.

## Web frames (guest_test.sh, every wallpaper run)

The engine runs with `LWE_WEB_STATS=1`; each web wallpaper reports `web frames arrive as dmabufs` or `through shared memory`, and
fails when `--web-frames auto` picked dmabuf and fell back to shm ("restarting the web host"). Expected: dmabuf on every
Wayland desktop and on X11 where a web wallpaper is the startup background, shm for web wallpapers swapped in later on X11
and on NVIDIA's proprietary driver.

## Regression checks for known bugs (guest_test.sh, BEHAVIOR=1)

Run on every VM of the matrix, each one SKIPs where the guest cannot do it:

- hotswap from 3471294034 (cursor particles leave the colour mask's alpha off) to 3818199837 (video layer): the swapped
  frame must look like a fresh start (`check_frame.py --similar`), the old bug left the video transparent, grey clouds.
  The VMs' virgl does not reproduce it (the old engine passes there), so the real test is `tools/check_hotswap_video.sh
  <engine binary>` on the host GPU (difference 102 on the old engine, 3.5 on the fixed one): run it in the regression step
- hotswap changes the picture: `hot-after.png` must differ from `hot-before.png` (`check_frame.py --differ`). On GNOME 46 an
  orphaned window actor of an earlier wallpaper kept a frozen frame on screen and made them identical (see Known problems)
- fullscreen pause, then `MINIMIZE_CMD` hides the window (X11 profiles): the engine must resume
- automute (`--automute`, pulse tools in the guest): another app's sound mutes the wallpaper, the music is back within 5s
  of it stopping, the same while a fullscreen window has the wallpaper paused (it used to take up to 15s and ignored
  other apps while paused)
- hotswap cycle (`hotswap_cycle_check`): one engine goes through scene (1120596121), video (1367456719), web (1078208425), scene
  (1111948150), web (834237690), video and scene again; every step must keep the process, log the swap, show a picture that is not
  flat or the empty desktop (`shot-cycle-*.png`), have a web host only on web wallpapers and add no crash lines to the log
- pointer y direction (`pointer_check.sh`, `hover_check.py`): workshop item 3599498791 (a dock and a calendar whose weekday hover zones
  are the engine's cursor hit test) with the VM's pointer on the Monday row and on the Sunday row of the calendar, the enlarged
  label has to be the one under the pointer (it was mirrored on flipped outputs, i.e. KDE Wayland), and the dock icons have to
  magnify with the pointer on the dock. Windows that open late and cover the calendar are closed by title before every pointer
  position (XFCE's Displays dialog, an xfce4-panel plugin dialog, EndeavourOS Welcome, Ubuntu's update prompts, Steam's sign-in on
  SteamOS). `POINTER=0` in a profile skips it
- second monitor (`MULTI=1` and `MULTI_SHOT_CMD` in the profile, the VM starts with `LINUXVM_OUTPUTS=2`): the wallpaper must
  show on both screens, the old X11 root handling painted one
- `clean_install_check.sh` (needs `--app` and `--engine-tarball`): no engine, libvulkan removed (the engine needs it, Electron
  does not; nss and libXxf86vm take Electron down too), we_manager's prebuilt install has to bring the library back (pacman, apt,
  dnf and zypper paths). Ubuntu-based systems (Ubuntu, Mint, Zorin) bundle their libraries in the test tarball and removing libvulkan1
  leaves apt with broken dependents, so there the check only starts from a system without `libglut.so.3.12`. The app dir is pushed to the
  guest on every run (golden disks carry an old build), a .deb is installed when `we-manager` is missing; its engine tag is `v98.0.*` so
  `update_check.sh`'s `v99.0.*` is the newer one and the engine must find all its libraries
- unit test `Cursor y in scene units ...` (tests/MouseCoordinates.cpp): cursor y grows upwards on flipped and unflipped outputs

## Automation

All of it lives in `tools/linuxvm/`:

- `profiles/<vm>.sh`: how to reach the session (`SCREEN`, `SHOT_CMD`, `FULLSCREEN_CMD`, `MAXIMIZE_CMD`, `WEM`, `WEM_FLAGS`,
  `WEM_LAUNCH`, `PREP`, `MULTI`, `POINTER`, `POINTER_DOCK_XY` for a desktop whose own dock covers the pointer spot of the dock check, `LOGIN_PASSWORD` for greeters without autologin). `WEM_LAUNCH` is the prefix of the
  we_manager start line: SteamOS needs `systemd-run --user --scope -q` because a process started from an ssh session dies with it.
- `linuxvm.sh golden <vm>` / `fresh <vm>`: the VM's disk is frozen as `golden.qcow2` and runs on a throwaway overlay, `fresh`
  throws the overlay away. Every VM of the matrix has one.
- `<vm>/vm.env` (sourced by `linuxvm.sh start`): `LINUXVM_SIZE`, `LINUXVM_CPUS`, `LINUXVM_MEM` and `LINUXVM_CPU` (the QEMU `-cpu` model) for that VM alone.
  The `steamdeck` VM was made by copying `steamos`'s golden disk (new port, `qemu-extra`, `vm.env`) and adding
  `/etc/modules-load.d/test_power.conf` + `/etc/modprobe.d/test_power.conf` (AC and USB off, battery discharging) before `golden`.
- `release_check.sh [--vm name]... [--candidate dir] [--app dir|.deb] [--engine-tarball tgz] [--no-overlay]`: loops over the
  profiles sequentially, writes `<REPORT_DIR>/summary.md` (default `reports/<date>/`) with screenshots and logs per VM, retries the
  failed checks once, powers the VM off (ssh `systemctl poweroff`, ACPI does not reach a locked KDE session) and resets the
  overlay. `KEEP_VM=1` leaves the VM running, `REPORT_DIR` keeps parallel runs apart (they would share one summary.md).
- `guest_test.sh` (one PASS/FAIL/SKIP line per check; BEHAVIOR=1 adds hotswap, fullscreen and maximized pause through
  `pause_check`, SIGKILL cleanup), `run_behavior.sh` (the same by hand), `pointer_check.sh` and `hover_check.py` (the pointer
  checks), `host_shots.sh` (screenshots through the VM's display, and a click on the screen centre so a test window gets the
  focus), `check_frame.py` (black/flat/unchanged/hotswap differs, and `--reference` against `reference/<id>.png`: upside down, mirrored or another crop; Pillow, run with `-I`). `make_reference_frames.sh` renders those references headlessly from the payload (video wallpapers have none). On X11 sessions `guest_test.sh` also runs a `windowbg` group (`--window --window-background` on the output's geometry).
- `clean_install_check.sh` (a first install of the prebuilt engine through we_manager), `wem_check.sh` (we_manager install,
  launch, stop, kill all, autostart entry), `update_check.sh` (`fake_github.py` serving a newer app .deb, `bump_deb.py`, and the
  candidate engine as a newer release), `deps_check.sh` (the "install build dependencies" button on the VM's package manager,
  then the engine's CMake configure on the source tarball), `autostart_check.sh` (a one wallpaper autostart playlist, a real
  reboot, we_manager and the engine back; wallpapers only come back through the autostart playlist, a wallpaper launched by hand
  is not restored), `flatpak_check.sh` (bundle install, scene + web wallpaper, stop, kill all, autostart entry).
- `update_check.sh` and `clean_install_check.sh` start `fake_github.py` through `fake_github_lib.sh`: the apps ask
  `https://api.github.com` (port 443), so the guest redirects 10.0.2.2:443 to a host port of the VM's own (nft or iptables rule)
  and VMs run these checks in parallel. A guest without nft/iptables, or a redirect that does not reach our server, falls back to
  host port 443 behind `/tmp/lwe-fake-github.lock`. The report's `wem.txt` says which one ran (`fake api.github.com on host port ...`).
- `prep_guest.sh <vm>` fills `~/lwe` in a VM (the payload from `$LINUXVM/payload`, the candidate engine, the guest scripts), it must
  exist before `release_check.sh` runs. `qtype.sh <vm> <us|de> <text>` types one key at a time over QMP (`linuxvm.sh type` drops
  characters under load). `bootstrap_ssh.sh <vm> <port> <us|de>` sets up ssh in an ISO installed VM: it serves a setup script from
  the host and runs it in a terminal already open in the guest (`FETCH='wget -q'` where curl is missing). `<vm>/qemu-extra` holds
  extra QEMU arguments (SteamOS: NVMe disk and Valve SMBIOS, MX Linux: the ISO as USB disk).
- `update_golden_extension.sh <vm>...` puts the repo's GNOME Shell extension.js onto the golden disks of GNOME VMs (fresh overlay, replace the file, `qemu-img commit` into `golden.qcow2`).
- GNOME Shell extension debugging without touching the golden disks: `debug_extension.py <dir>` builds the extension with `LWE_DBG`
  journal lines, `ghost_run.sh <vm> <tag> [extension.js]` runs `release_check.sh` on an overlay that has that extension (needs
  `LWE_CANDIDATE`, `LWE_ENGINE_TARBALL`, `LWE_APP_DEB` or `LWE_APP_DIR`, writes `reports/ghost/<vm>-<tag>/shell-debug.log`),
  `ghost_loop.sh <vm> <tries>` repeats it until a check fails, and `ghost_diag.sh <vm>` looks at a VM kept up with `KEEP_VM=1`.

## Running it

Start `release_check.sh` in the background and watch it, 10 to 25 minutes per VM. One or two VMs fit in RAM comfortably (6 to 8 GB
each); four at a time worked on a 30 GB host, check `free -g` before starting another. Every VM has its own fake GitHub port, so
parallel runs are fine, but a pause check can fail only because of the extra CPU load: rerun that VM alone. Power off every VM that
was started for an experiment. A failing VM is reported with its screenshot and log excerpt and the matrix carries on with the next
one. Nothing is fixed and tagged on its own: the report goes to the maintainer first. Cleanup after a run: all VMs powered off,
overlays gone.

## Pitfalls of the rig

- A locked or sleeping screen makes every picture identical: KDE VMs need screen lock and sleep disabled (`kde-inhibit`, in the
  profile's PREP).
- Leftover test windows keep the engine paused, and a paused engine does not read hotswap requests. Windows of the desktop itself
  (Welcome apps, update prompts, panel dialogs) cover the wallpaper the same way.
- `pkill -f` patterns match the ssh command that carries them (use `pkill -x`). The remote login shell can be zsh, which aborts a
  command on an unmatched glob.
- Two runs on one VM disturb each other, including a hand started experiment while a matrix lane reaches that VM.
- A GNOME fullscreen window only counts when it has the focus (`host_shots.sh` clicks it).
- KWin 6.4 reuses the id of a live script after any unload, which also hits `maximize_kde.sh` and any script loaded over D-Bus
  (check the ids against the `Script<id>` children of `/Scripting`).
- The engine's command line shows `/var/home` on Fedora Atomic systems. we_manager's library scan skips symlinked folders. An
  autostarted we_manager only brings wallpapers back through its autostart playlist.
- Force-removing a library with dpkg breaks apt for the rest of the run (hence the `libglut` path for Ubuntu-based profiles).
- Many `qemu-system-x86` zombies pile up under a long matrix run (the container's pid limit): restart before a very long run.

## Per VM notes

- Bazzite's /home is /var/home.
- Debian's cloud image needs the regular kernel for any graphics.
- GNOME needs the extension enabled and one relogin. Bazzite, Leap and COSMIC stop at a greeter (`LOGIN_PASSWORD`). The COSMIC VM
  needs `--disable-gpu-compositing` for we_manager.
- Hyprland: `sessenv.sh` reads the session variables from `systemctl --user show-environment` when the compositor process does not
  carry them (uwsm).
- MX Linux 25.3 XFCE: the ISO does not boot as a CD-ROM under OVMF ("Failed to boot both default and fallback entries") but boots as a
  USB disk (`qemu-extra`: `-drive file=<iso>,if=none,id=usbd,format=raw,readonly=on -device usb-storage,drive=usbd,bootindex=1`, pick
  the systemd entry). Its own installer (whole disk, user lab, autologin, no Samba), then `openssh-server` from a terminal
  (ctrl-alt-t) and `bootstrap_ssh.sh mx-xfce 8772 us lab`.
- Installer pitfalls: Calamares starts with a German layout, pick English on the keyboard page (or use `qtype.sh de`), clear the
  prefilled login and hostname fields, Zorin and elementary need `openssh-server` installed from the live terminal, never click
  "Restart now" in an installer (it boots the ISO again).

## Known problems and open items

- **elementary** (Gala has no wlr-layer-shell): the engine draws through XWayland there (2026-10-21, 73 checks pass), so only X11 windows pause it,
  the profile's test window is an XWayland one and there is no maximize check.
- **Frozen wallpaper on GNOME 46** (Ubuntu, Zorin): Mutter frees a wallpaper window without unmanaging it, and its actor stays in
  the Shell's window group with the last frame (`hot-after` identical to `hot-before`, no engine alive). It turns up in a minority
  of full runs, so a single clean run proves little. A ghost shows as a surplus wallpaper `MetaWindowActor` in the `GROUP` lines of
  the debug extension. The extension hides such orphans (`_hideOrphanActors`), and the golden disks of the GNOME VMs carry the fixed one since
  2026-10-21 (`update_golden_extension.sh`).
- **Running the matrix in lanes** (2026-10-22): four lanes at once fit in 30 GB (about 5 GB resident per VM), a fifth is fine for harness
  work. A lane is `release_check.sh --vm <name>` per VM one after the other, each with its own report folder. Under that load a few windows
  turn up later than the 20 s settle in `release_check.sh`: we_manager's autostarted window on a Debian GNOME guest and Steam's sign-in on
  SteamOS/Steam Deck, both are closed once more after the settle. An app that restarts after its own .deb update (MX) can drop the first
  `installPrebuilt()` call, `update_check.sh` asks up to three times.
- **Electron's `EGL_PLATFORM=wayland`** reaches the engine when we_manager starts it in a Wayland session. The X11 driver (XWayland
  fallback on elementary, a web wallpaper as startup background) makes its EGL display under it and crashed in libwayland-client, only
  through we_manager (a shell start had no such variable). The GLFW driver drops the variable before `glfwInit`.
