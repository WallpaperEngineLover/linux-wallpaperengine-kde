#include "WallpaperEngine/WebBrowser/CEF/SubprocessApp.h"

// Chromium child processes of a --web-host, apart from the engine whose ~250 libraries take ~50ms to load
int main (int argc, char* argv[]) {
    const CefMainArgs mainArgs (argc, argv);

    return CefExecuteProcess (mainArgs, new WallpaperEngine::WebBrowser::CEF::SubprocessApp (), nullptr);
}
