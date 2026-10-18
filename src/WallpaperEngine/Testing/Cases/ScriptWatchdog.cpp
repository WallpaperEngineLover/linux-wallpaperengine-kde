#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "WallpaperEngine/Scripting/JS.h"
#include "WallpaperEngine/Scripting/ScriptWatchdog.h"

using namespace WallpaperEngine::Scripting;
using namespace std::chrono_literals;

namespace {
struct Isolate {
    Isolate () {
	JS::platform ();
	allocator.reset (v8::ArrayBuffer::Allocator::NewDefaultAllocator ());
	v8::Isolate::CreateParams params;
	params.array_buffer_allocator = allocator.get ();
	isolate = v8::Isolate::New (params);
    }

    ~Isolate () { isolate->Dispose (); }

    /** Whether the source ran to its end */
    bool run (const std::string& source, std::string* result = nullptr) const {
	const v8::Isolate::Scope isolateScope (isolate);
	const v8::HandleScope handleScope (isolate);
	const auto context = v8::Context::New (isolate);
	const v8::Context::Scope contextScope (context);
	const v8::TryCatch tryCatch (isolate);
	v8::Local<v8::Script> script;
	v8::Local<v8::Value> value;

	if (!v8::Script::Compile (context, JS::string (isolate, source)).ToLocal (&script)
	    || !script->Run (context).ToLocal (&value)) {
	    return false;
	}

	if (result != nullptr) {
	    *result = JS::toString (isolate, value);
	}

	return true;
    }

    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    v8::Isolate* isolate = nullptr;
};
} // namespace

TEST_CASE ("a script that never returns is terminated once its budget is gone", "[script][watchdog]") {
    Isolate engine;
    ScriptWatchdog watchdog (engine.isolate);

    watchdog.begin (200ms);

    const auto start = std::chrono::steady_clock::now ();
    const bool finished = engine.run ("for (;;) {}");
    const auto took = std::chrono::steady_clock::now () - start;

    REQUIRE_FALSE (finished);
    REQUIRE (watchdog.end ());
    // the budget plus the polling interval, with room for a busy machine
    REQUIRE (took >= 150ms);
    REQUIRE (took < 3s);

    // a termination can still be pending after the call (the watchdog fires as it ends), the embedder clears it
    engine.isolate->CancelTerminateExecution ();

    std::string result;
    REQUIRE (engine.run ("6 * 7", &result));
    REQUIRE (result == "42");
}

TEST_CASE ("a call inside its budget is left alone", "[script][watchdog]") {
    Isolate engine;
    ScriptWatchdog watchdog (engine.isolate);
    std::string result;

    watchdog.begin (5s);
    REQUIRE (engine.run ("let sum = 0; for (let i = 0; i < 1000; i++) sum += i; sum", &result));
    REQUIRE_FALSE (watchdog.end ());
    REQUIRE (result == "499500");
}

TEST_CASE ("a disarmed watchdog never terminates anything", "[script][watchdog]") {
    Isolate engine;
    ScriptWatchdog watchdog (engine.isolate);
    std::string result;

    watchdog.begin (50ms);
    REQUIRE_FALSE (watchdog.end ());

    // longer than the polling interval and the old budget
    std::this_thread::sleep_for (400ms);

    REQUIRE (engine.run ("1 + 1", &result));
    REQUIRE (result == "2");
}

TEST_CASE ("the watchdog can be armed again after it fired", "[script][watchdog]") {
    Isolate engine;
    ScriptWatchdog watchdog (engine.isolate);
    std::string result;

    watchdog.begin (100ms);
    REQUIRE_FALSE (engine.run ("for (;;) {}"));
    REQUIRE (watchdog.end ());
    engine.isolate->CancelTerminateExecution ();

    watchdog.begin (5s);
    REQUIRE (engine.run ("'ok'", &result));
    REQUIRE_FALSE (watchdog.end ());
    REQUIRE (result == "ok");
}
