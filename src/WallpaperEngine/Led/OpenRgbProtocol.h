#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace WallpaperEngine::Led {
namespace OpenRgb {
    constexpr uint32_t kProtocolVersion = 3;
    constexpr size_t kHeaderSize = 16;

    enum PacketId : uint32_t {
	RequestControllerCount = 0,
	RequestControllerData = 1,
	RequestProtocolVersion = 40,
	SetClientName = 50,
	DeviceListUpdated = 100,
	UpdateLeds = 1050,
	SetCustomMode = 1100,
    };

    void put32 (std::vector<uint8_t>& out, uint32_t value);

    struct Header {
	uint32_t device = 0;
	uint32_t id = 0;
	uint32_t size = 0;
    };

    struct Zone {
	std::string name;
	uint32_t leds = 0;
	/** row-major led indices inside the zone, 0xFFFFFFFF for an empty cell; empty for a linear zone */
	std::vector<uint32_t> matrix;
	uint32_t matrixWidth = 0;
	uint32_t matrixHeight = 0;
    };

    struct Device {
	uint32_t index = 0;
	std::string name;
	uint32_t ledCount = 0;
	std::vector<Zone> zones;
    };

    [[nodiscard]] std::vector<uint8_t> makePacket (uint32_t device, uint32_t id, std::span<const uint8_t> payload = {});
    [[nodiscard]] std::optional<Header> parseHeader (std::span<const uint8_t> bytes);
    [[nodiscard]] std::optional<Device> parseController (std::span<const uint8_t> payload, uint32_t index);
    [[nodiscard]] std::vector<uint8_t> makeUpdateLeds (uint32_t device, std::span<const uint8_t> rgb);
}
}
