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
check (clang-format 22 on `src`), a build of both CMake variants (`DISABLE_KDE_FEATURES` on and off) and the GitHub workflow's
`clang-build` job in Debug and Release (`tools/clang_build_check.sh`: everything built with clang 18 inside an Ubuntu 24.04 rootfs with the job's
apt packages, `--setup` makes it once, `--host` uses this machine's clang; CEF is taken from the main build tree). The job's clang 18 flagged a
never read private field that newer clang (22) lets through, the rootfs reproduces that failure on 7bf52ed6. All of
them run before the candidate's smoke tests and the VM matrix, so a pipeline failure in one of those steps shows up here first.

## The matrix (a VM per row, `tools/linuxvm/linuxvm.sh`)

| VM | Session | Engine path | Extra checks |
|----|---------|-------------|--------------|
| fedora-kde | Plasma Wayland | distribution + Flatpak | KWin script pause (maximized, fullscreen, show desktop), click passthrough |
| fedora-kde5 | Plasma 5.27 Wayland (Fedora 39) | portable | no pause, automute or pointer checks (the engine reads the cursor and the windows through KWin scripts that need Plasma 6), desktop icons overlay without wayland-devel (libwayland-client.so symlink workaround) |
| fedora-gnome | GNOME Wayland | distribution | shell extension, per-output pause |
| ubuntu-gnome | GNOME Wayland | distribution + .deb we_manager | polkit install/update dialog |
| debian-gnome | GNOME Wayland | portable + .deb + Flatpak | maximized + fullscreen pause, apt dependency install |
| debian-xfce | XFCE X11 | portable + .deb | covered root window, second monitor, apt dependency install |
| zorin-gnome | GNOME Wayland | portable + .deb | Ubuntu 24.04 base |
| zorin-xorg | GNOME Xorg (Zorin's X11 session) | portable + .deb | the zorin-gnome disk with GDM switched to Xorg by `BOOT_PREP`; `--screen-root` draws into the engine's own background window. Never in a lane next to zorin-gnome (same disk), own `REPORT_DIR` |
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
| steamdeck | Plasma Wayland, read-only root | portable | the steamos disk as its own golden, but shaped like the hardware (`vm.env`: 1280x800, 4 cores, 16G, Zen 2 CPU model, Valve Jupiter SMBIOS) with a fake battery (`test_power` kernel module, seen by UPower); `deck_check.sh` runs the Steam Deck preset's arguments and follows the battery down and up again Plasma's Battery power profile turns the display off about a minute after login (DPMS off, the VM's screen reads "Display output is not active"), so `PREP` turns the display timeouts off in `powerdevilrc` and restarts powerdevil (a `kde-inhibit` is not enough). `POINTER=0`: the weekday hover check is made for 1920x1080, the VM's screen is 1280x800 (the pointer spots and crops would miss, `steamos` runs it). |
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

## Session bus during a pause (guest_test.sh, every Plasma 6 row)

Part of the fullscreen `pause_check`, switched on by `XDG_CURRENT_DESKTOP=KDE`. A session bus whose quota is used up kicks
plasmashell, the portals and powerdevil off it (seen on a real Nobara desktop 2026-10-27), so while a fullscreen window
has the engine paused for longer than 3 s: the KWin cursor script is unloaded (`$XDG_RUNTIME_DIR/lwe-cursor-input-*.js`
is gone, KWin sends nothing), 300 MPRIS `PropertiesChanged` signals of 4 KB (`gdbus emit`) leave the engine alive, and
after the window closes the cursor script is back within 20 s, the log has no "could not load its KWin script again",
"session bus dropped the connection" or "DBus connection dropped", the journal has no new `exceeded its 'bytes' quota`
line and plasmashell is still on the bus. The same behaviour runs in the KDE lab on every change:
`tests/desktops/test_kde.py -k "cursor_script or media_signals"` (script unloaded while paused and reloaded after,
pointer calls counted with `dbus-monitor`, the final position of a flick reported, an MPRIS flood while paused).

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
- automute ignore list: the engine started without `--automute` (what we_manager launches for a wallpaper on the list) keeps
  playing while another app does and runs no detector (`automute_ignore_check`, group `automuteignore`). It is the same engine
  code on every distribution, so only profiles with `AUTOMUTE_IGNORE=1` (fedora-kde, fedora-gnome) run it, or `ONLY=automuteignore`;
  `wem_check.sh` checks the we_manager side on every VM: with the global automute on, a wallpaper whose own automute is off is
  launched without `--automute`, taken off the list it gets the flag back
- transition (`transition_check`, group `transition`): one engine hotswaps scene 1120596121 onto 1111948150 with `--transition fade
  --transition-duration 3000` and `LWE_TRANSITION_DUMP=<dir>/tr-half.ppm`; the engine must survive and dump the output as the old
  frame (`tr-half-old.ppm`), the new wallpaper alone (`tr-half-new.ppm`) and the frame halfway through the fade (`tr-half.ppm`),
  and `check_frame.py --blend` on the host requires the halfway picture's mean colour to be 25 to 75% of the way from the old
  to the new one (SKIP when the two have nearly the same average colour). `tools/check_transition.sh <binary>` is the headless
  version (also slide-left), run it with the regression step
- hotswap cycle (`hotswap_cycle_check`): one engine goes through scene (1120596121), video (1367456719), web (1078208425), scene
  (1111948150), web (834237690), video and scene again; every step must keep the process, log the swap, show a picture that is not
  flat or the empty desktop (`shot-cycle-*.png`), have a web host only on web wallpapers and add no crash lines to the log
- desktop icons (`desktop_icons_check.sh`, `ICONS=0` in a profile skips it): files on the desktop folder, the session's own icons
  (Plasma's Folder View, xfdesktop, nemo-desktop, Desktop Icons NG) are located by screenshot (with and without the files),
  then the engine runs and the same area must still show them. A session without an icon provider SKIPs (GNOME without DING,
  Hyprland, COSMIC...), on Plasma that is a FAIL. Plasma on Wayland hides Folder View under the engine's bottom layer
  surface, so the check installs gtk4-layer-shell, PyGObject and pycairo (distro package manager, SKIP where the root is
  read-only) and starts we_manager's `desktop-icons-overlay.py` after the engine, like we_manager does. The Plasma VMs also
  run at other resolutions with `desktop_icons_matrix.sh` (fedora-kde at 1280x800 to 3840x2160 and every other Plasma VM)
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

## Desktop lab (outside the VMs)

Compositors and desktops that have no VM run headless on the build host: `python3 -m pytest tests/desktops` with `LWE_BINARY` set to
the candidate's `linux-wallpaperengine` (sway, labwc, wayfire, river, niri, Hyprland, KDE Plasma 6 on Wayland and on X11 (`kde-x11`),
Plasma 5, GNOME 45 to 50, XFCE, Cinnamon, MATE, LXQt and seven X11 window managers; about 160 tests, 2 to 3 hours, one session at a time).
Run it once per release candidate next to the VM matrix and report failures with it.

## Automation

All of it lives in `tools/linuxvm/`:

- `profiles/<vm>.sh`: how to reach the session (`SCREEN`, `SHOT_CMD`, `FULLSCREEN_CMD`, `MAXIMIZE_CMD`, `WEM`, `WEM_FLAGS`,
  `WEM_LAUNCH`, `PREP`, `BOOT_PREP`, `MULTI`, `POINTER`, `POINTER_DOCK_XY` for a desktop whose own dock covers the pointer spot of the dock check, `LOGIN_PASSWORD` for greeters without autologin). `WEM_LAUNCH` is the prefix of the
  we_manager start line: SteamOS needs `systemd-run --user --scope -q` because a process started from an ssh session dies with it.
- `linuxvm.sh golden <vm>` / `fresh <vm>`: the VM's disk is frozen as `golden.qcow2` and runs on a throwaway overlay, `fresh`
  throws the overlay away. Every VM of the matrix has one.
- `<vm>/vm.env` (sourced by `linuxvm.sh start`): `LINUXVM_SIZE`, `LINUXVM_CPUS`, `LINUXVM_MEM` and `LINUXVM_CPU` (the QEMU `-cpu` model) for that VM alone.
  The `steamdeck` VM was made by copying `steamos`'s golden disk (new port, `qemu-extra`, `vm.env`) and adding
  `/etc/modules-load.d/test_power.conf` + `/etc/modprobe.d/test_power.conf` (AC and USB off, battery discharging) before `golden`.
- `release_check.sh [--vm name]... [--candidate dir] [--app dir|.deb] [--engine-tarball tgz] [--no-overlay]`: loops over the
  profiles (one lane; the gate runs 4 lanes, each its own `--vm` list and `REPORT_DIR`), writes `<REPORT_DIR>/summary.md` (default `reports/<date>/`) with screenshots and logs per VM, retries the
  failed checks once, powers the VM off (ssh `systemctl poweroff`, ACPI does not reach a locked KDE session) and resets the
  overlay. `KEEP_VM=1` leaves the VM running, `REPORT_DIR` keeps parallel runs apart (they would share one summary.md).
- `guest_test.sh` (one PASS/FAIL/SKIP line per check; BEHAVIOR=1 adds hotswap, fullscreen and maximized pause through
  `pause_check`, SIGKILL cleanup), `run_behavior.sh` (the same by hand), `pointer_check.sh` and `hover_check.py` (the pointer
  checks), `host_shots.sh` (screenshots through the VM's display, and a click on the screen centre so a test window gets the
  focus), `check_frame.py` (black/flat/unchanged/hotswap differs, and `--reference` against `reference/<id>.png`: upside down, mirrored or another crop; Pillow, run with `-I`). `make_reference_frames.sh` renders those references headlessly from the payload (video wallpapers have none). `guest_test.sh` also runs a `dpms` group: the engine is started, the display is blanked the way the desktop allows (X11 `xset dpms force off`, Hyprland `hyprctl dispatch dpms off`, sway `swaymsg output * power off`, Plasma `kscreen-doctor --dpms off`, GNOME Mutter's `PowerSaveMode` 3; `DPMS_OFF_CMD` / `DPMS_ON_CMD` override it, desktops with none are skipped) and the log must show `Paused: display off`, then `Resumed` after waking it (`SessionMonitor`). On X11 sessions `guest_test.sh` also runs a `windowbg` group (`--window --window-background` on the output's geometry).
- `clean_install_check.sh` (a first install of the prebuilt engine through we_manager), `wem_check.sh` (we_manager install,
  launch, stop, kill all, autostart entry), `update_check.sh` (`fake_github.py` serving a newer app .deb, `bump_deb.py`, and the
  candidate engine as a newer release, plus an older release of each and a `libnode-arch` helper release: manual check with the
  automatic checks off, close against "don't notify me again", the version picker's list, engine and .deb downgrades), `deps_check.sh` (the "install build dependencies" button on the VM's package manager,
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

## Where the time goes

`guest_test.sh` prints a `TIME <group> <seconds>` line per group and `release_check.sh` writes `<report>/<vm>/timing.txt` (boot,
checks, retry, pointer, icons, wem, extra, shutdown) and a "time" line per VM in `summary.md`. Waits poll the engine's log
instead of sleeping a fixed time: `ready` (the engine draws, then 2 s), `settle` (4 s after the first frame for scenes, for web
pages until "first page content after", then 3 s), the automute check's second phase follows the "Paused" and "stopped" log lines.

A failed group is repeated once, but only what failed: the wallpaper ids of the `wallpapers` group (`WALLPAPERS`) and the swaps of
the hotswap cycle up to the failed step (`CYCLE_UPTO`); the first run's lines the retry did not repeat stay in `checks.txt`, equal
lines count once. `release_check.sh --quick` is for daily runs: `video` and `automute` only run on profiles with `FULL_ENGINE=1`
(fedora-kde, fedora-gnome, arch-hypr and debian-xfce: one per compositor family) and the cycle only does its first 3 swaps
elsewhere; the release candidate gets the full run.

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

- **elementary** (Gala has no wlr-layer-shell): the engine draws through XWayland there. X11 windows pause it through the X server, native Wayland
  windows through Gala's `org.pantheon.gala.DesktopIntegration` D-Bus interface (`Desktop::PantheonShell`: window size, focus,
  hidden and workspace only, so fullscreen = a window at least as big as an output and there is no maximize check); the profile's test window is a native one.
- **Frozen wallpaper on GNOME 46** (Ubuntu, Zorin): Mutter frees a wallpaper window without unmanaging it, and its actor stays in
  the Shell's window group with the last frame (`hot-after` identical to `hot-before`, no engine alive). It turns up in a minority
  of full runs, so a single clean run proves little. A ghost shows as a surplus wallpaper `MetaWindowActor` in the `GROUP` lines of
  the debug extension. The extension hides such orphans (`_hideOrphanActors`), and the golden disks of the GNOME VMs carry the fixed one (`update_golden_extension.sh` refreshes them).
- **Running the matrix in lanes**: the matrix queue runs 4 VMs at a time, with a shared pool of 5 slots (about 5 GB
  resident per VM in 30 GB), so harness re-runs grab any free slot instead of a fixed one: when a VM fails, find out whether the
  harness or the VM is at fault, fix it and re-run that VM at once in a free slot without stopping the queue, never more than 5 VMs
  at once. `desktop_icons_matrix.sh` does this (`LANES`, `MAXVMS`, flock files in /tmp/lwe-vm-slots shared by every invocation);
  `release_check.sh` takes a slot per VM from the same pool (`vm_slots.sh`), its lanes are still separate `--vm` lists. A lane is `release_check.sh --vm <name>` per VM one after the other, each with its own report folder. Under that load a few windows
  turn up later than the 20 s settle in `release_check.sh`: we_manager's autostarted window on a Debian GNOME guest and Steam's sign-in on
  SteamOS/Steam Deck, both are closed once more after the settle. An app that restarts after its own .deb update (MX) can drop the first
  `installPrebuilt()` call, `update_check.sh` asks up to three times.
- **Electron's `EGL_PLATFORM=wayland`** reaches the engine when we_manager starts it in a Wayland session. The X11 driver (XWayland
  fallback on elementary, a web wallpaper as startup background) makes its EGL display under it and crashed in libwayland-client, only
  through we_manager (a shell start had no such variable). The GLFW driver drops the variable before `glfwInit`.
