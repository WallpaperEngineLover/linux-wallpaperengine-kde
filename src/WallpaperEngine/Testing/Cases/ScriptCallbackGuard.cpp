#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include "BareIsolate.h"
#include "WallpaperEngine/Scripting/JS.h"

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Testing::BareIsolate;

namespace {
void throwsStandard (const v8::FunctionCallbackInfo<v8::Value>&) { throw std::runtime_error ("native failure"); }

void throwsOther (const v8::FunctionCallbackInfo<v8::Value>&) { throw 42; }

void echoesData (const v8::FunctionCallbackInfo<v8::Value>& info) { info.GetReturnValue ().Set (info.Data ()); }

void addsOne (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (info[0]->NumberValue (info.GetIsolate ()->GetCurrentContext ()).FromMaybe (0) + 1);
}

// a bare isolate whose global holds the callbacks under test
struct Script : BareIsolate {
    Script () {
	this->inContext ([] (v8::Local<v8::Context> local) {
	    JS::set (local, local->Global (), "standard", JS::function (local, throwsStandard));
	    JS::set (local, local->Global (), "other", JS::function (local, throwsOther));
	    JS::set (
		local, local->Global (), "echo",
		JS::function (local, echoesData, JS::string (local->GetIsolate (), "kept"))
	    );
	    JS::set (local, local->Global (), "plusOne", JS::function (local, addsOne, {}, 1));
	});
    }

    std::string run (const std::string& source) {
	return BareIsolate::run (source, [this] (v8::Local<v8::Context>, v8::Local<v8::Value> result) {
	    return JS::toString (isolate, result);
	});
    }
};
}

TEST_CASE ("A native script callback that throws becomes a script error") {
    Script script;

    CHECK (script.run ("try { standard (); 'returned' } catch (e) { e.message }") == "native failure");
    CHECK (script.run ("try { other (); 'returned' } catch (e) { e.message }") == "unknown error");
    // an uncaught one is the script's own error, not the end of the process
    CHECK (script.run ("standard ()").starts_with ("throws: Error: native failure"));

    // called from compiled code too, not just the first interpreted calls
    CHECK (
	script.run (
	    "let caught = 0; for (let i = 0; i < 20000; i++) { try { standard () } catch (e) { caught++ } } caught"
	)
	== "20000"
    );
}

TEST_CASE ("A guarded callback keeps its data and its answers") {
    Script script;

    CHECK (script.run ("echo ()") == "kept");
    CHECK (script.run ("plusOne (41)") == "42");
    const auto guardedAddsOne = JS::guard (addsOne);
    CHECK (JS::guard (addsOne) == guardedAddsOne);
    CHECK (JS::guard (addsOne) != JS::guard (echoesData));
    CHECK (JS::guard (nullptr) == nullptr);
}
