#include "FBOProvider.h"

#include <algorithm>
#include <cctype>
#include <map>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

namespace {
// sub_1401E7170: backbuffer names (any case) are the scene's format, the rest go through sub_1401E53A0's table,
// unknown names are RGBA8
TextureFormat parseFormat (const std::string& name, const bool hdr) {
    static const std::map<std::string, TextureFormat> formats = {
	{ "rgba8888", TextureFormat_ARGB8888 },
	{ "rgb888", TextureFormat_RGB888 },
	{ "rg88", TextureFormat_RG88 },
	{ "r8", TextureFormat_R8 },
	{ "rgb565", TextureFormat_RGB565 },
	{ "bc7", TextureFormat_BC7 },
	{ "dxt5", TextureFormat_DXT5 },
	{ "dxt3", TextureFormat_DXT3 },
	{ "dxt1", TextureFormat_DXT1 },
	{ "rgba16161616f", TextureFormat_RGBA16161616f },
	{ "rgb161616f", TextureFormat_RGB161616f },
	{ "rg1616f", TextureFormat_RG1616f },
	{ "r16f", TextureFormat_R16f },
	{ "rgba16161616", TextureFormat_RGBA16161616 },
	{ "rgb161616", TextureFormat_RGB161616 },
	{ "rgba16161616S", TextureFormat_RGBA16161616S },
	{ "rgb161616S", TextureFormat_RGB161616S },
	{ "rgba8888s", TextureFormat_RGBA8888S },
	{ "rgba1010102", TextureFormat_RGBa1010102 },
    };

    const auto equalsIgnoringCase = [&name] (const std::string_view other) {
	return std::ranges::equal (name, other, [] (const unsigned char a, const unsigned char b) {
	    return std::tolower (a) == std::tolower (b);
	});
    };

    if (equalsIgnoringCase ("rgba_backbuffer")) {
	return hdr ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;
    }

    if (equalsIgnoringCase ("rgb_backbuffer")) {
	return hdr ? TextureFormat_RGB161616f : TextureFormat_RGB888;
    }

    const auto it = formats.find (name);
    return it != formats.end () ? it->second : TextureFormat_ARGB8888;
}
} // namespace

FBOProvider::FBOProvider (const FBOProvider* parent) : m_parent (parent) { }

std::shared_ptr<CFBO> FBOProvider::create (const FBO& base, const glm::vec2 objectSize, const bool hdr) {
    // clamped unless uvs is "repeat" (sub_1401EA500)
    const uint32_t flags = base.repeat ? TextureFlags_NoFlags : TextureFlags_ClampUVs;
    const glm::uvec2 size = base.bufferSize (objectSize);

    return this->m_fbos[base.name] = std::make_shared<CFBO> (
	       base.name, parseFormat (base.format, hdr), flags, std::max (base.scale, 1.0f), size.x, size.y, size.x,
	       size.y
	   );
}

std::shared_ptr<CFBO> FBOProvider::create (
    const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
    glm::vec2 textureSize, const glm::vec4& borderColor, const uint32_t mipLevels
) {
    return this->m_fbos[name] = std::make_shared<CFBO> (
	       name, format, flags, scale, realSize.x, realSize.y, textureSize.x, textureSize.y, borderColor, mipLevels
	   );
}

std::shared_ptr<CFBO> FBOProvider::alias (const std::string& newName, const std::string& original) {
    return this->m_fbos[newName] = this->m_fbos[original];
}

std::shared_ptr<CFBO> FBOProvider::find (const std::string& name) const {
    if (const auto it = this->m_fbos.find (name); it != this->m_fbos.end ()) {
	return it->second;
    }

    if (this->m_parent == nullptr) {
	return nullptr;
    }

    return this->m_parent->find (name);
}