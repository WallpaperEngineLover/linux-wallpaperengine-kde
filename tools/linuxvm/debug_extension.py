#!/usr/bin/env python3
"""debug_extension.py <out dir> [extension.js] - writes <out dir>/extension.js, the GNOME Shell extension with LWE-DBG
journal lines added: every wallpaper window's adopt, unmanaging, unmanaged, wm map/destroy, restacked, lower, the pruning of
windows that vanished and the hiding of orphan actors, plus a line whenever the wallpaper actors or the children of the
Shell's window group change. Read them with `journalctl -b -o short-monotonic | grep LWE-DBG` (ghost_run.sh does)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, '..', '..', 'gnome-shell-extension',
                       'linux-wallpaperengine@wallpaperenginelover.github.io', 'extension.js')


def replace(source, old, new):
    if source.count(old) != 1:
        sys.exit(f"anchor not found exactly once in the extension: {old.strip()[:60]!r}")
    return source.replace(old, new)


HELPERS = '''function dbg(message) {
    console.log(`LWE-DBG ${message}`);
}

function wdesc(window) {
    try {
        return `#${window.get_stable_sequence()} "${window.get_title()}" type=${window.get_window_type()} layer=${window.get_layer()} mon=${window.get_monitor()} actor=${window.get_compositor_private() ? 1 : 0}`;
    } catch (e) {
        return `(window gone: ${e.message})`;
    }
}

function appIdOf(window) {'''

DUMP = '''        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);

        this._dbgLast = '';
        this._dbgTimer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 2, () => {
            const live = new Set(global.display.list_all_windows());
            const lines = global.get_window_actors().map(actor => {
                const w = actor.meta_window;
                return `${wdesc(w)} vis=${actor.visible ? 1 : 0} mapped=${actor.mapped ? 1 : 0} inlist=${live.has(w) ? 1 : 0} ours=${this._wallpapers.has(w) ? 1 : 0} shown=${this._shown.has(w) ? 1 : 0}`;
            }).filter(line => line.includes('wallpaper') || line.includes('linux-wallpaperengine'));
            const text = lines.join(' | ') || '(no wallpaper actors)';
            if (text !== this._dbgLast) {
                this._dbgLast = text;
                dbg(`ACTORS ${text}`);
            }
            const group = global.window_group.get_children().map(c => {
                let title = '';
                try { title = c.meta_window ? c.meta_window.get_title() : '-'; } catch (e) { title = '?'; }
                return `${c.constructor.name}[${title}|v${c.visible ? 1 : 0}|m${c.mapped ? 1 : 0}|${Math.round(c.width)}x${Math.round(c.height)}|o${c.opacity}]`;
            }).join(' ');
            if (group !== this._dbgGroup) {
                this._dbgGroup = group;
                dbg(`GROUP ${group}`);
            }
            return GLib.SOURCE_CONTINUE;
        });
        this._dbgWm = [
            global.window_manager.connect('map', (_wm, actor) => dbg(`wm map ${wdesc(actor.meta_window)}`)),
            global.window_manager.connect('destroy', (_wm, actor) => dbg(`wm destroy ${wdesc(actor.meta_window)}`)),
        ];
        dbg('enabled');

        this._queueState();
    }
'''

PLAIN_DUMP = '''        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);

        this._queueState();
    }
'''


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT
    source = open(path).read()

    source = replace(source, 'function appIdOf(window) {', HELPERS)
    source = replace(source, PLAIN_DUMP, DUMP)
    source = replace(source, '    disable() {\n', '''    disable() {
        dbg('disable');
        if (this._dbgTimer)
            GLib.source_remove(this._dbgTimer);
        for (const id of this._dbgWm ?? [])
            global.window_manager.disconnect(id);
''')
    source = replace(source, "            window.connect('unmanaged', () => {\n", '''            window.connect('unmanaging', () => dbg(`unmanaging ${wdesc(window)} ours=${this._wallpapers.has(window) ? 1 : 0}`)),
            window.connect('unmanaged', () => {
                dbg(`unmanaged ours=${this._wallpapers.has(window) ? 1 : 0}`);
''')
    source = replace(source, '    _adopt(window) {\n', '    _adopt(window) {\n        dbg(`adopt ${wdesc(window)}`);\n')
    source = replace(source, '                wallpaper.lower();\n', '''            {
                dbg(`lower ${wdesc(wallpaper)} index=${index} of ${stack.length}`);
                wallpaper.lower();
            }
''')
    source = replace(source, '''            for (const id of this._watched.get(window) ?? [])
                window.disconnect(id);
            this._watched.delete(window);
            this._wallpapers.delete(window);
            this._shown.delete(window);
            this._raisedIds.delete(window);
            this._syncOverview();''', '''            dbg(`PRUNE drops ${wdesc(window)} (not in list_all_windows, shown)`);
            for (const id of this._watched.get(window) ?? [])
                window.disconnect(id);
            this._watched.delete(window);
            this._wallpapers.delete(window);
            this._shown.delete(window);
            this._raisedIds.delete(window);
            this._syncOverview();''')
    source = replace(source, "            global.display.connect('restacked', () => {\n", '''            global.display.connect('restacked', () => {
                dbg(`restacked wallpapers=${[...this._wallpapers.keys()].map(w => '#' + w.get_stable_sequence()).join(',')}`);
''')
    if '            actor.hide();\n' in source:
        source = replace(source, '            actor.hide();\n', '            dbg(`hiding orphan ${wdesc(window)}`);\n            actor.hide();\n')

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'extension.js'), 'w') as f:
        f.write(source)


main()
