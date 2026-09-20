#include "TextureCache.h"

#include "AlbumTexture.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include "CTexture.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Assets;

TextureCache::TextureCache (RenderContext& context) : Helpers::ContextAware (context) {
    this->m_currentThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_currentThumbnail->getTextureID (0), -1, "$mediaThumbnail");
#endif

    this->m_previousThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_previousThumbnail->getTextureID (0), -1, "$mediaPreviousThumbnail");
#endif

    this->m_currentThumbnail->load ();

    this->store ("$mediaThumbnail", this->m_currentThumbnail);
    this->store ("$mediaPreviousThumbnail", this->m_previousThumbnail);

    this->m_mediaCallback = this->getContext ().getMediaSource ().addAlbumArtListener (
	[this] (const Media::MediaSource::MediaInfo& data) {
	    if (this->m_currentThumbnail->isReady ()) {
		this->m_previousThumbnail->copyContents (*this->m_currentThumbnail);
	    }

	    this->m_currentThumbnail->load ();
	}
    );
}

TextureCache::~TextureCache () { this->m_mediaCallback (); }

std::shared_ptr<const TextureProvider> TextureCache::resolve (const std::string& filename, const Project& requester) {
    if (const auto shared = this->m_sharedTextures.find (filename); shared != this->m_sharedTextures.end ()) {
	return shared->second;
    }

    const auto key = std::make_pair (&requester, filename);

    if (const auto found = this->m_textureCache.find (key); found != this->m_textureCache.end ()) {
	return found->second;
    }

    const auto load = [&] (const Project& project) {
	const auto contents = project.assetLocator->texture (filename);
	auto stream = BinaryReader (contents);

	auto metadataLoader = [&project] (const std::string& metaFilename) -> std::string {
	    std::filesystem::path fullPath = std::filesystem::path ("materials") / metaFilename;
	    return project.assetLocator->readString (fullPath);
	};

	auto parsedTexture = TextureParser::parse (stream, filename, metadataLoader);
	auto texture = std::make_shared<CTexture> (this->getContext (), std::move (parsedTexture));

#if !NDEBUG
	glObjectLabel (GL_TEXTURE, texture->getTextureID (0), -1, filename.c_str ());
#endif

	this->m_textureCache.insert_or_assign (key, texture);

	return texture;
    };

    try {
	return load (requester);
    } catch (AssetLoadException&) {
	// not shipped by the requesting project itself, it might still live in another loaded one
    }

    for (const auto& project : this->getContext ().getApp ().getBackgrounds () | std::views::values) {
	if (project.get () == &requester) {
	    continue;
	}

	try {
	    return load (*project);
	} catch (AssetLoadException&) {
	    // ignored, this happens if we're looking at the wrong background
	}
    }

    // TODO: fill in with a checkered pattern texture instead?
    throw AssetLoadException ("Cannot find file", filename, std::error_code ());
}

void TextureCache::store (const std::string& name, std::shared_ptr<const TextureProvider> texture) {
    this->m_sharedTextures.insert_or_assign (name, texture);
}

void TextureCache::prune () {
    const auto& backgrounds = this->getContext ().getApp ().getBackgrounds ();

    // entries of a gone project must not be inherited by a new one allocated at the same address
    std::erase_if (this->m_textureCache, [&backgrounds] (const auto& entry) {
	const auto alive = std::ranges::any_of (backgrounds, [&entry] (const auto& background) {
	    return background.second.get () == entry.first.first;
	});

	return !alive || entry.second.use_count () == 1;
    });
}
