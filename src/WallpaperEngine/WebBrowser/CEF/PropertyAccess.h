#pragma once

#include "WallpaperEngine/Debugging/ThreadSafety.h"

namespace WallpaperEngine::WebBrowser::CEF {
/** Guards the wallpaper's property values in the web host: the page bridge writes them on the UI thread, scheme
 * handlers read the file ones on CEF's worker threads */
inline Debugging::Mutex& propertyValues () {
    static Debugging::Mutex mutex;
    return mutex;
}
}
