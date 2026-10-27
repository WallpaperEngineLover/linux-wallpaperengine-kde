#!/bin/bash
# Logs what the session bus holds while a wallpaper is paused behind a fullscreen window: the broker's memory, how many
# cursor calls KWin sends the engine per second and how often the broker kicked programs off the bus.
# Usage: tools/dbus_quota_watch.sh [interval seconds, default 10]

interval=${1:-10}
broker=$(pgrep -u "$(id -u)" -xo dbus-broker)

if [ -z "$broker" ]; then
    echo "no dbus-broker running for this user" >&2
    exit 1
fi

while true; do
    calls=$(timeout 5 dbus-monitor --session "type='method_call',member='OnCursorMoved'" 2>/dev/null | grep -c 'member=OnCursorMoved')
    rss=$(ps -o rss= -p "$broker" | tr -d ' ')
    kicked=$(journalctl --user -b --no-pager 2>/dev/null | grep -c 'being disconnected as it does not have the resources')
    quota=$(journalctl --user -b --no-pager 2>/dev/null | grep -c "exceeded its 'bytes' quota")
    printf '%s broker_rss_kb=%s cursor_calls_per_5s=%s kicked_peers=%s quota_hits=%s\n' "$(date +%T)" "$rss" "$calls" "$kicked" "$quota"
    sleep "$interval"
done
