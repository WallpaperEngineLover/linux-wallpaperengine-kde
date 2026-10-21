VM=steamos
SCREEN=Virtual-1
SHOT_CMD=host
FULLSCREEN_CMD='konsole --fullscreen'
MAXIMIZE_CMD='~/lwe/maximize_kde.sh'
WEM=app
PREP='(setsid nohup kde-inhibit --power --screenSaver sleep 100000 >/dev/null 2>&1 &)'
WEM_LAUNCH='systemd-run --user --scope -q'
