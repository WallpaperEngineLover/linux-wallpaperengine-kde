#include "FBOProvider.h"

#include <algorithm>
#include <cctype>
#include <map>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

namespace {
// same names and fallback as wallpaper64.exe's lookup table (sub_140172ED0). the backbuffer formats
// only switch to half floats in HDR scenes, which aren't supported here, so they stay 8-bit
TextureFormat parseFormat (std::string name) {
    static const std::map<std::string, TextureFormat> formats = {
	{ "rgba8888", TextureFormat_ARGB8888 },
	{ "rgba_backbuffer", TextureFormat_ARGB8888 },
	{ "rgb888", TextureFormat_RGB888 },
	{ "rgb_backbuffer", TextureFormat_RGB888 },
	{ "rgb565", TextureFormat_RGB565 },
	{ "rg88", TextureFormat_RG88 },
	{ "r8", TextureFormat_R8 },
	{ "rg1616f", TextureFormat_RG1616f },
	{ "r16f", TextureFormat_R16f },
	{ "rgba16161616f", TextureFormat_RGBA16161616f },
	{ "rgb161616f", TextureFormat_RGB161616f },
    };

    std::ranges::transform (name, name.begin (), [] (const unsigned char c) { return std::tolower (c); });

    const auto it = formats.find (name);
    return it != formats.end () ? it->second : TextureFormat_ARGB8888;
}
} // namespace

FBOProvider::FBOProvider (const FBOProvider* parent) : m_parent (parent) { }

std::shared_ptr<CFBO> FBOProvider::create (const FBO& base, uint32_t flags, const glm::vec2 size) {
    return this->m_fbos[base.name] = std::make_shared<CFBO> (
	       base.name, parseFormat (base.format), flags, base.scale, size.x / base.scale, size.y / base.scale, size.x / base.scale,
	       size.y / base.scale
	   );
}

std::shared_ptr<CFBO> FBOProvider::create (
    const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
    glm::vec2 textureSize, const glm::vec4& borderColor
) {
    return this->m_fbos[name] = std::make_shared<CFBO> (
	       name, format, flags, scale, realSize.x, realSize.y, textureSize.x, textureSize.y,
	       borderColor
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