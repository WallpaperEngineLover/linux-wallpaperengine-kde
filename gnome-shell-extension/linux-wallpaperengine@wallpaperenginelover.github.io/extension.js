// Mutter has no layer-shell: turns the engine's per-output toplevels into desktop windows and reports the pointer
// and per-monitor window states (WE 2.8 rules) a Wayland client can't see.

import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {WorkspaceBackground} from 'resource:///org/gnome/shell/ui/workspace.js';

const APP_ID = 'linux-wallpaperengine';
const TITLE_PREFIX = 'linux-wallpaperengine wallpaper ';
const BUS_NAME = 'org.linuxwallpaperengine.Shell';
const OBJECT_PATH = '/org/linuxwallpaperengine/Shell';
const API_VERSION = 1;

const FLAG_FOCUSED = 1;
const FLAG_MAXIMIZED = 2;
const FLAG_FULLSCREEN = 4;

const INTERFACE = `
<node>
  <interface name="org.linuxwallpaperengine.Shell">
    <property name="Version" type="u" access="read"/>
    <property name="Supported" type="b" access="read"/>
    <property name="NeedsHelper" type="b" access="read"/>
    <method name="SpawnHelper">
      <arg type="s" name="socket" direction="in"/>
    </method>
    <method name="GetState">
      <arg type="a(sdd)" name="pointer" direction="out"/>
      <arg type="a(ssu)" name="windows" direction="out"/>
    </method>
    <signal name="PointerMoved">
      <arg type="a(sdd)" name="pointer"/>
    </signal>
    <signal name="DesktopButton">
      <arg type="b" name="pressed"/>
    </signal>
    <signal name="WindowsChanged">
      <arg type="a(ssu)" name="windows"/>
    </signal>
  </interface>
</node>`;

// Same rounded work-area clip the overview gives workspace backgrounds; bounds and radius are monitor fractions.
const CLIP_DECLARATIONS = `
uniform vec4 bounds;
uniform vec2 radius;
`;

const CLIP_CODE = `
vec2 p = cogl_tex_coord_in[0].xy;
float inside = step(bounds.x, p.x) * step(p.x, bounds.z) * step(bounds.y, p.y) * step(p.y, bounds.w);
if (radius.x > 0.0 && radius.y > 0.0) {
    vec2 corner = clamp(p, bounds.xy + radius, bounds.zw - radius);
    inside *= 1.0 - smoothstep(0.96, 1.0, length((p - corner) / radius));
}
cogl_color_out *= inside;
`;

const OverviewClipEffect = GObject.registerClass(
class OverviewClipEffect extends Shell.GLSLEffect {
    _init(params) {
        super._init(params);
        this._boundsLocation = this.get_uniform_location('bounds');
        this._radiusLocation = this.get_uniform_location('radius');
    }

    vfunc_build_pipeline() {
        // Cogl samples first, the clip only scales the result
        this.add_glsl_snippet(Cogl.SnippetHook.FRAGMENT, CLIP_DECLARATIONS, CLIP_CODE, false);
    }

    setClip(bounds, radius) {
        this.set_uniform_float(this._boundsLocation, 4, bounds);
        this.set_uniform_float(this._radiusLocation, 2, radius);
        this.queue_repaint();
    }
});

// 49+: set_type on any MetaWindow. 46-48: only via MetaWaylandClient for Shell-spawned clients (the helper).
// 45: no Wayland desktop windows, the wallpaper stays lowest in the normal layer like DING.
const CAN_SET_TYPE = typeof Meta.Window.prototype.set_type === 'function';
const CAN_MAKE_DESKTOP = typeof Meta.WaylandClient?.prototype.make_desktop === 'function';
const CAN_HIDE = typeof Meta.WaylandClient?.prototype.hide_from_window_list === 'function';
const SUPPORTED = CAN_SET_TYPE || CAN_HIDE;
const HELPER = 'linux-wallpaperengine-gnome-helper';
// icon extensions using a bottom-layer normal window (DING on Ubuntu 46, its Gtk4 fork)
const DESKTOP_ICON_APPS = ['com.rastersoft.ding', 'com.desktop.ding'];

function isMaximized(window) {
    const flags = window.get_maximize_flags ? window.get_maximize_flags() : window.get_maximized();
    return flags === Meta.MaximizeFlags.BOTH;
}

function cursorTracker() {
    return global.backend.get_cursor_tracker?.() ?? Meta.CursorTracker.get_for_display(global.display);
}


function appIdOf(window) {
    return window.get_sandboxed_app_id() || window.get_gtk_application_id() || window.get_wm_class() || '';
}

export default class WallpaperEngineExtension extends Extension {
    enable() {
        this._wallpapers = new Map();
        this._watched = new Map();
        this._windowsJson = '';
        this._windows = [];
        this._pointerIdle = 0;
        this._stateIdle = 0;

        this._dbus = Gio.DBusExportedObject.wrapJSObject(INTERFACE, this);
        this._dbus.export(Gio.DBus.session, OBJECT_PATH);
        this._nameId = Gio.bus_own_name_on_connection(Gio.DBus.session, BUS_NAME, Gio.BusNameOwnerFlags.NONE,
            null, null);

        this._displaySignals = [
            global.display.connect('window-created', (_display, window) => this._watch(window)),
            global.display.connect('notify::focus-window', () => this._queueState()),
            global.display.connect('restacked', () => {
                this._pruneWallpapers();
                this._keepBelowDesktopWindows();
                this._queueState();
            }),
        ];
        this._workspaceSignal = global.workspace_manager.connect('active-workspace-changed',
            () => this._queueState());
        this._monitorManager = global.backend.get_monitor_manager();
        this._monitorSignal = this._monitorManager.connect('monitors-changed', () => {
            for (const window of this._wallpapers.keys())
                this._place(window);
            this._queueState();
            this._queuePointer();
        });
        this._helperClients = new Set();
        this._raisedIds = new Map();
        this._shown = new Set();
        this._cursorTracker = cursorTracker();
        this._cursorSignal = this._cursorTracker.connect('position-invalidated', () => this._queuePointer());
        this._leftDown = false;
        this._leftForwarded = false;
        this._pointerOverDesktop = false;
        this._paintSignal = global.stage.connect('after-paint', () => this._pollLeftButton());

        this._overviewClones = new Map();
        this._overviewSignal = Main.overview.connect('showing', () => this._syncOverview());
        this._workareaSignal = global.display.connect('workareas-changed', () => this._syncOverview());

        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);

        this._queueState();
    }

    disable() {
        for (const id of [this._pointerIdle, this._stateIdle]) {
            if (id)
                GLib.source_remove(id);
        }
        this._pointerIdle = this._stateIdle = 0;

        Main.overview.disconnect(this._overviewSignal);
        global.display.disconnect(this._workareaSignal);
        for (const background of [...this._overviewClones.keys()])
            this._dropOverviewClone(background);
        this._overviewClones = null;

        this._cursorTracker.disconnect(this._cursorSignal);
        global.stage.disconnect(this._paintSignal);
        this._monitorManager.disconnect(this._monitorSignal);
        global.workspace_manager.disconnect(this._workspaceSignal);
        for (const id of this._displaySignals)
            global.display.disconnect(id);

        for (const [window, ids] of this._watched) {
            for (const id of ids)
                window.disconnect(id);
        }
        this._watched.clear();

        // otherwise they would be plain windows covering the monitor
        for (const window of this._wallpapers.keys())
            window.delete(global.display.get_current_time_roundtrip());
        this._wallpapers.clear();

        Gio.bus_unown_name(this._nameId);
        this._dbus.unexport();
        this._dbus = null;
        this._cursorTracker = this._monitorManager = null;
        for (const [window, id] of this._raisedIds)
            window.disconnect(id);
        this._helperClients = null;
        this._raisedIds = null;
        this._shown = null;
    }

    get Version() {
        return API_VERSION;
    }

    get Supported() {
        return SUPPORTED;
    }

    get NeedsHelper() {
        return !CAN_SET_TYPE && CAN_HIDE;
    }

    // Only ever runs our own helper; callers just pick an engine socket in the runtime folder (Flatpak calls come
    // through xdg-dbus-proxy, so the caller's pid is meaningless)
    SpawnHelperAsync([socket], invocation) {
        try {
            if (!this.NeedsHelper)
                throw new Error('this GNOME Shell makes desktop windows without the helper');

            const runtime = `${GLib.get_user_runtime_dir()}/`;
            const socketName = socket.startsWith(runtime) ? socket.slice(runtime.length) : '';
            if (!/^(app\/[^/]+\/)?lwe-gnome-\d+\.sock$/.test(socketName))
                throw new Error(`refusing a helper for ${socket}`);

            const helper = GLib.build_filenamev([this.path, HELPER]);

            const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.NONE});
            const client = Meta.WaylandClient.new(global.context, launcher);
            const process = client.spawnv(global.display, [helper, socket]);
            this._helperClients.add(client);
            process.wait_async(null, () => this._helperClients?.delete(client));
            invocation.return_value(null);
        } catch (error) {
            invocation.return_error_literal(Gio.DBusError, Gio.DBusError.ACCESS_DENIED, error.message);
        }
    }

    GetState() {
        return [this._pointer(), this._windows];
    }

    // fractions from the top left, stage and surface coords differ without scale-monitor-framebuffer
    _pointer() {
        const [x, y] = global.get_pointer();
        const result = [];

        for (const [window, connector] of this._wallpapers) {
            const frame = window.get_frame_rect();
            if (frame.width > 0 && frame.height > 0)
                result.push([connector, (x - frame.x) / frame.width, (y - frame.y) / frame.height]);
        }

        return result;
    }

    _watch(window) {
        if (this._watched.has(window))
            return;

        // title and app id may come after the window exists
        const check = () => {
            if (!this._wallpapers.has(window) && this._isWallpaper(window))
                this._adopt(window);
            this._queueState();
        };
        const ids = [
            window.connect('notify::wm-class', check),
            window.connect('notify::title', check),
            window.connect('notify::minimized', () => this._queueState()),
            window.connect('notify::maximized-horizontally', () => this._queueState()),
            window.connect('notify::maximized-vertically', () => this._queueState()),
            window.connect('notify::fullscreen', () => this._queueState()),
            window.connect('size-changed', () => {
                this._queueState();
                if (this._wallpapers.has(window))
                    this._queuePointer();
            }),
            window.connect('workspace-changed', () => this._queueState()),
            window.connect('unmanaged', () => {
                for (const id of this._watched.get(window) ?? [])
                    window.disconnect(id);
                this._watched.delete(window);
                this._raisedIds?.delete(window);
                this._shown?.delete(window);
                if (this._wallpapers.delete(window))
                    this._syncOverview();
                this._queueState();
            }),
        ];
        this._watched.set(window, ids);
        check();
    }

    _isWallpaper(window) {
        return window.get_client_type() === Meta.WindowClientType.WAYLAND &&
            window.get_wm_class() === APP_ID &&
            (window.get_title() ?? '').startsWith(TITLE_PREFIX);
    }

    _adopt(window) {
        const connector = window.get_title().slice(TITLE_PREFIX.length);
        this._wallpapers.set(window, connector);

        // not fullscreen on the engine side (would hide the top bar), the monitor size comes from here
        if (CAN_SET_TYPE) {
            window.set_type(Meta.WindowType.DESKTOP);
            window.hide_from_window_list();
        } else {
            const client = [...this._helperClients].find(candidate => candidate.owns_window(window));
            if (!client) {
                console.error(`${this.uuid}: ${window.get_title()} is not from a helper connection, it stays a window`);
                return;
            }
            if (CAN_MAKE_DESKTOP) {
                client.make_desktop(window);
            } else {
                this._raisedIds.set(window, window.connect_after('raised', () => window.lower()));
                this._returnFocus(window, global.display.focus_window);
            }
            client.hide_from_window_list(window);
        }
        window.stick();
        // again when shown, Mutter places new windows on the current monitor
        this._place(window);
        const shownId = window.connect('shown', () => {
            window.disconnect(shownId);
            this._shown.add(window);
            this._place(window);
            this._keepBelowDesktopWindows();
        });
        this._queuePointer();
        this._syncOverview();
    }

    // The overview hides the window group, so a clone over each workspace background keeps it live
    _syncOverview() {
        if (!this._overviewClones)
            return;

        const found = new Set();
        const walk = actor => {
            if (actor instanceof WorkspaceBackground) {
                found.add(actor);
                this._syncWorkspaceBackground(actor);
                return;
            }
            for (const child of actor.get_children())
                walk(child);
        };
        walk(Main.layoutManager.overviewGroup);

        for (const background of [...this._overviewClones.keys()]) {
            if (!found.has(background))
                this._dropOverviewClone(background);
        }
    }

    _wallpaperActorOn(monitorIndex) {
        for (const window of this._wallpapers.keys()) {
            if (window.get_monitor() === monitorIndex && window.get_compositor_private())
                return window.get_compositor_private();
        }
        return null;
    }

    _syncWorkspaceBackground(background) {
        const source = SUPPORTED ? this._wallpaperActorOn(background._monitorIndex) : null;
        const existing = this._overviewClones.get(background);

        if (existing?.clone.source === source && source) {
            this._updateOverviewClip(background, existing);
            return;
        }
        if (existing)
            this._dropOverviewClone(background);
        if (!source)
            return;

        const clone = new Clutter.Clone({source, x_expand: true, y_expand: true});
        const effect = new OverviewClipEffect();
        clone.add_effect(effect);

        const group = background._backgroundGroup;
        group.add_child(clone);
        group.set_child_above_sibling(clone, null);

        const entry = {clone, effect, group, signals: [], content: null, contentSignal: 0};
        const manager = background._bgManager;
        // new background actors go on top
        entry.signals.push([manager, manager.connect('changed', () => {
            group.set_child_above_sibling(clone, null);
            this._updateOverviewClip(background, entry);
        })]);
        entry.signals.push([background, background.connect('destroy', () => this._dropOverviewClone(background, true))]);

        this._overviewClones.set(background, entry);
        this._updateOverviewClip(background, entry);
    }

    _updateOverviewClip(background, entry) {
        const content = background._bgManager?.backgroundActor?.content;
        if (!content)
            return;

        if (entry.content !== content) {
            if (entry.contentSignal)
                entry.content.disconnect(entry.contentSignal);
            entry.content = content;
            entry.contentSignal = content.connect('notify::rounded-clip-radius',
                () => this._updateOverviewClip(background, entry));
        }

        const monitor = Main.layoutManager.monitors[background._monitorIndex];
        const area = background._workarea;
        if (!monitor || !area || monitor.width <= 0 || monitor.height <= 0)
            return;

        const left = (area.x - monitor.x) / monitor.width;
        const top = (area.y - monitor.y) / monitor.height;
        const radius = content.rounded_clip_radius;
        entry.effect.setClip(
            [left, top, left + area.width / monitor.width, top + area.height / monitor.height],
            [radius / monitor.width, radius / monitor.height]);
    }

    // the workspace went away with clone and background
    _dropOverviewClone(background, destroyed = false) {
        const entry = this._overviewClones?.get(background);
        if (!entry)
            return;

        this._overviewClones.delete(background);
        if (destroyed)
            return;

        for (const [object, id] of entry.signals)
            object.disconnect(id);
        if (entry.contentSignal)
            entry.content.disconnect(entry.contentSignal);
        entry.clone.destroy();
    }

    // DING shares the wallpaper's layer and later windows land on top; keep the wallpaper lowest, under the icons
    _keepBelowDesktopWindows() {
        const stack = global.display.sort_windows_by_stacking(global.display.list_all_windows());

        for (const wallpaper of this._wallpapers.keys()) {
            const layer = wallpaper.get_layer();
            const index = stack.indexOf(wallpaper);
            // lowering re-emits restacked
            if (stack.slice(0, Math.max(index, 0))
                .some(window => window.get_layer() === layer && !this._wallpapers.has(window)))
                wallpaper.lower();
        }
    }

    // GNOME 45 drops a gone client's window without 'unmanaged' (Shell 45.2)
    _pruneWallpapers() {
        const live = new Set(global.display.list_all_windows());

        for (const window of [...this._wallpapers.keys()]) {
            if (!this._shown.has(window) || live.has(window))
                continue;

            for (const id of this._watched.get(window) ?? [])
                window.disconnect(id);
            this._watched.delete(window);
            this._wallpapers.delete(window);
            this._shown.delete(window);
            this._raisedIds.delete(window);
            this._syncOverview();
        }
    }

    _returnFocus(window, previous) {
        const shownId = window.connect('shown', () => {
            window.disconnect(shownId);
            if (window.has_focus() && previous && previous !== window && !previous.minimized)
                previous.activate(global.display.get_current_time_roundtrip());
        });
    }

    _place(window) {
        const connector = this._wallpapers.get(window);
        const monitor = this._monitorManager.get_monitor_for_connector(connector);
        if (monitor < 0)
            return;

        const area = global.display.get_monitor_geometry(monitor);
        window.move_resize_frame(false, area.x, area.y, area.width, area.height);
    }

    // Clicks on desktop icons never reach our surface, but WE polls the left button each frame (sub_14010D9B0) and
    // counts a press over the icon view. Stage events miss Wayland client input, so poll once per painted frame too.
    _pollLeftButton() {
        if (this._wallpapers.size === 0)
            return;

        const [, , mods] = global.get_pointer();
        const down = (mods & Clutter.ModifierType.BUTTON1_MASK) !== 0;
        if (down === this._leftDown) {
            if (!down)
                this._updatePointerOverDesktop();
            return;
        }

        this._leftDown = down;

        // pick before the press, icons react to it (DING starts a rubber band)
        if (down && !Main.overview.visible && this._pointerOverDesktop) {
            this._leftForwarded = true;
            this._dbus?.emit_signal('DesktopButton', new GLib.Variant('(b)', [true]));
        } else if (!down && this._leftForwarded) {
            this._leftForwarded = false;
            this._dbus?.emit_signal('DesktopButton', new GLib.Variant('(b)', [false]));
        }
    }

    _updatePointerOverDesktop() {
        const [x, y] = global.get_pointer();
        this._pointerOverDesktop = this._overOtherDesktopWindow(x, y);
    }

    _overOtherDesktopWindow(x, y) {
        let actor = global.stage.get_actor_at_pos(Clutter.PickMode.REACTIVE, x, y);
        while (actor && !(actor instanceof Meta.WindowActor))
            actor = actor.get_parent();

        const window = actor?.get_meta_window();
        return !!window && this._isDesktop(window);
    }

    // other desktop windows and icon windows
    _isDesktop(window) {
        if (this._wallpapers.has(window))
            return false;
        return window.get_window_type() === Meta.WindowType.DESKTOP ||
            DESKTOP_ICON_APPS.includes(window.get_gtk_application_id());
    }

    _queuePointer() {
        if (this._pointerIdle || this._wallpapers.size === 0)
            return;

        // one signal per main loop pass
        this._pointerIdle = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._pointerIdle = 0;
            this._dbus?.emit_signal('PointerMoved', new GLib.Variant('(a(sdd))', [this._pointer()]));
            return GLib.SOURCE_REMOVE;
        });
    }

    _queueState() {
        if (this._stateIdle)
            return;

        this._stateIdle = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._stateIdle = 0;
            this._updateState();
            return GLib.SOURCE_REMOVE;
        });
    }

    // GNOME 46 can't list logical monitors, only outputs with a wallpaper are reported
    _connectorsOf(monitorIndex) {
        return [...new Set(this._wallpapers.values())]
            .filter(connector => this._monitorManager.get_monitor_for_connector(connector) === monitorIndex);
    }

    // WE 2.8 (sub_14003D050 / sub_14003BBA0): visible, unminimized windows on the current desktop. Maximized marks its
    // monitor; the foreground root window marks focused, and fullscreen if not maximized and at least monitor-sized.
    _collectWindows() {
        const workspace = global.workspace_manager.get_active_workspace();
        const focus = global.display.focus_window?.find_root_ancestor() ?? null;
        const windows = [];

        for (const actor of global.get_window_actors()) {
            const window = actor.meta_window;
            if (this._wallpapers.has(window) || window.minimized || !window.showing_on_its_workspace() ||
                !window.located_on_workspace(workspace) || window.is_override_redirect())
                continue;

            if (window.get_window_type() === Meta.WindowType.DOCK || this._isDesktop(window))
                continue;

            const maximized = isMaximized(window);
            let flags = maximized ? FLAG_MAXIMIZED : 0;

            if (window === focus) {
                flags |= FLAG_FOCUSED;
                const frame = window.get_frame_rect();
                const area = global.display.get_monitor_geometry(window.get_monitor());
                if (!maximized && (window.is_fullscreen() || (frame.width >= area.width && frame.height >= area.height)))
                    flags |= FLAG_FULLSCREEN;
            }

            if (flags === 0)
                continue;

            for (const connector of this._connectorsOf(window.get_monitor()))
                windows.push([appIdOf(window), connector, flags]);
        }

        return windows;
    }

    _updateState() {
        const windows = this._collectWindows();
        const json = JSON.stringify(windows);
        if (json === this._windowsJson)
            return;

        this._windowsJson = json;
        this._windows = windows;
        this._dbus?.emit_signal('WindowsChanged', new GLib.Variant('(a(ssu))', [windows]));
    }
}
