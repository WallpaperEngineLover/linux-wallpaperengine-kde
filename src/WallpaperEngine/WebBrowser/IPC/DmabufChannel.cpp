#include "DmabufChannel.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace WallpaperEngine::WebBrowser::IPC {
namespace {
    // what travels in the message body, the descriptors ride in the control data
    struct Wire {
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint32_t planeCount;
	uint64_t modifier;
	uint64_t sequence;
	uint32_t stride[DmabufFrame::MAX_PLANES];
	uint64_t offset[DmabufFrame::MAX_PLANES];
    };
}

void DmabufFrame::closeAll () {
    for (int& descriptor : fd) {
	if (descriptor >= 0) {
	    close (descriptor);
	    descriptor = -1;
	}
    }

    planeCount = 0;
}

bool sendDmabufFrame (const int socket, const DmabufFrame& frame) {
    if (frame.planeCount < 1 || frame.planeCount > DmabufFrame::MAX_PLANES) {
	return false;
    }

    Wire wire {};
    wire.width = frame.width;
    wire.height = frame.height;
    wire.fourcc = frame.fourcc;
    wire.planeCount = static_cast<uint32_t> (frame.planeCount);
    wire.modifier = frame.modifier;
    wire.sequence = frame.sequence;

    for (int i = 0; i < frame.planeCount; i++) {
	wire.stride[i] = frame.stride[i];
	wire.offset[i] = frame.offset[i];
    }

    iovec vector { &wire, sizeof (wire) };
    alignas (cmsghdr) char control[CMSG_SPACE (sizeof (int) * DmabufFrame::MAX_PLANES)] {};
    msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = CMSG_SPACE (sizeof (int) * frame.planeCount);

    cmsghdr* header = CMSG_FIRSTHDR (&message);

    if (header == nullptr) {
	return false;
    }

    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN (sizeof (int) * frame.planeCount);
    memcpy (CMSG_DATA (header), frame.fd.data (), sizeof (int) * frame.planeCount);

    ssize_t sent;

    do {
	sent = sendmsg (socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    return sent == static_cast<ssize_t> (sizeof (wire));
}

std::optional<DmabufFrame>
receiveLatestDmabufFrame (const int socket, const uint32_t maxWidth, const uint32_t maxHeight, bool& closed) {
    std::optional<DmabufFrame> latest;

    for (;;) {
	Wire wire {};
	iovec vector { &wire, sizeof (wire) };
	alignas (cmsghdr) char control[CMSG_SPACE (sizeof (int) * DmabufFrame::MAX_PLANES)] {};
	msghdr message {};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof (control);

	ssize_t received;

	do {
	    received = recvmsg (socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	} while (received < 0 && errno == EINTR);

	if (received == 0) {
	    closed = true;
	    break;
	}

	if (received < 0) {
	    if (errno != EAGAIN && errno != EWOULDBLOCK) {
		closed = true;
	    }

	    break;
	}

	DmabufFrame frame;
	int count = 0;

	for (cmsghdr* header = CMSG_FIRSTHDR (&message); header != nullptr; header = CMSG_NXTHDR (&message, header)) {
	    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
		continue;
	    }

	    const int incoming = static_cast<int> ((header->cmsg_len - CMSG_LEN (0)) / sizeof (int));
	    const int kept = incoming < DmabufFrame::MAX_PLANES ? incoming : DmabufFrame::MAX_PLANES;

	    for (int i = 0; i < incoming; i++) {
		int descriptor;
		memcpy (&descriptor, CMSG_DATA (header) + sizeof (int) * i, sizeof (int));

		if (i < kept && count < DmabufFrame::MAX_PLANES) {
		    frame.fd[count++] = descriptor;
		} else {
		    close (descriptor);
		}
	    }
	}

	const bool wellFormed = received == static_cast<ssize_t> (sizeof (wire))
	    && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && wire.planeCount >= 1
	    && wire.planeCount <= DmabufFrame::MAX_PLANES && static_cast<int> (wire.planeCount) == count
	    && wire.width >= 1 && wire.height >= 1 && wire.width <= maxWidth && wire.height <= maxHeight;

	if (!wellFormed) {
	    frame.closeAll ();
	    continue;
	}

	frame.width = wire.width;
	frame.height = wire.height;
	frame.fourcc = wire.fourcc;
	frame.modifier = wire.modifier;
	frame.planeCount = static_cast<int> (wire.planeCount);
	frame.sequence = wire.sequence;

	for (int i = 0; i < frame.planeCount; i++) {
	    frame.stride[i] = wire.stride[i];
	    frame.offset[i] = wire.offset[i];
	}

	if (latest) {
	    latest->closeAll ();
	}

	latest = frame;
    }

    return latest;
}

bool makeDmabufSocketPair (int& mainEnd, int& hostEnd) {
    int ends[2];

    if (socketpair (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, ends) != 0) {
	return false;
    }

    mainEnd = ends[0];
    hostEnd = ends[1];
    return true;
}
}
