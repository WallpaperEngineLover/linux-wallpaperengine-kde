#include <csignal>
#include <cstdlib>
#include <iostream>
#include <malloc.h>
#include <string_view>

#include <SDL.h>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/JS.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

WallpaperEngine::Application::WallpaperApplication* app;

static void signalhandler (const int sig) {
    if (app == nullptr) {
	return;
    }

    app->signal (sig);
}

static void initLogging () {
    static std::ostream out (std::cout.rdbuf ());
    static std::ostream error (std::cerr.rdbuf ());

    sLog.addOutput (&out);
    sLog.addError (&error);
}

// Hotswapping between wallpapers grew the resident size by 40 MB a minute with glibc's defaults (72 swaps: 638 MB to
// 1019 MB, all anonymous memory, heaptrack saw no leak): per thread arenas and a mmap threshold that rises with every
// freed big buffer leave freed textures and scene data scattered through the heap, and trimming never finds a free top.
// With these it stays at 330 MB. Explicit MALLOC_* variables win.
static void tuneAllocator () {
    if (std::getenv ("MALLOC_ARENA_MAX") == nullptr) {
	mallopt (M_ARENA_MAX, 2);
    }

    if (std::getenv ("MALLOC_MMAP_THRESHOLD_") == nullptr) {
	mallopt (M_MMAP_THRESHOLD, 256 * 1024);
    }

    if (std::getenv ("MALLOC_TRIM_THRESHOLD_") == nullptr) {
	mallopt (M_TRIM_THRESHOLD, 64 * 1024);
    }
}

// the CEF host and Chromium's children keep glibc's defaults, Chromium allocates most of its memory itself
static bool isWebChild (int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
	const std::string_view arg = argv[i];

	if (arg == "--web-host" || arg.starts_with ("--type=")) {
	    return true;
	}
    }

    return false;
}

int main (int argc, char* argv[]) {
    if (!isWebChild (argc, argv)) {
	tuneAllocator ();
    }

    WallpaperEngine::Scripting::JS::useNamedTimeZone ();

    try {
	// --type=* is a Chromium child process of a --web-host, hand it to CEF right away
	for (int i = 1; i < argc; i++) {
	    const std::string_view arg = argv[i];

	    if (!arg.starts_with ("--type=")) {
		continue;
	    }

	    if (!arg.starts_with ("--type=zygote") && !arg.starts_with ("--type=utility")) {
		initLogging ();
	    }

	    return WallpaperEngine::WebBrowser::WebBrowserContext::runSubprocess (argc, argv);
	}

	initLogging ();

	// we_manager reads our output through pipes and the engine outlives it: after the app quits, the next log line
	// (a hotswap logs right away) would end the engine with SIGPIPE
	std::signal (SIGPIPE, SIG_IGN);

	WallpaperEngine::Application::ApplicationContext appContext (argc, argv);

	appContext.loadSettingsFromArgv ();

	// leaving this scope by an exception destroys appContext before the catch below runs, and the app's audio
	// threads read through it: the app has to go first, which this does (it is declared after appContext)
	struct AppReleaser {
	    ~AppReleaser () {
		delete app;
		app = nullptr;
	    }
	} appReleaser;

	app = new WallpaperEngine::Application::WallpaperApplication (appContext);

	if (appContext.settings.general.onlyListProperties || appContext.settings.general.onlyListObjects
	    || appContext.settings.general.onlyListAudioObjects || appContext.settings.general.onlyListEffects
	    || appContext.settings.general.onlyListFonts) {
	    return 0;
	}

	// disposable CEF host for one Web wallpaper, quits over shared memory
	if (appContext.settings.general.webHost) {
	    app->runWebHost ();
	    return 0;
	}

	std::signal (SIGINT, signalhandler);
	std::signal (SIGTERM, signalhandler);
	std::signal (SIGKILL, signalhandler);
	// SIGUSR1 triggers a wallpaper hotswap without stopping the process
	std::signal (SIGUSR1, signalhandler);

	app->show ();

	// remove signal handlers before destroying app
	std::signal (SIGINT, SIG_DFL);
	std::signal (SIGTERM, SIG_DFL);
	std::signal (SIGKILL, SIG_DFL);
	std::signal (SIGUSR1, SIG_DFL);

	delete app;
	app = nullptr;

	// after the app: the audio threads are joined by then, sdl2-compat's SDL_Quit breaks what they still use
	SDL_Quit ();

	return 0;
    } catch (const std::exception& e) {
	std::cerr << e.what () << std::endl;
	return 1;
    } catch (...) {
	std::cerr << "Stopped by an unknown error" << std::endl;
	return 1;
    }
}