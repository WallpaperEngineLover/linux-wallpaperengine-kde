#pragma once

#include "Adapters/Types.h"
#include "Adapters/Virtual.h"

#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include <filesystem>

namespace WallpaperEngine::FileSystem {
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::FileSystem::Adapters;

class Container {
public:
    Container ();
    ~Container () = default;

    [[nodiscard]] ReadStreamSharedPtr read (const std::filesystem::path& path) const;

    [[nodiscard]] std::string readString (const std::filesystem::path& path) const;

    // the real filesystem path, for tools like mpv
    [[nodiscard]] std::filesystem::path physicalPath (const std::filesystem::path& path) const;

    AdapterSharedPtr mount (const std::filesystem::path& path, const std::filesystem::path& mountPoint);

    VirtualAdapter& getVFS () const;

    void registerAdapterFactory (FactoryUniquePtr factory);

    // workshop-dependency-prefixed variant across all adapters, fallback when the literal path fails
    [[nodiscard]] std::optional<std::filesystem::path>
    resolveWorkshopDependencyAlias (const std::filesystem::path& path) const;

    /** The long-named sibling a Windows 8.3 alias (KONACH~1.tex) in path stood for, see resolveShortNameAlias */
    [[nodiscard]] std::optional<std::filesystem::path> resolveShortNameAlias (const std::filesystem::path& path) const;

private:
    Adapter& resolveAdapterForFile (const std::filesystem::path& path) const;
    std::vector<FactoryUniquePtr> m_factories;
    std::vector<std::pair<std::filesystem::path, AdapterSharedPtr>> m_mountpoints;
    std::shared_ptr<VirtualAdapter> m_vfs;
};

using ContainerUniquePtr = std::unique_ptr<Container>;
}