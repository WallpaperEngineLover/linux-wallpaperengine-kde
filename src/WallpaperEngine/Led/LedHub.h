#pragma once

#include "LedOutput.h"

namespace WallpaperEngine::Led {
LedOutput& sharedOutput ();
[[nodiscard]] bool claimFeed (const void* scene);
void releaseFeed (const void* scene);
}
