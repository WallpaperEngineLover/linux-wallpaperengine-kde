#include "SystemFonts.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <strings.h>
#include <unordered_map>

namespace WallpaperEngine::Render::Fonts {
namespace {
    std::string run (const std::string& command) {
	FILE* pipe = popen (command.c_str (), "r");

	if (pipe == nullptr) {
	    return {};
	}

	std::string output;
	char buffer[512];

	while (fgets (buffer, sizeof (buffer), pipe) != nullptr) {
	    output += buffer;
	}

	pclose (pipe);
	return output;
    }

    // both ends of a command line, in single quotes: no quote, no line break (a backslash is fc-match's own escape)
    bool shellSafe (const std::string& text) { return text.find_first_of ("'\n") == std::string::npos; }

    std::string xmlEscape (const std::string& text) {
	std::string result;

	for (const char c : text) {
	    switch (c) {
		case '&':
		    result += "&amp;";
		    break;
		case '<':
		    result += "&lt;";
		    break;
		case '>':
		    result += "&gt;";
		    break;
		default:
		    result += c;
	    }
	}

	return result;
    }

    std::string fontconfigFile () {
	static std::string path;
	static std::once_flag once;

	std::call_once (once, [] {
	    const std::filesystem::path directory = userFontDirectory ();
	    std::error_code error;

	    if (!std::filesystem::is_directory (directory, error) || !shellSafe (directory.string ())) {
		return;
	    }

	    std::filesystem::path cache;

	    if (const char* home = std::getenv ("XDG_CACHE_HOME"); home != nullptr && home[0] == '/') {
		cache = home;
	    } else if (const char* user = std::getenv ("HOME"); user != nullptr && user[0] == '/') {
		cache = std::filesystem::path (user) / ".cache";
	    } else {
		return;
	    }

	    cache /= "linux-wallpaperengine";
	    std::filesystem::create_directories (cache, error);

	    const std::filesystem::path file = cache / "fonts.conf";
	    // a configuration the environment already points at (Nix, Guix) is the one to extend
	    std::string base = "/etc/fonts/fonts.conf";

	    if (const char* env = std::getenv ("FONTCONFIG_FILE"); env != nullptr && env[0] == '/') {
		base = env;
	    }

	    const std::string content
		= "<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n<fontconfig>\n"
		  "  <include ignore_missing=\"yes\">"
		+ xmlEscape (base) + "</include>\n  <dir>" + xmlEscape (directory.string ())
		+ "</dir>\n</fontconfig>\n";

	    std::string existing;

	    if (std::ifstream in (file); in) {
		std::stringstream buffer;
		buffer << in.rdbuf ();
		existing = buffer.str ();
	    }

	    if (existing != content) {
		std::ofstream out (file, std::ios::trunc);
		out << content;

		if (!out) {
		    return;
		}
	    }

	    if (shellSafe (file.string ())) {
		path = file.string ();
	    }
	});

	return path;
    }

    bool sameFamily (const std::string& names, const std::string& family) {
	std::stringstream list (names);
	std::string name;

	while (std::getline (list, name, ',')) {
	    if (strcasecmp (name.c_str (), family.c_str ()) == 0) {
		return true;
	    }
	}

	return false;
    }
}

std::filesystem::path userFontDirectory () {
    if (const char* forced = std::getenv ("LWE_FONT_DIR"); forced != nullptr && forced[0] == '/') {
	return forced;
    }

    if (const char* data = std::getenv ("XDG_DATA_HOME"); data != nullptr && data[0] == '/') {
	return std::filesystem::path (data) / "linux-wallpaperengine" / "fonts";
    }

    if (const char* home = std::getenv ("HOME"); home != nullptr && home[0] == '/') {
	return std::filesystem::path (home) / ".local/share/linux-wallpaperengine/fonts";
    }

    return {};
}

std::string fcMatch (const std::string& pattern, const std::string& format) {
    if (!shellSafe (pattern) || !shellSafe (format)) {
	return {};
    }

    const std::string config = fontconfigFile ();
    const std::string prefix = config.empty () ? std::string () : "FONTCONFIG_FILE='" + config + "' ";

    return run (prefix + "fc-match -f '" + format + "' '" + pattern + "' 2>/dev/null");
}

std::vector<std::string> systemFontFamilies (const std::string& font) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> families = {
	{ "systemfont_arial", { "Arial", "Liberation Sans", "Arimo" } },
	{ "systemfont_calibri", { "Calibri", "Carlito" } },
	{ "systemfont_cambria", { "Cambria", "Caladea" } },
	{ "systemfont_comicsans", { "Comic Sans MS", "Comic Neue", "Comic Relief" } },
	{ "systemfont_consolas", { "Consolas", "Inconsolata", "DejaVu Sans Mono" } },
	{ "systemfont_sansserif", { "sans-serif" } },
	{ "systemfont_segoe", { "Segoe UI", "Noto Sans", "Open Sans" } },
	{ "systemfont_verdana", { "Verdana", "DejaVu Sans" } },
    };

    for (const auto& [name, list] : families) {
	if (font == name) {
	    return list;
	}
    }

    return {};
}

std::string installedFamilyFile (const std::vector<std::string>& families) {
    static std::unordered_map<std::string, std::string> cache;

    for (const auto& family : families) {
	auto it = cache.find (family);

	if (it == cache.end ()) {
	    const std::string output = fcMatch (family, "%{family}\\n%{file}");
	    std::string path;

	    if (const size_t newline = output.find ('\n');
		newline != std::string::npos && sameFamily (output.substr (0, newline), family)) {
		path = output.substr (newline + 1);
	    }

	    std::error_code error;

	    if (!path.empty () && !std::filesystem::exists (path, error)) {
		path.clear ();
	    }

	    it = cache.emplace (family, path).first;
	}

	if (!it->second.empty ()) {
	    return it->second;
	}
    }

    return {};
}

FontReport systemFontReport (const std::string& font) {
    const auto families = systemFontFamilies (font);

    if (families.empty ()) {
	return {};
    }

    FontReport report;
    report.family = families.front ();

    if (families.front () == "sans-serif") {
	report.using_ = families.front ();
	return report;
    }

    static const std::vector<std::string> clones = { "Liberation Sans", "Arimo", "Carlito", "Caladea" };

    for (size_t index = 0; index < families.size (); index++) {
	if (installedFamilyFile ({ families[index] }).empty ()) {
	    continue;
	}

	report.using_ = families[index];

	if (index == 0) {
	    report.status = FontStatus::Exact;
	} else {
	    report.status = std::find (clones.begin (), clones.end (), families[index]) != clones.end ()
		? FontStatus::Compatible
		: FontStatus::Substitute;
	}

	return report;
    }

    report.status = FontStatus::Substitute;
    report.using_ = fcMatch (families.front (), "%{family[0]}");

    if (report.using_.empty ()) {
	report.using_ = "DejaVu Sans";
    }

    return report;
}

FontReport emojiReport () {
    FontReport report;
    report.family = "Segoe UI Emoji";

    if (!installedFamilyFile ({ report.family }).empty ()) {
	report.using_ = report.family;
	return report;
    }

    report.status = FontStatus::Substitute;
    report.using_ = "Twemoji";
    return report;
}

bool containsEmoji (const std::string& utf8) {
    // the characters Unicode draws as pictures by default (Emoji_Presentation) plus the markers that ask for one; the
    // text symbols of the same blocks (stars, hearts, arrows) are plain glyphs in ordinary fonts and don't count
    static constexpr std::pair<char32_t, char32_t> kRanges[] = {
	{ 0x1F000, 0x1FAFF }, { 0x231A, 0x231B }, { 0x23E9, 0x23F3 }, { 0x23F8, 0x23FA }, { 0x25FD, 0x25FE },
	{ 0x2614, 0x2615 },   { 0x2648, 0x2653 }, { 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
	{ 0x26AA, 0x26AB },   { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 },
	{ 0x26EA, 0x26EA },   { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD },
	{ 0x2705, 0x2705 },   { 0x270A, 0x270B }, { 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E },
	{ 0x2753, 0x2755 },   { 0x2757, 0x2757 }, { 0x2795, 0x2797 }, { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF },
	{ 0x2B1B, 0x2B1C },   { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 }, { 0xFE0F, 0xFE0F }, { 0x20E3, 0x20E3 },
    };

    for (size_t index = 0; index < utf8.size ();) {
	const auto lead = static_cast<unsigned char> (utf8[index]);
	char32_t codepoint = lead;
	size_t length = 1;

	if (lead >= 0xF0) {
	    length = 4;
	    codepoint = lead & 0x07;
	} else if (lead >= 0xE0) {
	    length = 3;
	    codepoint = lead & 0x0F;
	} else if (lead >= 0xC0) {
	    length = 2;
	    codepoint = lead & 0x1F;
	}

	for (size_t part = 1; part < length && index + part < utf8.size (); part++) {
	    codepoint = (codepoint << 6) | (static_cast<unsigned char> (utf8[index + part]) & 0x3F);
	}

	index += length;

	for (const auto& [low, high] : kRanges) {
	    if (codepoint >= low && codepoint <= high) {
		return true;
	    }
	}
    }

    return false;
}
}
