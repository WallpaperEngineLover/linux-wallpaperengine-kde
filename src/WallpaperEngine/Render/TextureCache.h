#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
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

    std::shared_ptr<const TextureProvider> resolve (const std::string& filename, const Data::Model::Project& project);
    [[nodiscard]] std::shared_ptr<const TextureProvider>
    findLoaded (const std::string& filename, const Data::Model::Project& project) const;

    /** The texture name of an icon the engine picked for a user shortcut; a name like it that comes from a wallpaper's
     * own files is not an icon and resolves like any other missing texture */
    [[nodiscard]] std::string iconTextureName (const std::filesystem::path& icon);

    void store (const std::string& name, std::shared_ptr<const TextureProvider> texture);

    void prune ();

    void repaintConditionalTextures ();

private:
    struct ConditionalTexture {
	std::weak_ptr<CTexture> texture;
	const Data::Model::Project* project = nullptr;
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
    std::set<std::string> m_iconNames = {};
    std::map<std::string, std::shared_ptr<const TextureProvider>> m_sharedTextures = {};
    std::map<std::pair<const Data::Model::Project*, std::string>, std::shared_ptr<const TextureProvider>> m_textureCache
	= {};
    std::function<void ()> m_mediaCallback;
};
}
