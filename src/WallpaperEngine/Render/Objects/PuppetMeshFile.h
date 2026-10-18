#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace WallpaperEngine::Render::Objects {
class PuppetFileReader {
public:
    PuppetFileReader (const std::vector<char>& data, size_t offset) : m_data (data), m_offset (offset) { }

    template <typename T> T next () {
	if (this->m_offset + sizeof (T) > this->m_data.size ()) {
	    throw std::runtime_error ("truncated puppet file");
	}

	T value;
	std::memcpy (&value, this->m_data.data () + this->m_offset, sizeof (T));
	this->m_offset += sizeof (T);
	return value;
    }

    void string () {
	while (this->next<char> () != '\0') { }
    }

    std::string text () {
	std::string result;

	for (char c = this->next<char> (); c != '\0'; c = this->next<char> ()) {
	    result.push_back (c);
	}

	return result;
    }

    std::pair<size_t, uint32_t> blob () {
	const auto bytes = this->next<uint32_t> ();
	const size_t start = this->m_offset;

	if (start + bytes > this->m_data.size ()) {
	    throw std::runtime_error ("truncated puppet file");
	}

	this->m_offset += bytes;
	return { start, bytes };
    }

    void skip (size_t bytes) { this->m_offset += bytes; }

    [[nodiscard]] size_t offset () const { return this->m_offset; }

private:
    const std::vector<char>& m_data;
    size_t m_offset;
};

/** headerOffset is 8 bytes before the vertices */
struct PuppetMeshBlock {
    size_t headerOffset = 0;
    uint32_t vertexBytes = 0;
    uint32_t indexBytes = 0;
};

struct PuppetVertexLayout {
    PuppetMeshBlock block;
    size_t vertexStride = 0;
    size_t uvOffset = 0;
};

/** First mesh of a puppet .mdl, read like sub_140261880 where the header holds up */
struct PuppetMeshFile {
    int version = 0;
    std::string tag;
    /** file size without MDLS */
    size_t skeleton = 0;
    /** after the meshes (MDLS, MDAT, MDLA... in any order), or the skeleton when the meshes don't parse */
    size_t sections = 0;
    uint32_t meshCount = 0;
    PuppetVertexLayout layout;
    /** false when the stride was guessed */
    bool declaredLayout = false;
    size_t indexEnd = 0;
    /** flags: 4 bone alpha, 8 draw order, 0x1000 morph alpha; 0 when the header doesn't match the mesh */
    uint32_t flags = 0;
    uint32_t format = 0;
    std::vector<float> positions;
    std::vector<float> texcoords;
    std::vector<uint16_t> indices;
    std::vector<glm::uvec4> blendIndices;
    std::vector<glm::vec4> blendWeights;
    std::vector<float> auxPositions;
    std::vector<float> normals;
    std::vector<float> tangents;
    /** position.w with format 0x10000, none where w <= 0 */
    std::vector<std::optional<uint32_t>> morphIndices;
    std::vector<std::string> warnings;
};

/** One MDLV mesh as sub_140261880 reads it */
struct MdlvMeshRecord {
    std::vector<std::string> materials;
    uint32_t flags = 0;
    std::optional<std::pair<glm::vec3, glm::vec3>> bounds;
    uint32_t format = 0;
    std::pair<size_t, uint32_t> vertices;
    std::pair<size_t, uint32_t> indices;
};

struct MdlvMeshes {
    int version = 0;
    std::vector<MdlvMeshRecord> meshes;
    size_t end = 0;
};

/** throws std::runtime_error on another tag or a truncated file */
MdlvMeshes readMdlvMeshes (const std::vector<char>& data);

/** Sections are "TAGnnnn\0" + absolute offset of the next, until an empty tag or EOF */
std::optional<size_t> findMdlSection (const std::vector<char>& data, size_t first, std::string_view tag);

/** nullopt when no usable mesh is found, never throws */
std::optional<PuppetMeshFile> readPuppetMeshFile (const std::vector<char>& data);

/** Like WE's loader; the first "MDLS" past the first mesh when the meshes don't parse, file size without one */
size_t findPuppetSkeleton (const std::vector<char>& data);
}
