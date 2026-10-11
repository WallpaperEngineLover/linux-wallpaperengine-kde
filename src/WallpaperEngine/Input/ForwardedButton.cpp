#include "ForwardedButton.h"

#include "WallpaperEngine/Data/JSON.h"

#include <fstream>

bool WallpaperEngine::Input::forwardedLeftDown (
    const std::filesystem::path& path, std::chrono::seconds maxAge, std::filesystem::file_time_type now
) {
    if (path.empty ()) {
	return false;
    }

    std::error_code error;
    const auto modified = std::filesystem::last_write_time (path, error);

    if (error || now - modified > maxAge) {
	return false;
    }

    std::ifstream file (path);
    const auto state = Data::JSON::JSON::parse (file, nullptr, false);

    return state.is_object () && state.value ("leftDown", false);
}
