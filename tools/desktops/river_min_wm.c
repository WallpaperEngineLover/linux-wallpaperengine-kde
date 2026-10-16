// Minimal river 0.4 WM: ends every manage/render sequence at once and binds river_layer_shell_v1, without which
// river closes all layer surfaces. Exits when the WM role is taken or the connection ends.
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"

static struct river_window_manager_v1* wm;
static struct river_layer_shell_v1* layer_shell;
static int running = 1;

static void global (void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (strcmp (interface, river_window_manager_v1_interface.name) == 0) {
	wm = wl_registry_bind (registry, name, &river_window_manager_v1_interface, 1);
    } else if (strcmp (interface, river_layer_shell_v1_interface.name) == 0) {
	layer_shell = wl_registry_bind (registry, name, &river_layer_shell_v1_interface, 1);
    }
}

static void global_remove (void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void unavailable (void* data, struct river_window_manager_v1* wm) {
    fprintf (stderr, "another window manager is running\n");
    running = 0;
}

static void finished (void* data, struct river_window_manager_v1* wm) { running = 0; }

static void manage_start (void* data, struct river_window_manager_v1* wm) { river_window_manager_v1_manage_finish (wm); }

static void render_start (void* data, struct river_window_manager_v1* wm) { river_window_manager_v1_render_finish (wm); }

static void session_locked (void* data, struct river_window_manager_v1* wm) { }

static void session_unlocked (void* data, struct river_window_manager_v1* wm) { }

static void window (void* data, struct river_window_manager_v1* wm, struct river_window_v1* window) { }

static void output (void* data, struct river_window_manager_v1* wm, struct river_output_v1* output) { }

static void seat (void* data, struct river_window_manager_v1* wm, struct river_seat_v1* seat) { }

static const struct river_window_manager_v1_listener wm_listener = {
    unavailable, finished, manage_start, render_start, session_locked, session_unlocked, window, output, seat,
};

int main (void) {
    struct wl_display* display = wl_display_connect (NULL);

    if (!display) {
	fprintf (stderr, "no wayland display\n");
	return 1;
    }

    wl_registry_add_listener (wl_display_get_registry (display), &registry_listener, NULL);
    wl_display_roundtrip (display);

    if (!wm || !layer_shell) {
	fprintf (stderr, "the compositor has no river_window_manager_v1 or river_layer_shell_v1\n");
	return 1;
    }

    river_window_manager_v1_add_listener (wm, &wm_listener, NULL);
    printf ("ready\n");
    fflush (stdout);

    while (running && wl_display_dispatch (display) != -1) { }

    return 0;
}
