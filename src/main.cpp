#include <csignal>
#include <iostream>
#include <string_view>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/JS.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

WallpaperEngine::Application::WallpaperApplication* app;

void signalhandler (const int sig) {
    if (app == nullptr) {
	return;
    }

    app->signal (sig);
}

void initLogging () {
    sLog.addOutput (new std::ostream (std::cout.rdbuf ()));
    sLog.addError (new std::ostream (std::cerr.rdbuf ()));
}

int main (int argc, char* argv[]) {
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

	WallpaperEngine::Application::ApplicationContext appContext (argc, argv);

	appContext.loadSettingsFromArgv ();

	app = new WallpaperEngine::Application::WallpaperApplication (appContext);

	if (appContext.settings.general.onlyListProperties || appContext.settings.general.onlyListObjects
	    || appContext.settings.general.onlyListAudioObjects || appContext.settings.general.onlyListEffects) {
	    delete app;
	    return 0;
	}

	// Disposable CEF host for a single Web wallpaper - runs its own loop until told to quit
	// over shared memory, never touches Wayland/GL/audio.
	if (appContext.settings.general.webHost) {
	    app->runWebHost ();
	    delete app;
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

	return 0;
    } catch (const std::exception& e) {
	std::cerr << e.what () << std::endl;
	return 1;
    }
}