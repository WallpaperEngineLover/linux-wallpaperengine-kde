# Gala has no layer shell: the engine draws through XWayland into a background window and sees only X11 windows, so the test window is an
# XWayland one (a native Wayland window pauses nothing there) and there is no MAXIMIZE_CMD (Gala keeps a GTK window at its size)
# Plank covers the bottom 70 px of the screen, the dock check of pointer_check.sh goes on the wallpaper's own dock above it
POINTER_DOCK_XY=960,980
VM=elementary
SCREEN=Virtual-1
SHOT_CMD=host
FULLSCREEN_CMD='GDK_BACKEND=x11 python3 ~/lwe/fullscreen_window.py'
WEM=deb
