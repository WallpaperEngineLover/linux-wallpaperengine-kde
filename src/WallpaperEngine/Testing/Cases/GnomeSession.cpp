#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <optional>
#include <string>

#include "WallpaperEngine/Desktop/GnomeShell.h"

using WallpaperEngine::Desktop::isGnomeSession;

namespace {
bool withDesktop (const char* value) {
    const char* previous = std::getenv ("XDG_CURRENT_DESKTOP");
    const std::optional<std::string> saved = previous ? std::optional<std::string> (previous) : std::nullopt;

    if (value) {
	setenv ("XDG_CURRENT_DESKTOP", value, 1);
    } else {
	unsetenv ("XDG_CURRENT_DESKTOP");
    }

    const bool result = isGnomeSession ();

    if (saved) {
	setenv ("XDG_CURRENT_DESKTOP", saved->c_str (), 1);
    } else {
	unsetenv ("XDG_CURRENT_DESKTOP");
    }

    return result;
}
} // namespace

TEST_CASE ("isGnomeSession finds GNOME anywhere in the XDG_CURRENT_DESKTOP list") {
    CHECK (withDesktop ("GNOME"));
    CHECK (withDesktop ("ubuntu:GNOME"));
    CHECK (withDesktop ("GNOME-Classic:GNOME"));
    CHECK (withDesktop ("GNOME:extra"));
}

TEST_CASE ("isGnomeSession only takes whole entries") {
    CHECK_FALSE (withDesktop (nullptr));
    CHECK_FALSE (withDesktop (""));
    CHECK_FALSE (withDesktop ("KDE"));
    CHECK_FALSE (withDesktop ("GNOME-Flashback"));
    CHECK_FALSE (withDesktop ("X-Cinnamon"));
    CHECK_FALSE (withDesktop ("Budgie:GNOME-ish"));
}
