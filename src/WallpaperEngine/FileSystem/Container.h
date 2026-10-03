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

    /**
     * Tries to resolve the given file into an absolute, real, filesystem path
     * to be used as info for other tools that might require it (like MPV)
     *
     * @param path The path to resolve to a real file
     * @return The full public, absolute path to the given file
     */
    [[nodiscard]] std::filesystem::path physicalPath (const std::filesystem::path& path) const;

    /**
     *
     * @param path Base of the mountpoint
     * @param mountPoint Where in the VFS to mount it
     */
    AdapterSharedPtr mount (const std::filesystem::path& path, const std::filesystem::path& mountPoint);

    VirtualAdapter& getVFS () const;

    void registerAdapterFactory (FactoryUniquePtr factory);

    /** Looks for a workshop-dependency-prefixed variant of path across every mounted adapter, only as a fallback when
     * the literal path fails */
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