// One virtual pointer for wlroots-style compositors (sway, labwc, Hyprland, niri), kept for as long as stdin is open:
// wlrctl makes a new device per call and the seat's pointer capability comes and goes with it.
// stdin lines: "abs <x> <y> <width> <height>" (position within a layout extent), "quit"; each answered with "ok"
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1* manager;
static struct wl_seat* seat;

static void global (void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (strcmp (interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
	manager = wl_registry_bind (registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (strcmp (interface, wl_seat_interface.name) == 0) {
	seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
    }
}

static void global_remove (void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registry_listener = { global, global_remove };

int main (void) {
    struct wl_display* display = wl_display_connect (NULL);

    if (!display) {
	fprintf (stderr, "no wayland display\n");
	return 1;
    }

    wl_registry_add_listener (wl_display_get_registry (display), &registry_listener, NULL);
    wl_display_roundtrip (display);

    if (!manager) {
	fprintf (stderr, "the compositor has no zwlr_virtual_pointer_manager_v1\n");
	return 1;
    }

    struct zwlr_virtual_pointer_v1* pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer (manager, seat);
    char line[256];
    uint32_t time = 1;

    wl_display_roundtrip (display);

    while (fgets (line, sizeof line, stdin)) {
	unsigned x, y, width, height;

	if (sscanf (line, "abs %u %u %u %u", &x, &y, &width, &height) == 4) {
	    zwlr_virtual_pointer_v1_motion_absolute (pointer, time++, x, y, width, height);
	    zwlr_virtual_pointer_v1_frame (pointer);
	} else if (strncmp (line, "quit", 4) == 0) {
	    break;
	}

	wl_display_roundtrip (display);
	printf ("ok\n");
	fflush (stdout);
    }

    zwlr_virtual_pointer_v1_destroy (pointer);
    wl_display_roundtrip (display);
    wl_display_disconnect (display);
    return 0;
}
