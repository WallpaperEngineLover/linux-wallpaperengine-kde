#!/bin/bash
# hide_steam_kde.sh - freezes Steam and minimizes its windows with a KWin script. On the Steam Deck shaped VM the client starts
# with -steamdeck, updates itself a few minutes after login and then takes the whole screen (the VM's display reads "not active"),
# a stopped client stays where it is and its sign-in window no longer covers the wallpaper.
freeze() {
    pkill -STOP -f '[u]buntu12_32/steam'
    pkill -STOP -f '[s]teamwebhelper'
}
if [ "$1" = --watch ]; then
    # Steam is not running yet this early after login: keep stopping what shows up for 15 minutes
    (setsid nohup bash -c 'for _ in $(seq 900); do pkill -STOP -f "[u]buntu12_32/steam"; pkill -STOP -f "[s]teamwebhelper"; sleep 1; done' >/dev/null 2>&1 &)
    exit 0
fi
freeze
script=$(mktemp /tmp/lwe-hide-steam-XXXXXX.js)
echo 'for (const w of workspace.windowList ()) { if (String (w.resourceClass).toLowerCase ().includes ("steam")) { w.minimized = true; } }' > "$script"
live=$(gdbus introspect --session --dest org.kde.KWin --object-path /Scripting | grep -o 'node Script[0-9]*' | tr -dc '0-9\n')
names=()
for attempt in 0 1 2 3; do
    name=lwe-hide-steam-$attempt
    id=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.loadScript "$script" "$name" | tr -dc '0-9')
    names+=("$name")
    echo "$live" | grep -qx "$id" || break
done
gdbus call --session --dest org.kde.KWin --object-path "/Scripting/Script$id" --method org.kde.kwin.Script.run >/dev/null
sleep 2
for name in "${names[@]}"; do
    gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.unloadScript "$name" >/dev/null
done
rm -f "$script"
