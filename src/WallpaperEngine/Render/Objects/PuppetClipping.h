#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Objects {
// puppet mesh clipping masks (MDLV 23+) like 2.8.42: part ranges and records from sub_140261880, split into draws and a
// command list by sub_14020AE00/sub_14020B720, walked by sub_140208670/sub_140208C80
struct PuppetClipping {
    enum RecordFlags : uint32_t {
	/** the targets are drawn additively instead of translucent */
	Additive = 0x1,
	/** the mask is inverted: cleared to white, the shader flips it (g_RenderVar0.x) */
	Inverted = 0x2,
	/** the sources only make the mask, they aren't drawn themselves */
	HideSources = 0x4,
	AtTargets = 0x8,
    };

    struct Part {
	/** bone whose MDLA v6 draw order track moves the part (mesh flag 8) */
	uint32_t bone = 0;
	uint32_t order = 0;
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
    };

    struct Record {
	/** texture name relative to materials/, sampled through the mesh UVs */
	std::string mask;
	uint32_t flags = 0;
	/** parts drawn through the mask */
	std::vector<uint32_t> targets;
	/** parts that make the mask */
	std::vector<uint32_t> sources;
	/** the record whose targets contain every source of this one, its mask is multiplied in */
	int parent = -1;
    };

    struct Draw {
	uint32_t offset = 0;
	uint32_t count = 0;
    };

    enum Command : int {
	/** draw the next draw normally */
	PlainDraw = 0,
	/** record: its mask from the next draw, the targets with the one after */
	Mask = 1,
	/** chain length, (record, mask draw) per ancestor, record: like Mask with the ancestors' masks multiplied in */
	NestedMask = 2,
    };

    // what follows a puppet's index buffer (offset = its end), nullopt without clipping records
    static std::optional<PuppetClipping>
    read (const std::vector<char>& data, size_t offset, int version, size_t indexCount);
    /** Only the part ranges (MDLV 21+), what the animated draw order sorts */
    static std::vector<Part> readParts (const std::vector<char>& data, size_t offset, int version, size_t indexCount);

    void build (const std::vector<uint16_t>& meshIndices);

    std::vector<Part> parts;
    /** parts in drawing order (indices into parts), file order unless the draw order animates; build () walks it */
    std::vector<uint32_t> order;
    std::vector<Record> records;
    std::vector<uint16_t> indices;
    std::vector<Draw> draws;
    std::vector<int> commands;
};
} // namespace WallpaperEngine::Render::Objects
