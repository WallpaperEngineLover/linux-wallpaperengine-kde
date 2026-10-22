VM=steamdeck
SCREEN=Virtual-1
SHOT_CMD=host
FULLSCREEN_CMD='konsole --fullscreen'
MAXIMIZE_CMD='~/lwe/maximize_kde.sh'
WEM=app
HIDE=1
PREP='bash ~/lwe/hide_steam_kde.sh --watch; (setsid nohup kde-inhibit --power --screenSaver sleep 100000 >/dev/null 2>&1 &)'
WEM_LAUNCH='systemd-run --user --scope -q'
EXTRA=deck_extra

deck_extra() {
    lv scp "$1" "$HERE/deck_check.sh" lwe/deck_check.sh
    sshv ". ~/sessenv; SCREEN=$SCREEN bash ~/lwe/deck_check.sh"
}
