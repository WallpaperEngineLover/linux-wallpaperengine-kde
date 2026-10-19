#!/bin/bash
# maximize_kde.sh - a Konsole window maximized by a KWin script, for the pause-on-maximized check on Plasma (Konsole has
# no maximize option, KWin 6 scripts have workspace.activeWindow.setMaximize). Runs until it is killed.
konsole -e sleep 100000 &
terminal=$!
trap 'kill $terminal 2>/dev/null' EXIT
sleep 4
script=$(mktemp /tmp/lwe-maximize-XXXXXX.js)
echo 'workspace.activeWindow.setMaximize(true, true);' > "$script"
id=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.loadScript "$script" lwe-maximize-test | tr -dc '0-9')
gdbus call --session --dest org.kde.KWin --object-path "/Scripting/Script$id" --method org.kde.kwin.Script.run >/dev/null
sleep 2
gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.unloadScript lwe-maximize-test >/dev/null
rm -f "$script"
wait $terminal
