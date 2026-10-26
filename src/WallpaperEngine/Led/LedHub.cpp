#include "LedHub.h"

#include <atomic>

namespace WallpaperEngine::Led {
namespace {
    std::atomic<const void*> s_feeder = nullptr;
}

LedOutput& sharedOutput () {
    static LedOutput output;
    return output;
}

bool claimFeed (const void* scene) {
    const void* none = nullptr;
    return s_feeder.compare_exchange_strong (none, scene) || none == scene;
}

void releaseFeed (const void* scene) {
    const void* expected = scene;
    s_feeder.compare_exchange_strong (expected, nullptr);
}
}
