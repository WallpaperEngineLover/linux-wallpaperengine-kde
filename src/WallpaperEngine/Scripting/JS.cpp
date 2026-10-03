#include "JS.h"

#include <libplatform/libplatform.h>

#include <cstdlib>

namespace WallpaperEngine::Scripting::JS {
v8::Platform& platform () {
    // V8 initializes once per process and can't come back after V8::Dispose (), so it stays up until exit; every
    // scene's isolate shares it like WE's scenescript64 (sub_180030400 under a once guard)
    static v8::Platform* instance = [] {
	v8::Platform* created = v8::platform::NewDefaultPlatform ().release ();

	// reproducible renders (tools/regression): V8 seeds Math.random from the OS otherwise
	if (std::getenv ("LWE_FIXED_TIMESTEP") != nullptr) {
	    v8::V8::SetFlagsFromString ("--random-seed=1");
	}

	v8::V8::InitializeICU ();
	v8::V8::InitializePlatform (created);
	v8::V8::Initialize ();

	return created;
    }();

    return *instance;
}
} // namespace WallpaperEngine::Scripting::JS
