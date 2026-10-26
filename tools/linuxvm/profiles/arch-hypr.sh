# Hyprland: layer shell, per output pause; maximized windows do not pause here (wlroots rule, needs --pause-on-maximized), so no
# MAXIMIZE_CMD. The idle inhibitor keeps the session from blanking, sessenv.sh reads the session variables from the user manager
VM=arch-hypr
SCREEN=Virtual-1
SHOT_CMD='grim $SHOT_FILE'
FULLSCREEN_CMD='kitty --start-as=fullscreen'
WEM=app
PREP='(setsid nohup systemd-inhibit --what=idle:sleep --who=lwe-test --why=test sleep 100000 >/dev/null 2>&1 &)'
FULL_ENGINE=1
