VM=arch-hypr
SCREEN=Virtual-1
SHOT_CMD='grim $SHOT_FILE'
FULLSCREEN_CMD='kitty --start-as=fullscreen'
WEM=app
PREP='(setsid nohup systemd-inhibit --what=idle:sleep --who=lwe-test --why=test sleep 100000 >/dev/null 2>&1 &)'
