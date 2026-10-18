#include "JS.h"

#include "WallpaperEngine/Logging/Log.h"

#include <libplatform/libplatform.h>

#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

// embedders skip node's ICU setup, without it Fedora's libnode has no time zone data. Weak for nodes without Intl
namespace node::i18n {
[[gnu::weak]] bool InitializeICUDirectory (const std::string& path, std::string* error);
}

namespace WallpaperEngine::Scripting::JS {
namespace {
    // the callbacks the stand-ins call, written before a stand-in is handed out and only read by the script thread
    constexpr size_t GuardSlots = 1024;
    std::array<v8::FunctionCallback, GuardSlots> guardedCallbacks {};

    template <size_t Slot> void guarded (const v8::FunctionCallbackInfo<v8::Value>& info) {
	try {
	    guardedCallbacks[Slot](info);
	} catch (const std::exception& e) {
	    sLog.error ("Script API call failed: ", e.what ());
	    info.GetIsolate ()->ThrowException (v8::Exception::Error (string (info.GetIsolate (), e.what ())));
	} catch (...) {
	    sLog.error ("Script API call failed: unknown error");
	    info.GetIsolate ()->ThrowException (v8::Exception::Error (string (info.GetIsolate (), "unknown error")));
	}
    }

    template <size_t... Slot> constexpr auto makeGuards (std::index_sequence<Slot...>) {
	return std::array<v8::FunctionCallback, sizeof...(Slot)> { &guarded<Slot>... };
    }

    constexpr auto guards = makeGuards (std::make_index_sequence<GuardSlots> ());

    // node's lookup: NODE_ICU_DATA, then the build's default directory if installed
    std::string icuDataDirectory () {
	if (const char* env = std::getenv ("NODE_ICU_DATA"); env != nullptr) {
	    return env;
	}

#ifdef V8_ICU_DEFAULT_DATA_DIR
	std::error_code ec;

	for (const auto& entry : std::filesystem::directory_iterator (V8_ICU_DEFAULT_DATA_DIR, ec)) {
	    const auto name = entry.path ().filename ().string ();

	    if (name.starts_with ("icudt") && name.ends_with (".dat")) {
		return V8_ICU_DEFAULT_DATA_DIR;
	    }
	}
#endif

	return {};
    }
}

v8::Platform& platform () {
    // V8 initializes once per process and can't come back after V8::Dispose (), so it stays up until exit; every
    // scene's isolate shares it like WE's scenescript64 (sub_180030400 under a once guard)
    static v8::Platform* instance = [] {
	v8::Platform* created = v8::platform::NewDefaultPlatform ().release ();

	// reproducible renders (tools/regression): V8 seeds Math.random from the OS otherwise
	if (std::getenv ("LWE_FIXED_TIMESTEP") != nullptr) {
	    v8::V8::SetFlagsFromString ("--random-seed=1");
	}

	if (node::i18n::InitializeICUDirectory != nullptr) {
	    std::string error;

	    if (!node::i18n::InitializeICUDirectory (icuDataDirectory (), &error)) {
		sLog.error ("Could not initialize ICU: ", error);
	    }
	}

	v8::V8::InitializeICU ();
	v8::V8::InitializePlatform (created);
	v8::V8::Initialize ();

	return created;
    }();

    return *instance;
}

namespace {
    std::string zoneNameFromPath (const std::string& path) {
	std::error_code ec;
	std::string target = path;

	if (std::filesystem::is_symlink (path, ec)) {
	    target = std::filesystem::read_symlink (path, ec).string ();
	}

	const auto at = target.find ("zoneinfo/");

	if (at == std::string::npos) {
	    return {};
	}

	std::string name = target.substr (at + 9);

	for (const char* prefix : { "posix/", "right/" }) {
	    if (name.starts_with (prefix)) {
		name.erase (0, std::char_traits<char>::length (prefix));
	    }
	}

	return name;
    }
}

void useNamedTimeZone () {
    const char* current = std::getenv ("TZ");
    std::string value = current != nullptr ? current : "";

    if (value.starts_with (':')) {
	value.erase (0, 1);
    }

    // zone names and POSIX rules work in ICU as they are
    if (!value.empty () && !value.starts_with ('/')) {
	return;
    }

    std::string name = zoneNameFromPath (value.empty () ? "/etc/localtime" : value);

    if (name.empty () && value.empty ()) {
	std::ifstream file ("/etc/timezone");
	std::getline (file, name);
    }

    if (!name.empty () && std::filesystem::exists ("/usr/share/zoneinfo/" + name)) {
	setenv ("TZ", name.c_str (), 1);
    }
}
v8::FunctionCallback guard (v8::FunctionCallback callback) {
    static std::mutex mutex;
    static size_t used = 0;
    static bool warned = false;
    const std::lock_guard lock (mutex);

    if (callback == nullptr) {
	return callback;
    }

    for (size_t i = 0; i < used; i++) {
	if (guardedCallbacks[i] == callback) {
	    return guards[i];
	}
    }

    if (used == GuardSlots) {
	if (!warned) {
	    warned = true;
	    sLog.error ("More script API callbacks than guards (", GuardSlots, "), the rest run without one");
	}

	return callback;
    }

    guardedCallbacks[used] = callback;

    return guards[used++];
}
}
