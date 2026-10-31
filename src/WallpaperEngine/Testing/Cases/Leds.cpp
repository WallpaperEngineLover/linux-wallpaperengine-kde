#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "WallpaperEngine/Led/LedLayout.h"
#include "WallpaperEngine/Led/LedOutput.h"
#include "WallpaperEngine/Led/OpenRgbProtocol.h"

using namespace WallpaperEngine::Led;
using namespace std::chrono_literals;

namespace {
void put16 (std::vector<uint8_t>& out, const uint32_t value) {
    out.push_back (static_cast<uint8_t> (value));
    out.push_back (static_cast<uint8_t> (value >> 8));
}

void put32 (std::vector<uint8_t>& out, const uint32_t value) {
    put16 (out, value & 0xFFFF);
    put16 (out, value >> 16);
}

void putText (std::vector<uint8_t>& out, const std::string& text) {
    put16 (out, static_cast<uint32_t> (text.size () + 1));
    out.insert (out.end (), text.begin (), text.end ());
    out.push_back (0);
}

struct TestZone {
    std::string name;
    uint32_t leds = 0;
    uint32_t height = 0;
    uint32_t width = 0;
};

std::vector<uint8_t> controllerData (const std::string& name, const std::vector<TestZone>& zones) {
    std::vector<uint8_t> body;
    uint32_t total = 0;

    put32 (body, 0);
    put32 (body, 6);
    putText (body, name);

    for (const char* field : { "vendor", "description", "version", "serial", "location" }) {
	putText (body, field);
    }

    put16 (body, 1);
    put32 (body, 0);
    putText (body, "Direct");

    for (int i = 0; i < 12; i++) {
	put32 (body, 0);
    }

    put16 (body, 0);
    put16 (body, static_cast<uint32_t> (zones.size ()));

    for (const auto& zone : zones) {
	putText (body, zone.name);
	put32 (body, 1);
	put32 (body, zone.leds);
	put32 (body, zone.leds);
	put32 (body, zone.leds);

	if (zone.height == 0) {
	    put16 (body, 0);
	} else {
	    put16 (body, 8 + 4 * zone.height * zone.width);
	    put32 (body, zone.height);
	    put32 (body, zone.width);

	    for (uint32_t cell = 0; cell < zone.height * zone.width; cell++) {
		put32 (body, cell);
	    }
	}

	total += zone.leds;
    }

    put16 (body, static_cast<uint32_t> (total));

    for (uint32_t led = 0; led < total; led++) {
	putText (body, "led");
	put32 (body, led);
    }

    put16 (body, 0);

    const auto size = static_cast<uint32_t> (body.size ());
    std::vector<uint8_t> out = { static_cast<uint8_t> (size), static_cast<uint8_t> (size >> 8),
				 static_cast<uint8_t> (size >> 16), static_cast<uint8_t> (size >> 24) };

    out.insert (out.end (), body.begin () + 4, body.end ());
    return out;
}

class FakeServer {
public:
    FakeServer () {
	this->m_listener = socket (AF_INET, SOCK_STREAM, 0);

	if (this->m_listener < 0) {
	    throw std::runtime_error ("cannot make the fake OpenRGB server socket");
	}

	sockaddr_in address = {};
	socklen_t length = sizeof (address);

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

	if (bind (this->m_listener, reinterpret_cast<sockaddr*> (&address), sizeof (address)) != 0
	    || listen (this->m_listener, 1) != 0
	    || getsockname (this->m_listener, reinterpret_cast<sockaddr*> (&address), &length) != 0) {
	    close (this->m_listener);
	    throw std::runtime_error ("cannot set up the fake OpenRGB server");
	}

	this->m_port = ntohs (address.sin_port);
	this->m_thread = std::thread ([this] { this->serve (); });
    }

    ~FakeServer () {
	this->m_done = true;
	this->m_thread.join ();
	close (this->m_listener);
    }

    [[nodiscard]] uint16_t port () const { return this->m_port; }

    std::vector<uint8_t> lastColors (const uint32_t device) {
	for (int i = 0; i < 200; i++) {
	    {
		const std::lock_guard lock (this->m_mutex);

		if (this->m_colors.contains (device)) {
		    return this->m_colors[device];
		}
	    }

	    std::this_thread::sleep_for (20ms);
	}

	return {};
    }

    std::vector<uint32_t> idsSeen () {
	const std::lock_guard lock (this->m_mutex);
	return this->m_ids;
    }

private:
    bool readExactly (const int fd, uint8_t* into, const size_t count) const {
	size_t got = 0;

	while (got < count && !this->m_done) {
	    pollfd wait = { .fd = fd, .events = POLLIN };

	    if (poll (&wait, 1, 50) != 1) {
		continue;
	    }

	    const ssize_t now = recv (fd, into + got, count - got, 0);

	    if (now <= 0) {
		return false;
	    }

	    got += static_cast<size_t> (now);
	}

	return got == count;
    }

    static void reply (const int fd, const uint32_t device, const uint32_t id, const std::vector<uint8_t>& payload) {
	const auto packet = OpenRgb::makePacket (device, id, payload);
	send (fd, packet.data (), packet.size (), MSG_NOSIGNAL);
    }

    void serve () {
	pollfd wait = { .fd = this->m_listener, .events = POLLIN };

	while (!this->m_done && poll (&wait, 1, 50) != 1) { }

	if (this->m_done) {
	    return;
	}

	const int fd = accept (this->m_listener, nullptr, nullptr);

	if (fd < 0) {
	    return;
	}

	while (!this->m_done) {
	    uint8_t raw[OpenRgb::kHeaderSize];

	    if (!this->readExactly (fd, raw, sizeof (raw))) {
		break;
	    }

	    const auto header = OpenRgb::parseHeader (raw);
	    std::vector<uint8_t> payload (header->size);

	    if (!this->readExactly (fd, payload.data (), payload.size ())) {
		break;
	    }

	    {
		const std::lock_guard lock (this->m_mutex);
		this->m_ids.push_back (header->id);
	    }

	    if (header->id == OpenRgb::RequestProtocolVersion) {
		std::vector<uint8_t> version;
		put32 (version, 5);
		reply (fd, 0, header->id, version);
	    } else if (header->id == OpenRgb::RequestControllerCount) {
		std::vector<uint8_t> count;
		put32 (count, 2);
		reply (fd, 0, header->id, count);
	    } else if (header->id == OpenRgb::RequestControllerData) {
		reply (
		    fd, header->device, header->id,
		    header->device == 0 ? controllerData ("Keys", { { "Keys", 6, 2, 3 } })
					: controllerData ("Strips", { { "A", 4 }, { "B", 2 } })
		);
	    } else if (header->id == OpenRgb::UpdateLeds && payload.size () >= 6) {
		const std::lock_guard lock (this->m_mutex);
		const uint32_t count = payload[4] | (payload[5] << 8);
		auto& colors = this->m_colors[header->device];

		colors.clear ();

		for (uint32_t i = 0; i < count; i++) {
		    colors.insert (colors.end (), payload.begin () + 6 + i * 4, payload.begin () + 9 + i * 4);
		}
	    }
	}

	close (fd);
    }

    int m_listener = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_done = false;
    std::thread m_thread;
    std::mutex m_mutex;
    std::map<uint32_t, std::vector<uint8_t>> m_colors;
    std::vector<uint32_t> m_ids;
};
}

TEST_CASE ("A controller description of protocol 3 is read") {
    const auto data = controllerData ("Board", { { "Main", 6, 2, 3 }, { "Strip", 4 } });
    const auto device = OpenRgb::parseController (data, 7);

    REQUIRE (device.has_value ());
    CHECK (device->index == 7);
    CHECK (device->name == "Board");
    CHECK (device->ledCount == 10);
    REQUIRE (device->zones.size () == 2);
    CHECK (device->zones[0].matrixWidth == 3);
    CHECK (device->zones[0].matrixHeight == 2);
    CHECK (device->zones[0].matrix.size () == 6);
    CHECK (device->zones[1].matrix.empty ());
    CHECK_FALSE (OpenRgb::parseController (std::span (data).first (data.size () / 2), 0).has_value ());
}

TEST_CASE ("Leds sit at the centre of their cell in the frame") {
    OpenRgb::Device device;

    device.ledCount = 6;
    device.zones.push_back (
	{ .name = "Keys", .leds = 6, .matrix = { 0, 1, 2, 3, 4, 5 }, .matrixWidth = 3, .matrixHeight = 2 }
    );

    const auto layout = layoutDevice (device);

    REQUIRE (layout.size () == 6);
    CHECK (layout[0].x == 13);
    CHECK (layout[1].x == 40);
    CHECK (layout[2].x == 66);
    CHECK (layout[0].y == 2);
    CHECK (layout[3].y == 7);
}

TEST_CASE ("Zones without a matrix are rows stacked top to bottom") {
    OpenRgb::Device device;

    device.ledCount = 6;
    device.zones.push_back ({ .name = "A", .leds = 4 });
    device.zones.push_back ({ .name = "B", .leds = 2 });

    const auto layout = layoutDevice (device);

    CHECK (layout[0].x == 10);
    CHECK (layout[3].x == 70);
    CHECK (layout[0].y == 2);
    CHECK (layout[4].x == 20);
    CHECK (layout[4].y == 7);
}

TEST_CASE ("A matrix zone below another one continues the rows") {
    OpenRgb::Device device;

    device.ledCount = 6;
    device.zones.push_back ({ .name = "A", .leds = 2 });
    device.zones.push_back ({ .name = "B", .leds = 4, .matrix = { 0, 1, 2, 3 }, .matrixWidth = 2, .matrixHeight = 2 });

    const auto layout = layoutDevice (device);

    REQUIRE (layout.size () == 6);
    CHECK (layout[0].y == 1);
    CHECK (layout[2].y == 5);
    CHECK (layout[4].y == 8);
    CHECK (layout[3].x == 60);
}

TEST_CASE ("A matrix cell naming a led the zone does not have is left out") {
    OpenRgb::Device device;

    device.ledCount = 3;
    device.zones.push_back ({ .name = "A", .leds = 2, .matrix = { 0, 1, 2 }, .matrixWidth = 3, .matrixHeight = 1 });

    const auto layout = layoutDevice (device);

    REQUIRE (layout.size () == 3);
    CHECK (layout[1].x == 40);
    CHECK (layout[2].x == 0);
    CHECK (layout[2].y == 0);
}

TEST_CASE ("Brightness stays inside the spans it was given") {
    std::vector<uint8_t> in = { 10, 20, 40, 200, 200, 200, 200, 200, 200 };
    std::vector<uint8_t> out (9, 7);

    applyBrightness (std::span (in).first (4), std::span (out).first (4), { .boost = false, .factor = 1.0f });
    CHECK (out == std::vector<uint8_t> { 10, 20, 40, 7, 7, 7, 7, 7, 7 });

    applyBrightness (std::span (in).first (3), std::span (out).first (3), { .boost = false, .factor = 1.0f });
    CHECK (out[3] == 7);
}

TEST_CASE ("Sampling a frame reads each led's own pixel in all three channels") {
    std::vector<uint8_t> frame (kFrameBytes);

    for (int y = 0; y < kFrameHeight; y++) {
	for (int x = 0; x < kFrameWidth; x++) {
	    const size_t at = (static_cast<size_t> (y) * kFrameWidth + x) * 3;

	    frame[at] = static_cast<uint8_t> (x);
	    frame[at + 1] = static_cast<uint8_t> (100 + y);
	    frame[at + 2] = static_cast<uint8_t> (200 + x % 50);
	}
    }

    const std::vector<LedPixel> layout = { { 5, 0 }, { 79, 9 }, { 40, 4 } };
    std::vector<uint8_t> out;

    sampleDevice (frame, layout, out);

    CHECK (out == std::vector<uint8_t> { 5, 100, 205, 79, 109, 229, 40, 104, 240 });
}

TEST_CASE ("Boost raises the strongest channel to full and the factor scales on top") {
    const std::vector<uint8_t> in = { 10, 20, 40, 0, 0, 0, 100, 50, 0 };
    std::vector<uint8_t> out (in.size ());

    applyBrightness (in, out, { .boost = true, .factor = 1.0f });
    CHECK (out == std::vector<uint8_t> { 63, 127, 255, 0, 0, 0, 255, 127, 0 });

    applyBrightness (in, out, { .boost = true, .factor = 0.5f });
    CHECK (out[2] == 127);

    applyBrightness (in, out, { .boost = false, .factor = 2.0f });
    CHECK (out == std::vector<uint8_t> { 20, 40, 80, 0, 0, 0, 200, 100, 0 });
}

TEST_CASE ("The output sends each device its leds' colors") {
    FakeServer server;
    LedOutput output;

    output.start ({ .host = "127.0.0.1", .port = server.port (), .brightness = { .boost = false, .factor = 1.0f } });

    std::vector<uint8_t> frame (kFrameBytes);

    for (int y = 0; y < kFrameHeight; y++) {
	for (int x = 0; x < kFrameWidth; x++) {
	    uint8_t* pixel = frame.data () + (static_cast<size_t> (y) * kFrameWidth + x) * 3;

	    pixel[x < 40 ? 0 : 2] = 200;
	}
    }

    for (int i = 0; i < 100 && output.deviceCount () < 2; i++) {
	std::this_thread::sleep_for (20ms);
    }

    REQUIRE (output.deviceCount () == 2);
    output.submit (frame);

    const auto keys = server.lastColors (0);
    const auto strips = server.lastColors (1);

    REQUIRE (keys.size () == 18);
    CHECK (keys[0] == 200);
    CHECK (keys[2] == 0);
    CHECK (keys[3] == 0);
    CHECK (keys[5] == 200);
    CHECK (keys[8] == 200);

    REQUIRE (strips.size () == 18);
    CHECK (strips[0] == 200);
    CHECK (strips[3] == 200);
    CHECK (strips[8] == 200);
    CHECK (strips[11] == 200);
    CHECK (strips[12] == 200);
    CHECK (strips[17] == 200);

    output.stop ();

    const auto ids = server.idsSeen ();
    CHECK (std::ranges::find (ids, uint32_t (OpenRgb::SetClientName)) != ids.end ());
    CHECK (std::ranges::find (ids, uint32_t (OpenRgb::SetCustomMode)) != ids.end ());
}

TEST_CASE ("The output keeps trying while no server is there") {
    LedOutput output;

    output.start ({ .host = "127.0.0.1", .port = 1 });
    std::this_thread::sleep_for (100ms);
    CHECK (output.running ());
    CHECK (output.deviceCount () == 0);
    output.stop ();
    CHECK_FALSE (output.running ());
}
