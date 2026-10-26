#include "OpenRgbProtocol.h"

#include <cstring>

namespace WallpaperEngine::Led::OpenRgb {
void put32 (std::vector<uint8_t>& out, const uint32_t value) {
    for (int i = 0; i < 4; i++) {
	out.push_back (static_cast<uint8_t> (value >> (8 * i)));
    }
}

namespace {
    class Reader {
    public:
	explicit Reader (const std::span<const uint8_t> bytes) : m_bytes (bytes) { }

	bool u16 (uint32_t& value) {
	    if (this->m_at + 2 > this->m_bytes.size ()) {
		return false;
	    }

	    value = this->m_bytes[this->m_at] | (this->m_bytes[this->m_at + 1] << 8);
	    this->m_at += 2;
	    return true;
	}

	bool u32 (uint32_t& value) {
	    if (this->m_at + 4 > this->m_bytes.size ()) {
		return false;
	    }

	    value = 0;

	    for (int i = 0; i < 4; i++) {
		value |= static_cast<uint32_t> (this->m_bytes[this->m_at + i]) << (8 * i);
	    }

	    this->m_at += 4;
	    return true;
	}

	bool skip (const size_t count) {
	    if (this->m_at + count > this->m_bytes.size ()) {
		return false;
	    }

	    this->m_at += count;
	    return true;
	}

	bool text (std::string& value) {
	    uint32_t length = 0;

	    if (!this->u16 (length) || this->m_at + length > this->m_bytes.size ()) {
		return false;
	    }

	    value.assign (reinterpret_cast<const char*> (this->m_bytes.data () + this->m_at), length);

	    while (!value.empty () && value.back () == '\0') {
		value.pop_back ();
	    }

	    this->m_at += length;
	    return true;
	}

    private:
	std::span<const uint8_t> m_bytes;
	size_t m_at = 0;
    };
}

std::vector<uint8_t> makePacket (const uint32_t device, const uint32_t id, const std::span<const uint8_t> payload) {
    std::vector<uint8_t> out = { 'O', 'R', 'G', 'B' };

    out.reserve (kHeaderSize + payload.size ());
    put32 (out, device);
    put32 (out, id);
    put32 (out, static_cast<uint32_t> (payload.size ()));
    out.insert (out.end (), payload.begin (), payload.end ());
    return out;
}

std::optional<Header> parseHeader (const std::span<const uint8_t> bytes) {
    if (bytes.size () < kHeaderSize || std::memcmp (bytes.data (), "ORGB", 4) != 0) {
	return std::nullopt;
    }

    Header header;
    const auto read = [&] (const size_t at) {
	return static_cast<uint32_t> (bytes[at]) | (static_cast<uint32_t> (bytes[at + 1]) << 8)
	    | (static_cast<uint32_t> (bytes[at + 2]) << 16) | (static_cast<uint32_t> (bytes[at + 3]) << 24);
    };

    header.device = read (4);
    header.id = read (8);
    header.size = read (12);
    return header;
}

std::optional<Device> parseController (const std::span<const uint8_t> payload, const uint32_t index) {
    Reader in (payload);
    Device device;
    std::string ignored;
    uint32_t packetSize = 0;
    uint32_t deviceType = 0;
    uint32_t value = 0;
    uint32_t count = 0;

    device.index = index;

    if (!in.u32 (packetSize) || !in.u32 (deviceType) || !in.text (device.name)) {
	return std::nullopt;
    }

    for (int i = 0; i < 5; i++) {
	if (!in.text (ignored)) {
	    return std::nullopt;
	}
    }

    if (!in.u16 (count) || !in.skip (4)) {
	return std::nullopt;
    }

    for (uint32_t mode = 0; mode < count; mode++) {
	// value, flags, speed and brightness and color count ranges, speed, brightness, direction, color mode: 12 words
	if (!in.text (ignored) || !in.skip (4 * 12) || !in.u16 (value) || !in.skip (static_cast<size_t> (value) * 4)) {
	    return std::nullopt;
	}
    }

    if (!in.u16 (count)) {
	return std::nullopt;
    }

    for (uint32_t i = 0; i < count; i++) {
	Zone zone;
	uint32_t matrixBytes = 0;

	if (!in.text (zone.name) || !in.skip (4) || !in.skip (8) || !in.u32 (zone.leds) || !in.u16 (matrixBytes)) {
	    return std::nullopt;
	}

	if (matrixBytes > 0) {
	    if (!in.u32 (zone.matrixHeight) || !in.u32 (zone.matrixWidth)
		|| static_cast<uint64_t> (zone.matrixHeight) * zone.matrixWidth > 65536) {
		return std::nullopt;
	    }

	    zone.matrix.resize (static_cast<size_t> (zone.matrixHeight) * zone.matrixWidth);

	    for (auto& cell : zone.matrix) {
		if (!in.u32 (cell)) {
		    return std::nullopt;
		}
	    }
	}

	device.ledCount += zone.leds;
	device.zones.push_back (std::move (zone));
    }

    return device;
}

std::vector<uint8_t> makeUpdateLeds (const uint32_t device, const std::span<const uint8_t> rgb) {
    const auto leds = static_cast<uint32_t> (rgb.size () / 3);
    std::vector<uint8_t> payload;

    payload.reserve (6 + leds * 4);
    put32 (payload, 6 + leds * 4);
    payload.push_back (static_cast<uint8_t> (leds));
    payload.push_back (static_cast<uint8_t> (leds >> 8));

    for (uint32_t i = 0; i < leds; i++) {
	payload.push_back (rgb[i * 3]);
	payload.push_back (rgb[i * 3 + 1]);
	payload.push_back (rgb[i * 3 + 2]);
	payload.push_back (0);
    }

    return makePacket (device, UpdateLeds, payload);
}
}
