#include "LedOutput.h"

#include <algorithm>
#include <chrono>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"

namespace WallpaperEngine::Led {
using namespace std::chrono_literals;

namespace {
    struct Socket {
	int fd = -1;

	~Socket () {
	    if (this->fd >= 0) {
		close (this->fd);
	    }
	}

	bool open (const std::string& host, const uint16_t port) {
	    addrinfo hints = {};
	    addrinfo* found = nullptr;

	    hints.ai_family = AF_UNSPEC;
	    hints.ai_socktype = SOCK_STREAM;

	    if (getaddrinfo (host.c_str (), std::to_string (port).c_str (), &hints, &found) != 0) {
		return false;
	    }

	    for (const auto* it = found; it != nullptr && this->fd < 0; it = it->ai_next) {
		const int candidate
		    = socket (it->ai_family, it->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, it->ai_protocol);

		if (candidate < 0) {
		    continue;
		}

		bool connected = connect (candidate, it->ai_addr, it->ai_addrlen) == 0;

		if (!connected && errno == EINPROGRESS) {
		    pollfd wait = { .fd = candidate, .events = POLLOUT };
		    int error = 0;
		    socklen_t length = sizeof (error);

		    connected = poll (&wait, 1, 2000) == 1
			&& getsockopt (candidate, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
		}

		if (connected) {
		    this->fd = candidate;
		} else {
		    close (candidate);
		}
	    }

	    freeaddrinfo (found);

	    if (this->fd >= 0) {
		const int on = 1;
		setsockopt (this->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof (on));
	    }

	    return this->fd >= 0;
	}

	[[nodiscard]] bool writeAll (std::span<const uint8_t> bytes) const {
	    while (!bytes.empty ()) {
		const ssize_t sent = send (this->fd, bytes.data (), bytes.size (), MSG_NOSIGNAL);

		if (sent < 0 && errno == EINTR) {
		    continue;
		}

		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		    pollfd wait = { .fd = this->fd, .events = POLLOUT };

		    if (poll (&wait, 1, 2000) != 1) {
			return false;
		    }

		    continue;
		}

		if (sent <= 0) {
		    return false;
		}

		bytes = bytes.subspan (static_cast<size_t> (sent));
	    }

	    return true;
	}

	[[nodiscard]] bool readAll (std::span<uint8_t> bytes, const int timeoutMs) const {
	    while (!bytes.empty ()) {
		pollfd wait = { .fd = this->fd, .events = POLLIN };

		if (poll (&wait, 1, timeoutMs) != 1) {
		    return false;
		}

		const ssize_t got = recv (this->fd, bytes.data (), bytes.size (), 0);

		if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
		    continue;
		}

		if (got <= 0) {
		    return false;
		}

		bytes = bytes.subspan (static_cast<size_t> (got));
	    }

	    return true;
	}

	[[nodiscard]] bool readable () const {
	    pollfd wait = { .fd = this->fd, .events = POLLIN };
	    return poll (&wait, 1, 0) == 1;
	}

	bool readPacket (OpenRgb::Header& header, std::vector<uint8_t>& payload, const int timeoutMs) const {
	    uint8_t raw[OpenRgb::kHeaderSize];

	    if (!this->readAll (raw, timeoutMs)) {
		return false;
	    }

	    const auto parsed = OpenRgb::parseHeader (raw);

	    if (!parsed || parsed->size > (1u << 24)) {
		return false;
	    }

	    header = *parsed;
	    payload.resize (header.size);
	    return this->readAll (payload, 5000);
	}
    };

    struct Target {
	OpenRgb::Device device;
	std::vector<LedPixel> layout;
	std::vector<uint8_t> last;
	std::chrono::steady_clock::time_point lastSent;
    };
}

LedOutput::~LedOutput () { this->stop (); }

void LedOutput::start (const Options& options) {
    if (this->m_running.exchange (true)) {
	return;
    }

    this->m_options = options;
    this->m_thread = std::thread ([this] { Logging::guardThread ("LED output", [this] { this->run (); }); });
}

void LedOutput::stop () {
    if (!this->m_running.exchange (false)) {
	return;
    }

    this->m_wake.notify_all ();

    if (this->m_thread.joinable ()) {
	this->m_thread.join ();
    }
}

void LedOutput::submit (const std::span<const uint8_t> rgb) {
    if (!this->m_running.load () || rgb.size () != kFrameBytes) {
	return;
    }

    {
	const std::lock_guard lock (this->m_mutex);
	this->m_frame.assign (rgb.begin (), rgb.end ());
	this->m_fresh = true;
    }

    this->m_wake.notify_one ();
}

void LedOutput::run () {
    bool reported = false;

    while (this->m_running.load ()) {
	if (this->connectAndServe (this->m_options)) {
	    reported = false;
	} else if (!reported) {
	    sLog.error (
		"LED output: no OpenRGB server at ", this->m_options.host, ":", this->m_options.port,
		", trying again every few seconds (start OpenRGB with --server)"
	    );
	    reported = true;
	}

	std::unique_lock lock (this->m_mutex);
	this->m_wake.wait_for (lock, 3s, [this] { return !this->m_running.load (); });
    }
}

bool LedOutput::connectAndServe (const Options& options) {
    Socket socket;

    if (!socket.open (options.host, options.port)) {
	return false;
    }

    std::vector<uint8_t> payload;
    OpenRgb::Header header;
    uint32_t protocol = 0;

    {
	std::vector<uint8_t> ask;
	OpenRgb::put32 (ask, OpenRgb::kProtocolVersion);

	if (!socket.writeAll (OpenRgb::makePacket (0, OpenRgb::RequestProtocolVersion, ask))) {
	    return false;
	}

	if (socket.readPacket (header, payload, 1500) && header.id == OpenRgb::RequestProtocolVersion
	    && payload.size () >= 4) {
	    const uint32_t server
		= payload[0] | (payload[1] << 8) | (payload[2] << 16) | (static_cast<uint32_t> (payload[3]) << 24);
	    protocol = std::min (server, OpenRgb::kProtocolVersion);
	}
    }

    if (protocol < OpenRgb::kProtocolVersion) {
	if (!this->m_protocolReported) {
	    sLog.error (
		"LED output: the OpenRGB server speaks protocol ", protocol, ", needs ", OpenRgb::kProtocolVersion,
		" (OpenRGB 0.9 or newer)"
	    );
	    this->m_protocolReported = true;
	}

	return true;
    }

    this->m_protocolReported = false;

    const std::string name = "linux-wallpaperengine";

    if (!socket.writeAll (
	    OpenRgb::makePacket (
		0, OpenRgb::SetClientName, { reinterpret_cast<const uint8_t*> (name.c_str ()), name.size () + 1 }
	    )
	)) {
	return false;
    }

    std::vector<Target> targets;
    const auto listDevices = [&] () -> bool {
	targets.clear ();

	if (!socket.writeAll (OpenRgb::makePacket (0, OpenRgb::RequestControllerCount))) {
	    return false;
	}

	do {
	    if (!socket.readPacket (header, payload, 3000)) {
		return false;
	    }
	} while (header.id != OpenRgb::RequestControllerCount);

	if (payload.size () < 4) {
	    return false;
	}

	const uint32_t count
	    = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (static_cast<uint32_t> (payload[3]) << 24);

	for (uint32_t index = 0; index < count; index++) {
	    std::vector<uint8_t> ask;
	    OpenRgb::put32 (ask, OpenRgb::kProtocolVersion);

	    if (!socket.writeAll (OpenRgb::makePacket (index, OpenRgb::RequestControllerData, ask))) {
		return false;
	    }

	    do {
		if (!socket.readPacket (header, payload, 3000)) {
		    return false;
		}
	    } while (header.id != OpenRgb::RequestControllerData);

	    auto device = OpenRgb::parseController (payload, index);

	    if (!device || device->ledCount == 0) {
		continue;
	    }

	    sLog.out ("LED output: ", device->name, " with ", device->ledCount, " leds");
	    if (!socket.writeAll (OpenRgb::makePacket (index, OpenRgb::SetCustomMode))) {
		continue;
	    }

	    targets.push_back ({ .device = *device, .layout = layoutDevice (*device) });
	}

	this->m_devices.store (targets.size ());
	return true;
    };

    if (!listDevices ()) {
	return false;
    }

    if (targets.empty ()) {
	sLog.out ("LED output: the OpenRGB server has no devices");
    }

    std::vector<uint8_t> frame (kFrameBytes);
    std::vector<uint8_t> processed (kFrameBytes);
    std::vector<uint8_t> colors;
    auto lastRound = std::chrono::steady_clock::now () - 1h;

    while (this->m_running.load ()) {
	{
	    std::unique_lock lock (this->m_mutex);

	    this->m_wake.wait_for (lock, 250ms, [this] { return this->m_fresh || !this->m_running.load (); });

	    if (this->m_fresh) {
		frame = this->m_frame;
		this->m_fresh = false;
	    } else {
		frame.clear ();
	    }
	}

	while (socket.readable ()) {
	    if (!socket.readPacket (header, payload, 1000)) {
		return true;
	    }

	    if (header.id == OpenRgb::DeviceListUpdated && !listDevices ()) {
		return true;
	    }
	}

	if (frame.size () != kFrameBytes) {
	    continue;
	}

	const auto now = std::chrono::steady_clock::now ();

	if (options.reduceFps && now - lastRound < 66ms) {
	    continue;
	}

	lastRound = now;
	applyBrightness (frame, processed, options.brightness);

	for (auto& target : targets) {
	    sampleDevice (processed, target.layout, colors);

	    // devices keep their colors, so an unchanged frame only goes out now and then
	    if (colors == target.last && now - target.lastSent < 2s) {
		continue;
	    }

	    if (!socket.writeAll (OpenRgb::makeUpdateLeds (target.device.index, colors))) {
		return true;
	    }

	    target.last = colors;
	    target.lastSent = now;
	}
    }

    return true;
}
}
