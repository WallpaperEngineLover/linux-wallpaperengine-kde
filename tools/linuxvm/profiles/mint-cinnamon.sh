# Cinnamon on X11 (Muffin): --screen-root draws into a desktop type background window (XDG_CURRENT_DESKTOP X-Cinnamon), the windowbg
# group of guest_test.sh checks --window-background; the Mint Update Manager and System Reports windows are closed by PREP,
# fullscreen and maximize go through a GTK window (fullscreen_window.py)
VM=mint-cinnamon
SCREEN=Virtual-1
SHOT_CMD='gnome-screenshot -f $SHOT_FILE'
FULLSCREEN_CMD='python3 ~/lwe/fullscreen_window.py'
MAXIMIZE_CMD='python3 ~/lwe/fullscreen_window.py maximize'
WEM=deb
MINIMIZE_CMD='xdotool search --name lwe-fullscreen-test windowminimize %@'
MULTI=1
MULTI_SHOT_CMD='gnome-screenshot -f $SHOT_FILE'
PREP='pkill -f "[m]intupdate"; pkill -x mintreport; true'
