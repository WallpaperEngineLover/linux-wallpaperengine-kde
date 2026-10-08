#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "TextureProvider.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/RenderContext.h"

using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render {
class AlbumTexture;
class CTexture;
namespace Helpers {
    class ContextAware;
}

class RenderContext;

class TextureCache final : Helpers::ContextAware {
public:
    explicit TextureCache (RenderContext& context);
    ~TextureCache () override;

    /** Returns the texture for filename as seen by the given project, cached per project since wallpapers reuse file
     * names */
    std::shared_ptr<const TextureProvider> resolve (const std::string& filename, const Data::Model::Project& project);
    /** nullptr unless already loaded for that project */
    [[nodiscard]] std::shared_ptr<const TextureProvider>
    findLoaded (const std::string& filename, const Data::Model::Project& project) const;

    /** Registers a texture that is shared by every project (media thumbnails and such) */
    void store (const std::string& name, std::shared_ptr<const TextureProvider> texture);

    /** Drops every cached texture that no wallpaper is using anymore */
    void prune ();

    /** TEXB0004 textures whose conditions changed */
    void repaintConditionalTextures ();

private:
    struct ConditionalTexture {
	std::weak_ptr<CTexture> texture;
	const Data::Model::Project* project;
	std::string filename;
	std::vector<std::function<void ()>> listeners = {};
	bool dirty = false;

	~ConditionalTexture () {
	    for (const auto& deregister : this->listeners) {
		deregister ();
	    }
	}
    };

    void followConditions (
	const std::shared_ptr<CTexture>& texture, const Data::Model::Project& project, const std::string& filename,
	const std::vector<std::string>& properties
    );

    std::vector<std::unique_ptr<ConditionalTexture>> m_conditionalTextures = {};
    std::shared_ptr<const AlbumTexture> m_previousThumbnail = nullptr;
    std::shared_ptr<const AlbumTexture> m_currentThumbnail = nullptr;
    std::map<std::string, std::shared_ptr<const TextureProvider>> m_sharedTextures = {};
    std::map<std::pair<const Data::Model::Project*, std::string>, std::shared_ptr<const TextureProvider>> m_textureCache
	= {};
    std::function<void ()> m_mediaCallback;
};
} // namespace WallpaperEngine::Render
