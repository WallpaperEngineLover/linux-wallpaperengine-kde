#pragma once

#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>

namespace WallpaperEngine::Application {
/** Whether something owned by someone other than user sits at path: lstat, so a link someone else planted counts by
 * its own owner and not by what it points to. A path that is not there is nobody's */
inline bool belongsToAnotherUser (const std::filesystem::path& path, uid_t user) {
    struct stat info {};

    return lstat (path.c_str (), &info) == 0 && info.st_uid != user;
}
} // namespace WallpaperEngine::Application
