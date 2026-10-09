#include "AssetLocator.h"

#include "AssetLoadException.h"

using namespace WallpaperEngine::Assets;

AssetLocator::AssetLocator (ContainerUniquePtr filesystem) : m_filesystem (std::move (filesystem)) { }

std::string AssetLocator::shader (const std::filesystem::path& filename) const {
    // assets/zcompat shader replacements are mobile-only (wallpaperui.exe), wallpaper64.exe never reads them
    try {
	return this->m_filesystem->readString ("shaders" / filename);
    } catch (std::filesystem::filesystem_error& base) {
	throw AssetLoadException (base);
    }
}

std::string AssetLocator::fragmentShader (const std::filesystem::path& filename) const {
    auto final = filename;

    final.replace_extension ("frag");

    return this->shader (final);
}

std::string AssetLocator::vertexShader (const std::filesystem::path& filename) const {
    auto final = filename;

    final.replace_extension ("vert");

    return this->shader (final);
}

std::string AssetLocator::geometryShader (const std::filesystem::path& filename) const {
    auto final = filename;

    final.replace_extension ("geom");

    return this->shader (final);
}

std::string AssetLocator::includeShader (const std::filesystem::path& filename) const {
    auto final = filename;

    final.replace_extension ("h");

    return this->shader (final);
}

std::string AssetLocator::readString (const std::filesystem::path& filename) const {
    try {
	return this->m_filesystem->readString (filename);
    } catch (std::filesystem::filesystem_error& base) {
	throw AssetLoadException (base);
    }
}

ReadStreamSharedPtr AssetLocator::texture (const std::filesystem::path& filename) const {
    const auto final = std::filesystem::path ("materials") / filename.string ().append (".tex");

    try {
	return this->m_filesystem->read (final);
    } catch (std::filesystem::filesystem_error& base) {
	throw AssetLoadException (base);
    }
}

ReadStreamSharedPtr AssetLocator::read (const std::filesystem::path& path) const {
    try {
	return this->m_filesystem->read (path);
    } catch (std::filesystem::filesystem_error& base) {
	throw AssetLoadException (base);
    }
}

std::filesystem::path AssetLocator::physicalPath (const std::filesystem::path& path) const {
    try {
	return this->m_filesystem->physicalPath (path);
    } catch (std::filesystem::filesystem_error& base) {
	throw AssetLoadException (base);
    }
}

std::optional<std::filesystem::path>
AssetLocator::resolveWorkshopDependencyAlias (const std::filesystem::path& path) const {
    return this->m_filesystem->resolveWorkshopDependencyAlias (path);
}