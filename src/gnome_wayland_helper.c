/*
 * linux-wallpaperengine-gnome-helper <socket path>
 *
 * Hands the engine a Wayland connection made outside of it, over <socket path>, and lives until the engine closes it.
 * GNOME 45-48 only make desktop windows of a client the Shell spawned (MetaWaylandClient owns_window): the extension
 * spawns this with WAYLAND_SOCKET set. A Flatpak engine starts it on the host through flatpak-spawn when its own
 * socket (Flatpak's security context) hides the layer shell: then it connects to WAYLAND_DISPLAY itself.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* what libwayland's wl_display_connect does: an absolute WAYLAND_DISPLAY, else one in XDG_RUNTIME_DIR */
static int connect_display (void) {
    const char* display = getenv ("WAYLAND_DISPLAY");
    const char* runtime = getenv ("XDG_RUNTIME_DIR");
    struct sockaddr_un address = { .sun_family = AF_UNIX };

    if (!display || !*display) {
	display = "wayland-0";
    }

    const int written = display[0] == '/'
	? snprintf (address.sun_path, sizeof (address.sun_path), "%s", display)
	: snprintf (address.sun_path, sizeof (address.sun_path), "%s/%s", runtime ? runtime : "", display);

    if (written < 0 || (size_t)written >= sizeof (address.sun_path) || (display[0] != '/' && !runtime)) {
	return -1;
    }

    const int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0 || connect (fd, (struct sockaddr*)&address, sizeof (address)) != 0) {
	fprintf (stderr, "cannot connect to %s: %s\n", address.sun_path, strerror (errno));
	if (fd >= 0) {
	    close (fd);
	}
	return -1;
    }

    return fd;
}

int main (int argc, char** argv) {
    if (argc != 2) {
	fprintf (stderr, "usage: %s <socket path>\n", argv[0]);
	return 2;
    }

    const char* wayland = getenv ("WAYLAND_SOCKET");
    char* end = NULL;
    const long fd = wayland ? strtol (wayland, &end, 10) : connect_display ();

    if (fd < 0 || (wayland && (!end || *end != '\0'))) {
	fprintf (stderr, "no WAYLAND_SOCKET from the compositor and no WAYLAND_DISPLAY to connect to\n");
	return 2;
    }

    struct sockaddr_un address = { .sun_family = AF_UNIX };

    if (strlen (argv[1]) >= sizeof (address.sun_path)) {
	fprintf (stderr, "socket path too long\n");
	return 2;
    }

    strcpy (address.sun_path, argv[1]);

    const int engine = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (engine < 0 || connect (engine, (struct sockaddr*)&address, sizeof (address)) != 0) {
	fprintf (stderr, "cannot reach the engine at %s: %s\n", argv[1], strerror (errno));
	return 1;
    }

    char byte = 0;
    struct iovec data = { .iov_base = &byte, .iov_len = 1 };
    union {
	char buffer[CMSG_SPACE (sizeof (int))];
	struct cmsghdr align;
    } control;
    memset (&control, 0, sizeof (control));

    struct msghdr message = {
	.msg_iov = &data,
	.msg_iovlen = 1,
	.msg_control = control.buffer,
	.msg_controllen = sizeof (control.buffer),
    };
    struct cmsghdr* header = CMSG_FIRSTHDR (&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN (sizeof (int));
    const int passed = (int)fd;
    memcpy (CMSG_DATA (header), &passed, sizeof (int));

    if (sendmsg (engine, &message, MSG_NOSIGNAL) != 1) {
	fprintf (stderr, "cannot hand the connection over: %s\n", strerror (errno));
	return 1;
    }

    close (passed);

    // EOF = engine gone
    for (;;) {
	const ssize_t got = read (engine, &byte, 1);

	if (got > 0 || (got < 0 && errno == EINTR)) {
	    continue;
	}

	break;
    }

    return 0;
}
