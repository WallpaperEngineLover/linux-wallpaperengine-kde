#!/bin/bash
# maximize_kde.sh - a Konsole window maximized by a KWin script, for the pause-on-maximized check on Plasma (Konsole has
# no maximize option, KWin 6 scripts have workspace.activeWindow.setMaximize). Runs until it is killed.
konsole -e sleep 100000 &
terminal=$!
trap 'kill $terminal 2>/dev/null' EXIT
sleep 4
script=$(mktemp /tmp/lwe-maximize-XXXXXX.js)
echo 'workspace.activeWindow.setMaximize(true, true);' > "$script"
# KWin 6.4 hands a new script the id of a loaded one after any script was unloaded (it numbers by count), "run" then
# starts that one again: load under more names until the id is a free one
live=$(gdbus introspect --session --dest org.kde.KWin --object-path /Scripting | grep -o 'node Script[0-9]*' | tr -dc '0-9\n')
names=()
for attempt in 0 1 2 3; do
    name=lwe-maximize-test-$attempt
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
wait $terminal
