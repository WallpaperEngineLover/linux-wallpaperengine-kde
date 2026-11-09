#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace WallpaperEngine::Application {
/**
 * Where WE's daytime and dayofweek playlists (wallpaper64.exe 2.8.42, sub_140075A90 and sub_140067A00) switch.
 * Every item has an end, the first item whose end is past the clock is the one shown, the last item when none is.
 */

/**
 * Daytime ends as fractions of the day. requested holds each item's "daytimeend" (0 or less when it has none): items
 * without one share the time up to the next one that has one evenly, the last item always ends at 1
 */
[[nodiscard]] std::vector<float> daytimeEnds (const std::vector<float>& requested);

/** Day of week ends in days since the first day of the week: 7 days shared out among at most 7 items */
[[nodiscard]] std::vector<float> dayOfWeekEnds (std::size_t count);

[[nodiscard]] std::size_t scheduledIndex (const std::vector<float>& ends, float position);

/** minutes into the local day as WE counts them, divided by 1440 */
[[nodiscard]] float daytimePosition (int hour, int minute);

/** weekday: 0 is Sunday like tm_wday, firstWeekday likewise, the result counts days from the first one */
[[nodiscard]] float dayOfWeekPosition (int weekday, int firstWeekday);

/** the first day of the week of the current locale, 0 is Sunday (WE asks Windows' LOCALE_IFIRSTDAYOFWEEK) */
[[nodiscard]] int localeFirstWeekday ();

/** item to show for the local clock now, the playlist's mode is "daytime" or "dayofweek" */
[[nodiscard]] std::size_t scheduledIndexNow (const std::vector<float>& ends, bool dayOfWeek);

/**
 * "logon" playlists show the next item once per session (WE keeps the index in its config). The state file maps a
 * playlist name to the index it showed and the session that was started in: the same session asking again gets the
 * same item, a new one the item after it. A missing or unreadable file starts at the first item. Without commit the
 * state file is only read, asking again with commit records the item (and gives the same answer)
 */
[[nodiscard]] std::size_t logonIndex (
    const std::filesystem::path& stateFile, const std::string& playlist, std::size_t count, const std::string& session,
    bool commit = true
);

/** id of the mount holding dir from /proc/self/mountinfo, empty when it cannot be told */
[[nodiscard]] std::string runtimeMountId (const std::string& dir);

/**
 * the id stored in dir/lwe-login, made on first use: a file in the user's runtime folder is gone with the last logout.
 * Empty when dir cannot hold it
 */
[[nodiscard]] std::string loginMarker (const std::filesystem::path& dir);

/**
 * boot id and the mount of the user's runtime dir: a new boot or a login after the last logout is a new session. A
 * Flatpak has a mount of its own for the runtime dir on every run, there the marker in the app's shared folder tells
 * the logins apart
 */
[[nodiscard]] std::string currentSessionToken ();

[[nodiscard]] std::filesystem::path logonStateFile ();
}
