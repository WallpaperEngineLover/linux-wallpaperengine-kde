#!/bin/bash
# usage: span_display.sh start <WxH+X+Y> [<WxH+X+Y> ...] | layout <WxH+X+Y> ... | stop | env
# a private multi monitor display for live WE: headless sway (one output per argument, 60 Hz, at that position) with
# its own Xwayland as an X window manager, so Wine sees one RandR monitor per output and its windows land where WE
# puts them. sway runs under proot with a private /tmp: Xwayland's socket directory is read-only here and display
# numbers below 10 are taken by lock files, so it never touches :0. `env` prints DISPLAY, WAYLAND_DISPLAY and
# XDG_RUNTIME_DIR for grim/swaymsg/Wine.
S=${WE_SPAN_DIR:-$HOME/.local/share/we_live/span}
R=$S/display
T=$R/tmp

swaymsg_() { XDG_RUNTIME_DIR=$R SWAYSOCK=$(ls $R/sway-ipc.*.sock 2>/dev/null | head -1) swaymsg "$@"; }

layout() {
    local i=1 spec
    for spec in "$@"; do
        local mode=${spec%%+*} pos=${spec#*+}
        swaymsg_ output HEADLESS-$i enable mode ${mode}@60Hz pos ${pos%+*} ${pos#*+} >/dev/null || return 1
        i=$((i + 1))
    done
    while swaymsg_ -t get_outputs -r | grep -q "\"HEADLESS-$i\""; do
        swaymsg_ output HEADLESS-$i disable >/dev/null
        i=$((i + 1))
    done
}

display_number() {
    ls $T/.X11-unix 2>/dev/null | sed -n 's/^X//p' | head -1
}

case $1 in
start)
    shift
    if ! swaymsg_ -t get_version >/dev/null 2>&1; then
        mkdir -p $R && chmod 700 $R
        rm -rf $T && mkdir -p $T/.X11-unix && chmod 1777 $T $T/.X11-unix
        for i in $(seq 0 9); do printf '%10d\n' 1 > $T/.X$i-lock; done
        # no_new_privs refuses to exec Fedora's sway (file capabilities), a copy has none
        mkdir -p $S/bin && cp -u /usr/bin/sway $S/bin/sway
        printf 'xwayland force\ndefault_border none\nswaybg_command -\nfocus_follows_mouse no\n' > $R/sway.conf
        printf 'for_window [class=".*"] floating enable\nseat * hide_cursor 0\n' >> $R/sway.conf
        env -i PATH=$PATH HOME=$HOME XDG_RUNTIME_DIR=$R WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=$# \
            WLR_LIBINPUT_NO_DEVICES=1 setsid nohup proot -b $T:/tmp $S/bin/sway -c $R/sway.conf > $R/sway.log 2>&1 &
        for i in $(seq 50); do [ -n "$(display_number)" ] && swaymsg_ -t get_version >/dev/null 2>&1 && break; sleep 0.2; done
    fi
    layout "$@" || exit 1
    N=$(display_number)
    [ -n "$N" ] || { echo "no Xwayland, see $R/sway.log"; exit 1; }
    for i in $(seq 25); do DISPLAY=:$N xrandr --listmonitors >/dev/null 2>&1 && break; sleep 0.2; done
    DISPLAY=:$N xrandr --listmonitors
    ;;
layout)
    shift
    layout "$@"
    ;;
stop)
    # proot hides the binary, the session is found by its runtime dir
    for p in $(pgrep -x sway); do
        { tr "\\0" "\\n" < /proc/$p/environ; } 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$R" && kill $p
    done
    ;;
env)
    echo "DISPLAY=:$(display_number) WAYLAND_DISPLAY=$(ls $R | grep -m1 '^wayland-[0-9]*$') XDG_RUNTIME_DIR=$R"
    ;;
*)
    sed -n '2,7p' "$0"
    exit 1
    ;;
esac
