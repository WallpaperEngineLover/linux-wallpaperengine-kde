# shellcheck shell=bash disable=SC2154
# wem_lib.sh - shared setup of the we_manager checks (sourced by video_optimize_check.sh, desktop_icons_auto_check.sh,
# wem_check.sh and desktop_icons_wem_check.sh, which define HERE, vm, app, sshv, pass and fail).
# wem_install puts the app into the guest (a .deb is installed, an unpacked dir is copied unless the guest already has this very
# build) and sets $wem_start_line the way the other we_manager scripts build it, with WEM_LAUNCH and WEM_FLAGS. wem_run starts
# the app (after the previous instance is gone) and waits for its DevTools port. cdp <js> evaluates JavaScript in the page (CDP_TIMEOUT seconds, default 90).

cdp() { sshv ". ~/sessenv; cd ~/wem; timeout ${CDP_TIMEOUT:-90} python3 cdp.py \"$1\" 2>&1 | cut -c1-600"; }

wem_copy_app() {
    local name stamp
    name=$(basename "$app")
    stamp=$(find "$app" -type f -printf '%P %s %T@\n' | sort | sha256sum | cut -c1-16)
    if [ "$(sshv "cat ~/wem/.$name.stamp 2>/dev/null; test -x ~/wem/$name/we-manager || echo gone")" = "$stamp" ]; then return 0; fi
    tar -C "$(dirname "$app")" -cf - "$name" | ssh -i "$HOME/.local/share/linuxvm/id_ed25519" \
        -p "$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR lab@127.0.0.1 "mkdir -p ~/wem && rm -f ~/wem/.$name.stamp && rm -rf ~/wem/$name && tar -C ~/wem -xf - && echo $stamp > ~/wem/.$name.stamp"
    local rc=("${PIPESTATUS[@]}")
    [ "${rc[0]}" = 0 ] && [ "${rc[1]}" = 0 ]
}

wem_install() {
    sshv 'mkdir -p ~/wem'
    "$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
    if [[ $app == *.deb ]]; then
        "$HERE/linuxvm.sh" scp "$vm" "$app" wem/app.deb
        if ! sshv 'sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 install -y --allow-downgrades ./wem/app.deb >/tmp/wem-deb.log 2>&1'; then
            fail "the .deb did not install: $(sshv 'tail -3 /tmp/wem-deb.log')"
            return 1
        fi
        pass "the .deb installs"
        wem_start_line="${WEM_LAUNCH:-} we-manager ${WEM_FLAGS:-}"
    else
        if ! wem_copy_app; then fail "could not copy the app into the guest"; return 1; fi
        wem_start_line="${WEM_LAUNCH:-} ~/wem/$(basename "$app")/we-manager --no-sandbox ${WEM_FLAGS:-}"
    fi
}

# wem_steam_lib [workshop id]... - a Steam library like a real user has: the Wallpaper Engine assets and the workshop items
# (copies, the library scan skips symlinked folders)
wem_steam_lib() {
    local id
    sshv 'mkdir -p ~/.steam/steam/steamapps/common/wallpaper_engine ~/.steam/steam/steamapps/workshop/content/431960
ln -sfn ~/lwe/assets ~/.steam/steam/steamapps/common/wallpaper_engine/assets'
    for id in "$@"; do
        sshv "d=~/.steam/steam/steamapps/workshop/content/431960/$id; [ -L \$d ] && rm \$d; [ -d \$d ] || cp -al ~/lwe/wp-$id \$d"
    done
}

wem_engine_fallback() {
    sshv 'mkdir -p ~/.local/share; [ -x ~/.local/share/linux-wallpaperengine-kde/linux-wallpaperengine ] || cp -a ~/lwe/lwe-hints ~/.local/share/linux-wallpaperengine-kde'
}

wem_wait_gone() {
    sshv 'for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; true'
}

wem_ffmpeg_cache=${XDG_CACHE_HOME:-$HOME/.cache}/lwe-release-prep/we-manager-ffmpeg

wem_ffmpeg_seed() {
    [ -x "$wem_ffmpeg_cache/ffmpeg" ] && [ -x "$wem_ffmpeg_cache/ffprobe" ] || return 0
    sshv '. ~/sessenv; mkdir -p "${XDG_DATA_HOME:-$HOME/.local/share}/we-manager"'
    local dir
    dir=$(sshv '. ~/sessenv; echo "${XDG_DATA_HOME:-$HOME/.local/share}/we-manager"')
    # scp -r into an existing directory nests the copy
    sshv "test -x $dir/ffmpeg/ffmpeg" && return 0
    "$HERE/linuxvm.sh" scp "$vm" "$wem_ffmpeg_cache" "$dir/ffmpeg" >/dev/null 2>&1
}

wem_ffmpeg_keep() {
    local dir
    dir=$(sshv '. ~/sessenv; echo "${XDG_DATA_HOME:-$HOME/.local/share}/we-manager/ffmpeg"')
    [ -x "$wem_ffmpeg_cache/ffmpeg" ] && return 0
    sshv "test -x $dir/ffmpeg && test -x $dir/ffprobe" || return 0
    mkdir -p "$wem_ffmpeg_cache"
    sshv "tar -C $dir -cf - ffmpeg ffprobe" | tar -C "$wem_ffmpeg_cache" -xf -
}

# wem_ffmpeg_fake_start <work dir> - serves the cached ffmpeg and ffprobe as the latest BtbN/FFmpeg-Builds release from fake_github.py
# (like update_check.sh does for the app's own releases), so the app's download in a guest without an encoder comes from the host and
# a 504 from GitHub no longer fails the check. Returns 1 without a cache (the real download is used then); sets $fake, $fgh_port.
# The app has to be started after this with NODE_EXTRA_CA_CERTS=$HOME/wem/fake-ca.pem. wem_ffmpeg_fake_stop undoes the guest changes.
wem_ffmpeg_fake_start() {
    local work=$1 name=ffmpeg-n7.1-latest-linux64-gpl-7.1 pack
    [ -x "$wem_ffmpeg_cache/ffmpeg" ] && [ -x "$wem_ffmpeg_cache/ffprobe" ] || return 1
    pack=$(dirname "$wem_ffmpeg_cache")/ffmpeg-fake/$name.tar.xz
    if [ ! -s "$pack" ]; then
        mkdir -p "$(dirname "$pack")"
        tar -C "$wem_ffmpeg_cache" --transform "s,^,$name/bin/," -cf - ffmpeg ffprobe | xz -0 -T4 > "$pack.part" && mv "$pack.part" "$pack" || return 1
    fi
    mkdir -p "$work/certs"
    . "$HERE/fake_github_lib.sh"
    fgh_start "$vm" "$work" --release "BtbN/FFmpeg-Builds=latest:$pack" || return 1
    kill -0 "$fake" 2>/dev/null || return 1
    "$HERE/linuxvm.sh" scp "$vm" "$work/certs/ca.pem" wem/fake-ca.pem
    sshv 'grep -q "api.github.com" /etc/hosts || echo "10.0.2.2 api.github.com" | sudo tee -a /etc/hosts >/dev/null'
}

wem_ffmpeg_fake_stop() {
    kill "$fake" 2>/dev/null
    fgh_unredirect
    sshv "sudo sed -i '/api.github.com/d' /etc/hosts; true"
}

wem_layer_shell_install() {
    sshv 'if command -v dnf >/dev/null; then sudo dnf install -y gtk4-layer-shell python3-gobject python3-cairo
          elif command -v pacman >/dev/null; then sudo pacman -Sy --noconfirm --needed gtk4-layer-shell python-gobject python-cairo
          elif command -v apt-get >/dev/null; then sudo apt-get install -y gir1.2-gtk4layershell-1.0 libgtk4-layer-shell0 python3-gi python3-gi-cairo gir1.2-gtk-4.0
          elif command -v zypper >/dev/null; then sudo zypper -n update libglib-2_0-0 libgio-2_0-0 libgobject-2_0-0 libgmodule-2_0-0 libgthread-2_0-0 glib2-tools; sudo zypper -n install libgtk4-layer-shell0 typelib-1_0-Gtk4LayerShell-1_0 python3-gobject python3-cairo typelib-1_0-Gtk-4_0
          fi'
}

wem_run() {
    wem_wait_gone
    sshv ". ~/sessenv; (setsid nohup $wem_start_line --remote-debugging-port=9222 > ~/wem/app.log 2>&1 </dev/null &); for _ in \$(seq 40); do grep -q 'DevTools listening' ~/wem/app.log 2>/dev/null && break; sleep 1; done; sleep 8"
    if [ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] && sshv "grep -q 'DevTools listening' ~/wem/app.log"; then return 0; fi
    fail "we_manager did not start: $(sshv 'tail -3 ~/wem/app.log')"
    return 1
}
