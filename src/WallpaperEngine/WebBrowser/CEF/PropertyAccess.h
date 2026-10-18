#pragma once

#include <mutex>

namespace WallpaperEngine::WebBrowser::CEF {
/** Guards the wallpaper's property values in the web host: the page bridge writes them on the UI thread, scheme
 * handlers read the file ones on CEF's worker threads */
inline std::mutex& propertyValues () {
    static std::mutex mutex;
    return mutex;
}
} // namespace WallpaperEngine::WebBrowser::CEF
