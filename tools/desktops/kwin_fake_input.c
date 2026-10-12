/* Drives KWin's pointer via org_kde_kwin_fake_input (needs KWIN_WAYLAND_NO_PERMISSION_CHECKS).
 * Usage: kwin_fake_input move X Y | down BUTTON | up BUTTON ...   (evdev codes, 272 = left) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "fake-input-client-protocol.h"

static struct org_kde_kwin_fake_input* fake_input;

static void global (void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (strcmp (interface, org_kde_kwin_fake_input_interface.name) == 0) {
	fake_input = wl_registry_bind (registry, name, &org_kde_kwin_fake_input_interface, version < 4 ? version : 4);
    }
}

static void global_remove (void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registry_listener = { global, global_remove };

int main (int argc, char** argv) {
    struct wl_display* display = wl_display_connect (NULL);

    if (!display) {
	fprintf (stderr, "no Wayland display\n");
	return 1;
    }

    wl_registry_add_listener (wl_display_get_registry (display), &registry_listener, NULL);
    wl_display_roundtrip (display);

    if (!fake_input) {
	fprintf (stderr, "the compositor offers no org_kde_kwin_fake_input\n");
	return 1;
    }

    org_kde_kwin_fake_input_authenticate (fake_input, "lwe-lab", "desktop tests");

    for (int i = 1; i < argc; i++) {
	if (strcmp (argv[i], "move") == 0 && i + 2 < argc) {
	    org_kde_kwin_fake_input_pointer_motion_absolute (
		fake_input, wl_fixed_from_double (atof (argv[i + 1])), wl_fixed_from_double (atof (argv[i + 2]))
	    );
	    i += 2;
	} else if ((strcmp (argv[i], "down") == 0 || strcmp (argv[i], "up") == 0) && i + 1 < argc) {
	    org_kde_kwin_fake_input_button (fake_input, atoi (argv[i + 1]), strcmp (argv[i], "down") == 0);
	    i += 1;
	} else {
	    fprintf (stderr, "unknown command %s\n", argv[i]);
	    return 1;
	}
	wl_display_roundtrip (display);
    }

    wl_display_disconnect (display);
    return 0;
}
