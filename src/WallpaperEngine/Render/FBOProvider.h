#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "CFBO.h"
#include "WallpaperEngine/Data/Model/Effect.h"

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class FBOProvider {
public:
    explicit FBOProvider (const FBOProvider* parent);

    /** An effect.json buffer for an object of the given size (sub_1401EA500) */
    std::shared_ptr<CFBO> create (const FBO& base, glm::vec2 objectSize, bool hdr);
    std::shared_ptr<CFBO> create (
	const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
	glm::vec2 textureSize, const glm::vec4& borderColor = { 0.0f, 0.0f, 0.0f, 1.0f }, uint32_t mipLevels = 1
    );
    std::shared_ptr<CFBO> alias (const std::string& newName, const std::string& original);
    [[nodiscard]] std::shared_ptr<CFBO> find (const std::string& name) const;

private:
    const FBOProvider* m_parent;
    std::map<std::string, std::shared_ptr<CFBO>> m_fbos = {};
};
}
