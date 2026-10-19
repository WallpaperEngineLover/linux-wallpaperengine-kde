VM=debian-xfce
SCREEN=Virtual-1
SHOT_CMD=host
FULLSCREEN_CMD='bash -c "xterm -T lwe-fullscreen-test -e sleep 100000 & sleep 2; wmctrl -r lwe-fullscreen-test -b add,fullscreen; wait"'
WEM=deb
PREP='xset s off -dpms; pkill -f "[x]fce4-screensaver"; pkill -f "[x]fce4-power-manager"; true'
