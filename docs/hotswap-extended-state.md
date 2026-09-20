# Task: extend hotswap to cover layers, volume, and other live settings

## Context

`--list-objects` / `--disable-object` / `--enable-object` were recently added
(see `ApplicationContext.cpp`), and the we_manager Electron app now has a UI
for toggling layers and adjusting per-wallpaper volume live from a sidebar.

Because those settings are only read from argv at process startup
(`ApplicationContext::loadSettingsFromArgv`), the app currently applies any
change by killing the running process and spawning a brand new one with the
updated flags. That works, but it's the wrong tool for the job:

- Full CEF/GL/DBus teardown and reinit on every toggle, visible as a flash.
- A real bug we hit and fixed on the app side: `stopLwe()` sends SIGTERM and
  moves on without waiting for the old process to actually exit. If it's
  still shutting down when the new one starts, both processes are briefly
  alive at once (we saw `DBus service org.linuxwallpaperengine.WaylandDetector
  is already owned by another process` in the logs from exactly this). The
  app now guards against the worst symptom (a corrupted `activeProcess`
  reference that made the new launch hang forever - see
  `we_manager/src/main/services/lwe.service.ts`, the `cleanup` closure in
  `launchLweAsync`), but the underlying process-overlap window is still
  there and still wasteful.
- Every toggle costs ~2-3 seconds end to end for something that should be
  near-instant.

The hotswap mechanism (SIGUSR1 + control file) already exists for switching
which background plays, without a process restart. This task is to extend
it to carry more than just a path, so layers/volume/etc. can be updated the
same lightweight way.

## Current hotswap mechanism

- `WallpaperApplication::checkHotswapRequest()` in
  `src/WallpaperEngine/Application/WallpaperApplication.cpp` (~line 506).
- Reads a single line (the new background path) from
  `$XDG_RUNTIME_DIR/lwe-control` (or `/tmp/lwe-control`), triggered by
  SIGUSR1 (`m_hotswapRequested` flag, set in `WallpaperApplication::signal`).
- Fully reloads the project (`loadBackground`), re-runs
  `setupPropertiesForProject`, and reconstructs a `CWallpaper` per screen via
  `CWallpaper::fromWallpaper(...)`.
- On the app side, this is driven by `hotReloadLwe()` in
  `we_manager/src/main/services/lwe.service.ts`, which just writes the path
  and sends the signal.

## Why this should be less work than it sounds

Both target settings already re-evaluate fresh on every reload, they're just
not reachable from the control-file path yet:

- **Layers**: `ApplicationContext::resolveObjectVisibility(id, name)`
  (`ApplicationContext.cpp:261`) reads straight from
  `settings.general.disabledObjects` / `enabledObjects`, and is called during
  object construction (`CImage.cpp:865,929`, `CText.cpp:592`,
  `CParticle.cpp:178`, `CScene.cpp:366`) - which already runs again on every
  hotswap reload. If the control-file handler updates those two vectors on
  `m_context.settings.general` *before* calling `loadBackground`, visibility
  should just work without touching the object-construction code at all.

- **Volume**: `GLPlayer::setVolume(double)`
  (`VideoPlayback/MPV/GLPlayer.cpp:102`) already does a live
  `mpv_set_property(..., "volume", ...)` on the existing mpv handle - no
  reload needed at all for video wallpapers, it just needs a path from the
  control file to this call. Scene/particle audio (`Render/Objects/CSound.h`)
  wasn't checked in detail; confirm whether it has an equivalent live setter
  or needs the same "re-apply on reload" treatment as layers.

## Proposed design

1. Replace the single-line control file with a small line-based or JSON
   format so it can carry multiple fields without breaking existing readers,
   e.g.:
   ```
   path=/path/to/wallpaper
   disable-object=13
   disable-object=110
   enable-object=7
   volume=42
   ```
   or a JSON object with the same fields. Either is fine; pick whichever is
   less code given `checkHotswapRequest` is currently a simple
   `std::getline`.

2. In `checkHotswapRequest`, before calling `loadBackground`:
   - If object fields are present, replace
     `m_context.settings.general.disabledObjects` /
     `enabledObjects` with the new lists.
   - If a volume field is present and the current wallpaper is a video,
     route it to `GLPlayer::setVolume` directly (no reload needed - this can
     probably be handled as its own signal path, separate from a full
     background reload, since it doesn't need `loadBackground` at all).

3. Path stays optional in the payload: a volume-only or layers-only update
   shouldn't require re-sending the current path and doing a full reload
   when nothing about the *project* changed. Worth splitting into two
   distinct requests internally: "reload background" (path/layers, since
   layers currently only take effect through object construction) vs.
   "live property push" (volume, and anything else with a direct setter).

## Acceptance criteria

- Toggling a layer on a running wallpaper no longer restarts the process
  (no new PID, no DBus/CEF re-init log lines).
- Changing volume on a running video wallpaper is audible within one mpv
  property update, no visual interruption.
- Existing plain path-only hotswap (playlist advance, "Play wallpaper" on a
  different item) keeps working unchanged.
- Rapid repeated toggles (stress case: 10 layer toggles in under 2 seconds)
  don't crash, hang, or leak processes.

## Follow-up work in we_manager (once this lands)

- `src/main/services/lwe.service.ts`: extend `getControlFilePath`/
  `hotReloadLwe` to write the richer payload, and add a variant that doesn't
  require `isLweRunning()` to be false (currently only used as a fallback
  inside `launchLweAsync`).
- `src/renderer/src/components/common/DetailSidebar.tsx`: `toggleObject` and
  `commitVolume` currently call `lwe.stop()` + `wallpaper.apply()` when
  `isActive` is true - swap that for a new lighter IPC call (e.g.
  `lwe.hotswapSettings(...)`) once the above exists, dropping the ~2-3s
  relaunch wait and the "live" restart flash entirely.

## Non-goals

- fps: changing FPS live is a bigger change (render loop timing), not
  covered here - keep it on the restart path unless someone wants to take
  that on separately.
- Web wallpapers: CEF browser reload semantics are different enough from
  scene/video that they're out of scope for this task.
