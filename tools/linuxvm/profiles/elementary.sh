# Gala has no layer shell: the engine draws through XWayland into a background window; X11 windows come from the X server, native
# Wayland ones from Gala's DesktopIntegration D-Bus interface (size only, so no MAXIMIZE_CMD: Gala keeps a GTK window at its size)
# Plank covers the bottom 70 px of the screen, the dock check of pointer_check.sh goes on the wallpaper's own dock above it
POINTER_DOCK_XY=960,980
VM=elementary
SCREEN=Virtual-1
SHOT_CMD=host
FULLSCREEN_CMD='python3 ~/lwe/fullscreen_window.py'
WEM=deb
