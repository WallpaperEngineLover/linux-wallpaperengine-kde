/*
 * linux-wallpaperengine-gnome-helper <socket path>
 *
 * GNOME 45-48 only make desktop windows of a client the Shell spawned (MetaWaylandClient owns_window). The extension
 * spawns this, it passes WAYLAND_SOCKET to the engine on <socket path> and lives until the engine closes it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main (int argc, char** argv) {
    if (argc != 2) {
	fprintf (stderr, "usage: %s <socket path>\n", argv[0]);
	return 2;
    }

    const char* wayland = getenv ("WAYLAND_SOCKET");
    char* end = NULL;
    const long fd = wayland ? strtol (wayland, &end, 10) : -1;

    if (fd < 0 || !end || *end != '\0') {
	fprintf (stderr, "no WAYLAND_SOCKET from the compositor\n");
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
