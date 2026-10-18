#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <utility>

namespace WallpaperEngine::Scripting {
/**
 * Decides which script errors reach the log: a script that fails the same way every frame is reported once, also when
 * it alternates between a few errors, and the next error that is shown says how many were left out. Memory stays
 * bounded however many different errors a script makes up.
 */
class ErrorRepeatFilter {
public:
    static constexpr std::size_t REMEMBERED_PER_SCRIPT = 8;
    static constexpr std::size_t MAX_SCRIPTS = 256;

    /** Whether to log this error, suppressed gets the number of repeats dropped since the last one logged */
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
} // namespace WallpaperEngine::Scripting
