#pragma once

#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

// The characters a long name part keeps in its Windows 8.3 short name: upper case, no spaces or dots, the ones FAT
// doesn't allow (and every non-ASCII character, counted once per UTF-8 sequence) become '_'
inline std::string shortNameChars (std::string_view part) {
    std::string out;

    for (const unsigned char c : part) {
	if (c == ' ' || c == '.' || (c & 0xC0) == 0x80) {
	    continue;
	}

	if (c >= 0x80 || std::strchr ("+,;=[]", c) != nullptr) {
	    out += '_';
	} else {
	    out += static_cast<char> (std::toupper (c));
	}
    }

    return out;
}

// Which sibling a Windows 8.3 alias like "KONACH~1.TEX" stood for on the author's disk. Scenes sometimes reference
// those (3810078802), but packages and copies only carry the long names. Windows numbers the aliases sharing a
// basis and extension in creation order, which isn't stored anywhere, so the siblings are taken in name order.
inline std::optional<std::string> resolveShortNameAlias (std::vector<std::string> siblings, const std::string& wanted) {
    const auto dot = wanted.rfind ('.');
    const std::string stem = wanted.substr (0, dot);
    const std::string extension = dot == std::string::npos ? "" : shortNameChars (wanted.substr (dot + 1));
    const auto tilde = stem.rfind ('~');

    if (tilde == std::string::npos || tilde == 0 || stem.size () > 8 || extension.size () > 3
	|| tilde + 1 == stem.size ()
	|| !std::all_of (stem.begin () + tilde + 1, stem.end (), [] (unsigned char c) { return std::isdigit (c); })) {
	return std::nullopt;
    }

    const std::string basis = shortNameChars (stem.substr (0, tilde));
    const int index = std::stoi (stem.substr (tilde + 1));

    if (basis.empty () || index < 1) {
	return std::nullopt;
    }

    std::erase_if (siblings, [&] (const std::string& name) {
	const auto nameDot = name.rfind ('.');
	const std::string nameStem = name.substr (0, nameDot);
	const std::string nameExtension = nameDot == std::string::npos ? "" : name.substr (nameDot + 1);
	const auto isShortName = [] (const std::string& part) {
	    return std::ranges::equal (shortNameChars (part), part, [] (unsigned char x, unsigned char y) {
		return x == std::toupper (y);
	    });
	};
	// names that already fit 8.3 (in any case) never got an alias
	const bool hasAlias = nameStem.size () > 8 || nameExtension.size () > 3 || !isShortName (nameStem)
	    || !isShortName (nameExtension) || std::ranges::count (name, '.') > 1;

	if (!hasAlias) {
	    return true;
	}

	const std::string nameBasis = shortNameChars (nameStem);

	return shortNameChars (nameExtension).substr (0, 3) != extension || nameBasis.substr (0, basis.size ()) != basis
	    || (nameBasis.size () > basis.size () && basis.size () != 7 - (stem.size () - tilde - 1));
    });

    std::ranges::sort (siblings, [] (const std::string& a, const std::string& b) {
	return std::ranges::lexicographical_compare (a, b, [] (unsigned char x, unsigned char y) {
	    return std::toupper (x) < std::toupper (y);
	});
    });

    if (static_cast<size_t> (index) > siblings.size ()) {
	return std::nullopt;
    }

    return siblings[index - 1];
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

    [[nodiscard]] virtual std::vector<std::string> listFiles (const std::filesystem::path& dir) const { return {}; }
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