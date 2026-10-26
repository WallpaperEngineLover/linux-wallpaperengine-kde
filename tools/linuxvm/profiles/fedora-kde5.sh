VM=fedora-kde5
SCREEN=Virtual-1
SHOT_CMD=host
WEM=app
POINTER=0
PREP='kwriteconfig5 --file plasma-welcomerc --group General --key LastSeenVersion 99.0; pkill -x plasma-welcome; (setsid nohup kde-inhibit --power --screenSaver sleep 100000 >/dev/null 2>&1 &)'
