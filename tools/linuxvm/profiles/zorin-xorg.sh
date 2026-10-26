# Zorin's Xorg session (GNOME Shell 46 on X11): --screen-root has to draw into a background window of our own, GNOME's own
# background covers the root window. The same disk as zorin-gnome, so never in a lane at the same time as that profile.
# GDM is switched to Xorg (WaylandEnable=false) and restarted before the session is awaited
VM=zorin-gnome
SCREEN=Virtual-1
SHOT_CMD=host
FULLSCREEN_CMD='python3 ~/lwe/fullscreen_window.py'
WEM=deb
BOOT_PREP='sudo sed -i "s/^#WaylandEnable=false/WaylandEnable=false/" /etc/gdm3/custom.conf; sudo systemctl restart gdm3'
