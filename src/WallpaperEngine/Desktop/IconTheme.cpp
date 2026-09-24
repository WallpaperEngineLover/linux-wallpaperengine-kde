#include "IconTheme.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

using namespace WallpaperEngine::Desktop;

namespace {
std::filesystem::path homeDir () {
    const char* home = std::getenv ("HOME");
    return home != nullptr ? home : "";
}

std::vector<std::filesystem::path> dataDirs () {
    std::vector<std::filesystem::path> result;
    const char* dataHome = std::getenv ("XDG_DATA_HOME");
    const char* dirs = std::getenv ("XDG_DATA_DIRS");

    result.emplace_back (dataHome != nullptr && *dataHome ? std::filesystem::path (dataHome) : homeDir () / ".local/share");

    std::stringstream stream (dirs != nullptr && *dirs ? dirs : "/usr/local/share:/usr/share");
    std::string dir;

    while (std::getline (stream, dir, ':')) {
	if (!dir.empty ()) {
	    result.emplace_back (dir);
	}
    }

    return result;
}

std::optional<std::string> iniValue (const std::filesystem::path& file, const std::string& section, const std::string& key) {
    std::ifstream stream (file);
    std::string line;
    bool inSection = false;

    while (std::getline (stream, line)) {
	if (line.starts_with ('[')) {
	    inSection = line == "[" + section + "]";
	    continue;
	}

	if (inSection && line.starts_with (key + "=")) {
	    return line.substr (key.size () + 1);
	}
    }

    return std::nullopt;
}

std::string userTheme () {
    const char* configHome = std::getenv ("XDG_CONFIG_HOME");
    const auto config = configHome != nullptr && *configHome ? std::filesystem::path (configHome) : homeDir () / ".config";

    if (const auto theme = iniValue (config / "kdeglobals", "Icons", "Theme"); theme.has_value () && !theme->empty ()) {
	return *theme;
    }

    return "breeze";
}

/** Scalable beats every fixed size, then bigger is better, SVG beats PNG at the same size */
int score (const std::filesystem::path& path) {
    const std::string dir = path.parent_path ().string ();
    const bool svg = path.extension () == ".svg";
    int size = 0;

    if (dir.find ("scalable") != std::string::npos) {
	size = 10000;
    } else {
	for (const auto& part : path.parent_path ()) {
	    const std::string name = part.string ();

	    if (!name.empty () && std::isdigit (static_cast<unsigned char> (name.front ()))) {
		size = std::max (size, std::atoi (name.c_str ()));
	    }
	}
    }

    // symbolic icons are monochrome glyphs meant for toolbars, not what a launcher should show
    const bool symbolic = path.stem ().string ().ends_with ("-symbolic");

    return size * 2 + (svg ? 1 : 0) - (symbolic ? 100000 : 0);
}
} // namespace

IconTheme& IconTheme::get () {
    static IconTheme instance;
    return instance;
}

IconTheme::IconTheme () {
    const auto home = homeDir ();

    for (const auto& dir : dataDirs ()) {
	this->m_baseDirs.push_back (dir / "icons");
    }

    this->m_baseDirs.push_back (home / ".icons");

    // the user's theme first, then whatever it inherits from, hicolor always last
    std::vector<std::string> pending { userTheme () };
    std::set<std::string> seen;

    while (!pending.empty ()) {
	const std::string theme = pending.front ();
	pending.erase (pending.begin ());

	if (theme == "hicolor" || !seen.insert (theme).second) {
	    continue;
	}

	this->m_themes.push_back (theme);

	for (const auto& parent : this->inherits (theme)) {
	    pending.push_back (parent);
	}
    }

    this->m_themes.emplace_back ("hicolor");
}

std::vector<std::string> IconTheme::inherits (const std::string& theme) const {
    for (const auto& base : this->m_baseDirs) {
	const auto parents = iniValue (base / theme / "index.theme", "Icon Theme", "Inherits");

	if (!parents.has_value ()) {
	    continue;
	}

	std::vector<std::string> result;
	std::stringstream stream (*parents);
	std::string parent;

	while (std::getline (stream, parent, ',')) {
	    if (!parent.empty ()) {
		result.push_back (parent);
	    }
	}

	return result;
    }

    return {};
}

const std::map<std::string, IconTheme::Candidate>& IconTheme::index (const std::string& theme) {
    if (const auto it = this->m_indexes.find (theme); it != this->m_indexes.end ()) {
	return it->second;
    }

    auto& result = this->m_indexes[theme];

    for (const auto& base : this->m_baseDirs) {
	std::error_code error;
	const auto root = base / theme;

	if (!std::filesystem::is_directory (root, error)) {
	    continue;
	}

	for (auto it = std::filesystem::recursive_directory_iterator (
		 root, std::filesystem::directory_options::skip_permission_denied, error
	     );
	     it != std::filesystem::recursive_directory_iterator (); it.increment (error)) {
	    if (error) {
		break;
	    }

	    const auto& path = it->path ();
	    const auto extension = path.extension ();

	    if (extension != ".png" && extension != ".svg") {
		continue;
	    }

	    const int candidateScore = score (path);
	    const auto [entry, inserted] = result.try_emplace (path.stem ().string (), Candidate { path, candidateScore });

	    if (!inserted && candidateScore > entry->second.score) {
		entry->second = Candidate { path, candidateScore };
	    }
	}
    }

    return result;
}

std::optional<std::filesystem::path> IconTheme::find (const std::string& icon) {
    if (icon.empty ()) {
	return std::nullopt;
    }

    std::error_code error;

    if (icon.starts_with ('/')) {
	return std::filesystem::is_regular_file (icon, error) ? std::optional<std::filesystem::path> (icon) : std::nullopt;
    }

    for (const auto& theme : this->m_themes) {
	const auto& icons = this->index (theme);

	if (const auto it = icons.find (icon); it != icons.end ()) {
	    return it->second.path;
	}
    }

    for (const auto& extension : { ".svg", ".png" }) {
	const auto path = std::filesystem::path ("/usr/share/pixmaps") / (icon + extension);

	if (std::filesystem::is_regular_file (path, error)) {
	    return path;
	}
    }

    return std::nullopt;
}
