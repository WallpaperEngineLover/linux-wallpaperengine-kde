#pragma once

#include <filesystem>
#include <utility>

#include "Types.h"

namespace WallpaperEngine::FileSystem::Adapters {
struct DirectoryFactory final : Factory {
    DirectoryFactory () = default;

    [[nodiscard]] bool handlesMountpoint (const std::filesystem::path& path) const override;
    [[nodiscard]] AdapterSharedPtr create (const std::filesystem::path& path) const override;
};

struct DirectoryAdapter final : Adapter {
    explicit DirectoryAdapter (std::filesystem::path path) : basepath (std::move (path)) { }

    [[nodiscard]] ReadStreamSharedPtr open (const std::filesystem::path& path) const override;
    [[nodiscard]] bool exists (const std::filesystem::path& path) const override;
    [[nodiscard]] std::filesystem::path physicalPath (const std::filesystem::path& path) const override;
    [[nodiscard]] std::optional<std::filesystem::path>
    resolveWorkshopDependencyAlias (const std::filesystem::path& path) const override;
    [[nodiscard]] std::vector<std::string> listFiles (const std::filesystem::path& dir) const override;

    const std::filesystem::path basepath;
};
}
