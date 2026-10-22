#include <catch2/catch_test_macros.hpp>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "WallpaperEngine/WebBrowser/IPC/DmabufChannel.h"

using namespace WallpaperEngine::WebBrowser::IPC;

namespace {
int makeBuffer (const char value) {
    const int fd = memfd_create ("dmabuf-test", MFD_CLOEXEC);
    REQUIRE (fd >= 0);
    REQUIRE (ftruncate (fd, 16) == 0);
    REQUIRE (pwrite (fd, &value, 1, 0) == 1);
    return fd;
}

char firstByte (const int fd) {
    char value = 0;
    REQUIRE (pread (fd, &value, 1, 0) == 1);
    return value;
}

DmabufFrame frameOf (const char value, const uint64_t sequence, const uint32_t width = 640) {
    DmabufFrame frame;
    frame.width = width;
    frame.height = 360;
    frame.fourcc = 0x34325241;
    frame.modifier = 0x0123456789abcdefull;
    frame.planeCount = 1;
    frame.sequence = sequence;
    frame.stride[0] = width * 4;
    frame.offset[0] = 64;
    frame.fd[0] = makeBuffer (value);
    return frame;
}
}

TEST_CASE ("a dmabuf frame arrives with its descriptors and layout", "[web][dmabuf]") {
    int mainEnd = -1;
    int hostEnd = -1;
    REQUIRE (makeDmabufSocketPair (mainEnd, hostEnd));

    bool closed = false;
    REQUIRE_FALSE (receiveLatestDmabufFrame (mainEnd, 4096, 4096, closed).has_value ());
    REQUIRE_FALSE (closed);

    DmabufFrame sent = frameOf ('x', 7);
    REQUIRE (sendDmabufFrame (hostEnd, sent));
    close (sent.fd[0]);

    auto received = receiveLatestDmabufFrame (mainEnd, 4096, 4096, closed);
    REQUIRE (received.has_value ());
    CHECK (received->width == 640);
    CHECK (received->height == 360);
    CHECK (received->fourcc == 0x34325241);
    CHECK (received->modifier == 0x0123456789abcdefull);
    CHECK (received->planeCount == 1);
    CHECK (received->sequence == 7);
    CHECK (received->stride[0] == 2560);
    CHECK (received->offset[0] == 64);
    REQUIRE (received->fd[0] >= 0);
    CHECK (firstByte (received->fd[0]) == 'x');
    received->closeAll ();

    close (mainEnd);
    close (hostEnd);
}

TEST_CASE ("only the newest of several queued dmabuf frames is kept", "[web][dmabuf]") {
    int mainEnd = -1;
    int hostEnd = -1;
    REQUIRE (makeDmabufSocketPair (mainEnd, hostEnd));

    for (int i = 0; i < 3; i++) {
	DmabufFrame frame = frameOf (static_cast<char> ('a' + i), static_cast<uint64_t> (i + 1));
	REQUIRE (sendDmabufFrame (hostEnd, frame));
	close (frame.fd[0]);
    }

    bool closed = false;
    auto received = receiveLatestDmabufFrame (mainEnd, 4096, 4096, closed);
    REQUIRE (received.has_value ());
    CHECK (received->sequence == 3);
    CHECK (firstByte (received->fd[0]) == 'c');
    received->closeAll ();

    CHECK_FALSE (receiveLatestDmabufFrame (mainEnd, 4096, 4096, closed).has_value ());

    close (mainEnd);
    close (hostEnd);
}

TEST_CASE ("a dmabuf frame larger than the capacity is dropped and its descriptors closed", "[web][dmabuf]") {
    int mainEnd = -1;
    int hostEnd = -1;
    REQUIRE (makeDmabufSocketPair (mainEnd, hostEnd));

    DmabufFrame frame = frameOf ('z', 1, 2000);
    REQUIRE (sendDmabufFrame (hostEnd, frame));
    close (frame.fd[0]);

    bool closed = false;
    CHECK_FALSE (receiveLatestDmabufFrame (mainEnd, 1000, 1000, closed).has_value ());

    DmabufFrame fitting = frameOf ('y', 2, 500);
    REQUIRE (sendDmabufFrame (hostEnd, fitting));
    close (fitting.fd[0]);
    auto received = receiveLatestDmabufFrame (mainEnd, 1000, 1000, closed);
    REQUIRE (received.has_value ());
    CHECK (received->sequence == 2);
    received->closeAll ();

    close (mainEnd);
    close (hostEnd);
}

TEST_CASE ("the receiver notices the host going away and a frame without planes is not sent", "[web][dmabuf]") {
    int mainEnd = -1;
    int hostEnd = -1;
    REQUIRE (makeDmabufSocketPair (mainEnd, hostEnd));

    DmabufFrame empty;
    CHECK_FALSE (sendDmabufFrame (hostEnd, empty));

    close (hostEnd);

    bool closed = false;
    CHECK_FALSE (receiveLatestDmabufFrame (mainEnd, 4096, 4096, closed).has_value ());
    CHECK (closed);

    close (mainEnd);
}
