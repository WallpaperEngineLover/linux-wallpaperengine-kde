#pragma once

#include <exception>

#include "WallpaperEngine/Logging/Log.h"

namespace WallpaperEngine::Logging {
// an exception that leaves a thread's function ends the whole process, this logs it and lets that thread end alone
template <typename Body> void guardThread (const char* name, Body&& body) noexcept {
    try {
	body ();
    } catch (const std::exception& e) {
	sLog.error (name, " stopped: ", e.what ());
    } catch (...) {
	sLog.error (name, " stopped by an unknown error");
    }
}
} // namespace WallpaperEngine::Logging
