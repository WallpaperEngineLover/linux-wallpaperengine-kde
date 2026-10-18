#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <utility>

namespace WallpaperEngine::Scripting {
class ErrorRepeatFilter {
public:
    static constexpr std::size_t REMEMBERED_PER_SCRIPT = 8;
    static constexpr std::size_t MAX_SCRIPTS = 256;

    bool admit (const std::string& script, const std::string& error, std::size_t& suppressed) {
	suppressed = 0;

	if (!m_scripts.contains (script) && m_scripts.size () >= MAX_SCRIPTS) {
	    m_scripts.clear ();
	}

	auto& recent = m_scripts[script];

	if (std::ranges::find (recent.errors, error) != recent.errors.end ()) {
	    recent.suppressed++;
	    return false;
	}

	recent.errors.push_front (error);

	if (recent.errors.size () > REMEMBERED_PER_SCRIPT) {
	    recent.errors.pop_back ();
	}

	suppressed = std::exchange (recent.suppressed, 0);

	return true;
    }

private:
    struct Recent {
	std::deque<std::string> errors;
	std::size_t suppressed = 0;
    };

    std::map<std::string, Recent> m_scripts;
};
}
