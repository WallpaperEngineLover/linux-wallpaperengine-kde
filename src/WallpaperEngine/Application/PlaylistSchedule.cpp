#include "PlaylistSchedule.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>

#include "WallpaperEngine/FileSystem/StateDirectory.h"
#include "WallpaperEngine/Logging/Log.h"

#include <clocale>
#include <fcntl.h>
#include <langinfo.h>
#include <sys/file.h>
#include <sys/stat.h>

#ifndef STATX_MNT_ID_UNIQUE
#define STATX_MNT_ID_UNIQUE 0x00004000U
#endif
#include <unistd.h>

namespace WallpaperEngine::Application {
std::vector<float> daytimeEnds (const std::vector<float>& requested) {
    std::vector<float> ends = requested;

    if (ends.empty ()) {
	return ends;
    }

    std::size_t runStart = 0;
    float previous = 0.0f;

    for (std::size_t index = 0; index < ends.size (); index++) {
	float end = 1.0f;

	if (index + 1 != ends.size ()) {
	    end = ends[index];

	    if (end <= 0.0f) {
		continue;
	    }
	}

	const float step = (end - previous) / static_cast<float> (index - runStart + 1);

	for (std::size_t item = runStart; item <= index; item++) {
	    ends[item] = static_cast<float> (item - runStart + 1) * step + previous;
	}

	ends[index] = end;

	runStart = index + 1;
	previous = end;
    }

    if (!std::is_sorted (ends.begin (), ends.end ())) {
	sLog.error ("Daytime playlist item ends do not rise through the day, some items will never be shown");
    }

    return ends;
}

std::vector<float> dayOfWeekEnds (const std::size_t count) {
    std::vector<float> ends (count);
    const int slots = static_cast<int> (std::clamp<std::size_t> (count, 1, 7));
    unsigned int total = 0;

    for (std::size_t item = 0; item < count; item++) {
	int remaining = 7;
	int share = 7;

	for (int slot = 0; slot < slots; slot++) {
	    share = slot < slots - 1 ? remaining / (slots - slot) : remaining;

	    if (slot == static_cast<int> (item)) {
		break;
	    }

	    remaining -= share;
	}

	total += static_cast<unsigned int> (share);
	ends[item] = static_cast<float> (total);
    }

    return ends;
}

std::size_t scheduledIndex (const std::vector<float>& ends, const float position) {
    for (std::size_t index = 0; index < ends.size (); index++) {
	if (ends[index] > position) {
	    return index;
	}
    }

    return ends.empty () ? 0 : ends.size () - 1;
}

float daytimePosition (const int hour, const int minute) { return static_cast<float> (minute + 60 * hour) / 1440.0f; }

float dayOfWeekPosition (const int weekday, const int firstWeekday) {
    return static_cast<float> (((weekday - firstWeekday) % 7 + 7) % 7);
}

int localeFirstWeekday () {
// the two items are enum members of glibc, not macros
#ifdef __GLIBC__
    const locale_t locale = newlocale (LC_TIME_MASK, "", nullptr);

    if (locale == nullptr) {
	return 0;
    }

    const int first = static_cast<unsigned char> (nl_langinfo_l (_NL_TIME_FIRST_WEEKDAY, locale)[0]);
    // a word item: glibc only defines the low 32 bits of the returned pointer
    const auto origin
	= static_cast<std::uint32_t> (reinterpret_cast<std::uintptr_t> (nl_langinfo_l (_NL_TIME_WEEK_1STDAY, locale)));

    freelocale (locale);

    // 19971130 was a Sunday and 19971201 a Monday, the week starts first_weekday - 1 days after that
    const int firstOrigin = origin == 19971201 ? 1 : 0;

    return first >= 1 && first <= 7 ? (firstOrigin + first - 1) % 7 : 0;
#else
    return 0;
#endif
}

std::size_t scheduledIndexNow (const std::vector<float>& ends, const bool dayOfWeek) {
    const std::time_t now = std::time (nullptr);
    std::tm local {};

    tzset ();
    localtime_r (&now, &local);

    static const int firstWeekday = localeFirstWeekday ();

    return scheduledIndex (
	ends,
	dayOfWeek ? dayOfWeekPosition (local.tm_wday, firstWeekday) : daytimePosition (local.tm_hour, local.tm_min)
    );
}

std::size_t logonIndex (
    const std::filesystem::path& stateFile, const std::string& playlist, const std::size_t count,
    const std::string& session, const bool commit
) {
    if (count == 0) {
	return 0;
    }

    std::error_code ignored;

    // a peek reads the renamed-into-place file and leaves the disk and the lock alone
    if (commit) {
	std::filesystem::create_directories (stateFile.parent_path (), ignored);
    }

    // two engines starting together must not both read the old index and overwrite each other's playlist
    auto lockPath = stateFile;
    lockPath += ".lock";
    struct Lock {
	int fd;

	~Lock () {
	    if (fd >= 0) {
		close (fd);
	    }
	}
    } lock { commit ? open (lockPath.c_str (), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600) : -1 };

    if (lock.fd >= 0) {
	flock (lock.fd, LOCK_EX);
    }

    nlohmann::json state = nlohmann::json::object ();

    try {
	std::ifstream in (stateFile);

	if (in.is_open ()) {
	    state = nlohmann::json::parse (in, nullptr, false);
	}
    } catch (...) {
	sLog.debug ("Cannot read playlist state ", stateFile.string ());
    }

    if (!state.is_object ()) {
	state = nlohmann::json::object ();
    }

    const auto entry = state.find (playlist);
    std::size_t index = 0;

    if (entry != state.end () && entry->is_object ()) {
	const auto saved = entry->find ("index");
	const auto savedSession = entry->find ("session");

	if (saved != entry->end () && saved->is_number_integer () && saved->get<long long> () >= 0) {
	    index = static_cast<std::size_t> (saved->get<long long> ()) % count;

	    if (savedSession != entry->end () && savedSession->is_string ()
		&& savedSession->get<std::string> () == session) {
		return index;
	    }

	    index = (index + 1) % count;
	}
    }

    if (!commit) {
	return index;
    }

    state[playlist] = { { "index", index }, { "session", session } };

    // renamed over the target so a reader never sees half of it, a state file that cannot be written only costs the
    // memory of the last item
    auto partial = stateFile;
    partial += ".partial" + std::to_string (getpid ());

    try {
	// a name another user could have guessed in a shared directory must not be followed or reused
	std::filesystem::remove (partial, ignored);

	const int fd = open (partial.c_str (), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

	if (fd >= 0) {
	    const std::string text = state.dump (1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
	    std::size_t written = 0;

	    while (written < text.size ()) {
		const auto step = write (fd, text.data () + written, text.size () - written);

		if (step <= 0) {
		    break;
		}

		written += static_cast<std::size_t> (step);
	    }

	    close (fd);

	    if (written == text.size ()) {
		std::filesystem::rename (partial, stateFile, ignored);
	    }
	}
    } catch (...) {
	sLog.debug ("Cannot write playlist state ", stateFile.string ());
    }

    std::filesystem::remove (partial, ignored);

    return index;
}

std::string runtimeMountId (const std::string& dir) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical (dir, error).string ();

    if (error) {
	return {};
    }

    std::ifstream info ("/proc/self/mountinfo");
    std::string line;
    std::string best;
    std::size_t bestLength = 0;

    while (std::getline (info, line)) {
	std::istringstream fields (line);
	std::string id, parent, device, root, escaped;

	fields >> id >> parent >> device >> root >> escaped;

	std::string point;

	for (std::size_t at = 0; at < escaped.size (); at++) {
	    if (escaped[at] == '\\' && at + 3 < escaped.size ()) {
		point += static_cast<char> (std::strtol (escaped.substr (at + 1, 3).c_str (), nullptr, 8));
		at += 3;
	    } else {
		point += escaped[at];
	    }
	}

	const bool inside = point == "/" || resolved == point || resolved.starts_with (point + "/");

	// later lines are mounted on top of earlier ones
	if (inside && point.size () >= bestLength) {
	    best = id;
	    bestLength = point.size ();
	}
    }

    return best;
}

std::string loginMarker (const std::filesystem::path& dir) {
    const auto marker = dir / "lwe-login";
    const auto read = [&marker] () {
	std::string id;
	std::ifstream in (marker);

	std::getline (in, id);

	return id;
    };

    if (auto id = read (); !id.empty ()) {
	return id;
    }

    std::random_device entropy;
    const std::uint64_t random[2] = {
	(static_cast<std::uint64_t> (entropy ()) << 32) | entropy (),
	(static_cast<std::uint64_t> (entropy ()) << 32) | entropy (),
    };

    char text[40];

    snprintf (
	text, sizeof (text), "%016llx%016llx\n", static_cast<unsigned long long> (random[0]),
	static_cast<unsigned long long> (random[1])
    );

    // linked into place whole: two engines starting together settle on the first one's id and nobody reads half of it
    auto partial = marker;
    partial += "." + std::to_string (getpid ());
    std::error_code ignored;
    std::filesystem::remove (partial, ignored);

    const int fd = open (partial.c_str (), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

    if (fd >= 0) {
	const auto length = std::strlen (text);
	const bool written = write (fd, text, length) == static_cast<ssize_t> (length);

	close (fd);

	if (written) {
	    const int linked = link (partial.c_str (), marker.c_str ());

	    static_cast<void> (linked);
	}
    }

    std::filesystem::remove (partial, ignored);

    return read ();
}

std::string currentSessionToken () {
    std::string token;
    std::ifstream boot ("/proc/sys/kernel/random/boot_id");

    std::getline (boot, token);

    // inside a Flatpak /run/user/<uid> sits on the sandbox's own tmpfs, a new mount for every run, but the app's folder
    // in it is the host's and lives until the last logout
    if (const char* appId = std::getenv ("FLATPAK_ID"); appId != nullptr && access ("/.flatpak-info", F_OK) == 0) {
	const char* shared = std::getenv ("XDG_RUNTIME_DIR");
	const std::filesystem::path appDir
	    = std::filesystem::path (
		  shared != nullptr && shared[0] == '/' ? shared : "/run/user/" + std::to_string (getuid ())
	      )
	    / "app" / appId;
	std::error_code error;

	if (std::filesystem::is_directory (appDir, error)) {
	    if (const auto id = loginMarker (appDir); !id.empty ()) {
		return token + ":f" + id;
	    }
	}
    }

    // the runtime dir is a tmpfs of its own from the first login of a user to the last logout, whatever started the
    // engine, unlike XDG_SESSION_ID which a terminal has and an autostart entry or a user service often lacks. Its
    // inode is always 1, the mount is what tells two logins apart (unique ids are never reused while the system runs)
    const char* runtime = std::getenv ("XDG_RUNTIME_DIR");
    const std::string runtimeDir
	= runtime != nullptr && runtime[0] == '/' ? std::string (runtime) : "/run/user/" + std::to_string (getuid ());

    // glibc 2.35's struct statx has no stx_mnt_id: the kernel's layout has the mask first and the id at 0x90
    alignas (8) unsigned char info[256] = {};
    std::uint32_t mask = 0;
    std::uint64_t mountId = 0;

    if (statx (AT_FDCWD, runtimeDir.c_str (), 0, STATX_MNT_ID_UNIQUE, reinterpret_cast<struct statx*> (info)) == 0) {
	std::memcpy (&mask, info, sizeof (mask));
	std::memcpy (&mountId, info + 0x90, sizeof (mountId));
    }

    if ((mask & STATX_MNT_ID_UNIQUE) != 0) {
	token += ":" + std::to_string (mountId);
    } else if (const auto mount = runtimeMountId (runtimeDir); !mount.empty ()) {
	// kernels before 6.8: the classic mount id is handed out cyclically, so it is not reused soon after a logout
	token += ":m" + mount;
    } else if (const char* session = std::getenv ("XDG_SESSION_ID"); session != nullptr) {
	token += ":";
	token += session;
    }

    return token;
}

std::filesystem::path logonStateFile () {
    if (const auto root = FileSystem::stateRoot (); !root.empty ()) {
	return root / "playlists.json";
    }

    static std::once_flag reported;

    std::call_once (reported, [] {
	sLog.error (
	    "No state directory (XDG_STATE_HOME and HOME unset): the logon playlist position is kept in a temporary "
	    "folder and starts over at every login"
	);
    });

    if (const char* runtime = std::getenv ("XDG_RUNTIME_DIR"); runtime != nullptr && runtime[0] == '/') {
	return std::filesystem::path (runtime) / "linux-wallpaperengine-playlists.json";
    }

    std::error_code error;
    const auto temp = std::filesystem::temp_directory_path (error);

    return (error ? std::filesystem::path ("/tmp") : temp)
	/ ("linux-wallpaperengine-playlists-" + std::to_string (getuid ()) + ".json");
}
}
