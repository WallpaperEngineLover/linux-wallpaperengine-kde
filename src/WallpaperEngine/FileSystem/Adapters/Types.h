#pragma once

#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include <filesystem>
#include <optional>
#include <string>

namespace WallpaperEngine::FileSystem::Adapters {
using namespace WallpaperEngine::Data::Utils;

// Whether candidate is wanted with one "workshop/<anything>" segment inserted before the basename.
// Scripts reference bundled workshop-dependency assets by their original, un-prefixed name.
inline bool isWorkshopDependencyAliasOf (const std::string& candidate, const std::filesystem::path& wanted) {
    const std::string dir = wanted.parent_path ().generic_string ();
    const std::string basename = wanted.filename ().string ();
    const std::string prefix = (dir.empty () ? std::string () : dir + "/") + "workshop/";
    const std::string suffix = "/" + basename;

    if (candidate.size () <= prefix.size () + suffix.size () || !candidate.starts_with (prefix)
	|| !candidate.ends_with (suffix)) {
	return false;
    }

    const std::string middle = candidate.substr (prefix.size (), candidate.size () - prefix.size () - suffix.size ());
    return !middle.empty () && middle.find ('/') == std::string::npos;
}

struct Adapter {
    Adapter () = default;
    virtual ~Adapter () = default;

    [[nodiscard]] virtual ReadStreamSharedPtr open (const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual bool exists (const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual std::filesystem::path physicalPath (const std::filesystem::path& path) const = 0;

    /** Finds a workshop-dependency-prefixed variant of path in this adapter, only when exactly one matches */
    [[nodiscard]] virtual std::optional<std::filesystem::path>
    resolveWorkshopDependencyAlias (const std::filesystem::path& path) const {
	return std::nullopt;
    }
};

using AdapterSharedPtr = std::shared_ptr<Adapter>;

struct Factory {
    Factory () = default;
    virtual ~Factory () = default;

    [[nodiscard]] virtual bool handlesMountpoint (const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual AdapterSharedPtr create (const std::filesystem::path& path) const = 0;
};

using FactoryUniquePtr = std::unique_ptr<Factory>;
} // namespace WallpaperEngine::FileSystem::Adapters