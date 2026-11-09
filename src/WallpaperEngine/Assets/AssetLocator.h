#pragma once

#include "WallpaperEngine/FileSystem/Container.h"

namespace WallpaperEngine::Assets {
using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::Data::Model;
class AssetLocator {
public:
    explicit AssetLocator (ContainerUniquePtr filesystem);

    [[nodiscard]] std::string vertexShader (const std::filesystem::path& filename) const;
    [[nodiscard]] std::string fragmentShader (const std::filesystem::path& filename) const;
    [[nodiscard]] std::string geometryShader (const std::filesystem::path& filename) const;
    [[nodiscard]] std::string includeShader (const std::filesystem::path& filename) const;
    [[nodiscard]] ReadStreamSharedPtr texture (const std::filesystem::path& filename) const;
    [[nodiscard]] std::string readString (const std::filesystem::path& filename) const;
    [[nodiscard]] ReadStreamSharedPtr read (const std::filesystem::path& path) const;
    [[nodiscard]] std::filesystem::path physicalPath (const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<std::filesystem::path>
    resolveWorkshopDependencyAlias (const std::filesystem::path& path) const;

private:
    [[nodiscard]] std::string shader (const std::filesystem::path& filename) const;

    ContainerUniquePtr m_filesystem;
};

using AssetLocatorUniquePtr = std::unique_ptr<AssetLocator>;
}