#pragma once

#include <string_view>

namespace WallpaperEngine::Render {
inline bool isFramebufferName (const std::string_view name) {
    return name.starts_with ("_rt_") || name.starts_with ("_alias_");
}
}
