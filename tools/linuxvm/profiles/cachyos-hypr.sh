VM=cachyos-hypr
SCREEN=Virtual-1
SHOT_CMD='grim $SHOT_FILE'
FULLSCREEN_CMD='kitty --start-as=fullscreen'
WEM=app
PREP='pkill -x cachyos-hello; pkill -x gcr-prompter; (setsid nohup systemd-inhibit --what=idle:sleep --who=lwe-test --why=test sleep 100000 >/dev/null 2>&1 &)'
# Hyprland: layer shell, maximized windows do not pause by default (wlroots rule), so no MAXIMIZE_CMD
# the CachyOS Hello window and the keyring prompt open on every login and tile over the whole screen: PREP closes them
