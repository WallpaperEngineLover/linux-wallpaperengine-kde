#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "TextureProvider.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/RenderContext.h"

using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render {
class AlbumTexture;
namespace Helpers {
    class ContextAware;
}

class RenderContext;

class TextureCache final : Helpers::ContextAware {
public:
    explicit TextureCache (RenderContext& context);
    ~TextureCache () override;

    /** Returns the cached texture for filename, loading it from the containers first if needed */
    std::shared_ptr<const TextureProvider> resolve (const std::string& filename);

    void store (const std::string& name, std::shared_ptr<const TextureProvider> texture);

private:
    std::shared_ptr<const AlbumTexture> m_previousThumbnail = nullptr;
    std::shared_ptr<const AlbumTexture> m_currentThumbnail = nullptr;
    std::map<std::string, std::shared_ptr<const TextureProvider>> m_textureCache = {};
    std::function<void ()> m_mediaCallback;
};
} // namespace WallpaperEngine::Render
