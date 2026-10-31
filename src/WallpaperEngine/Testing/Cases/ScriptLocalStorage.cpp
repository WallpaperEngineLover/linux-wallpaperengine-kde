#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include "BareIsolate.h"
#include "WallpaperEngine/Scripting/JS.h"
#include "WallpaperEngine/Scripting/LocalStorageObject.h"

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Testing::BareIsolate;

namespace {
// localStorage on a bare isolate with a chosen scene file and screen
struct Script : BareIsolate {
    explicit Script (const std::string& scene, const std::string& screen = "Monitor0") {
	this->inContext ([&] (v8::Local<v8::Context> local) {
	    storage = std::make_unique<LocalStorageObject> (
		isolate, local,
		LocalStorageObject::Host {
		    .identity = [scene] { return scene; },
		    .screen = [screen] { return screen; },
		    .evaluatingModuleBody = [this] { return moduleBody; },
		}
	    );
	    JS::set (local, local->Global (), "localStorage", storage->getInstance ());
	});
    }

    ~Script () { storage.reset (); }

    /** JSON, or "throws: <message>" */
    std::string run (const std::string& source) {
	return BareIsolate::run (source, [this] (v8::Local<v8::Context> local, v8::Local<v8::Value> result) {
	    if (result->IsUndefined ()) {
		return std::string ("undefined");
	    }

	    v8::Local<v8::String> json;
	    return v8::JSON::Stringify (local, result).ToLocal (&json) ? JS::toString (isolate, json)
								       : std::string ("?");
	});
    }

    std::unique_ptr<LocalStorageObject> storage;
    bool moduleBody = false;
};

void useTemporaryDirectory (const std::string& name) {
    const auto dir
	= std::filesystem::temp_directory_path () / ("lwe-script-storage-" + std::to_string (getpid ())) / name;
    std::filesystem::remove_all (dir);
    std::filesystem::create_directories (dir);
    LocalStorage::setDirectory (dir);
}
}

TEST_CASE ("Script localStorage hands values back as JSON") {
    useTemporaryDirectory ("values");
    Script script ("/scenes/values/scene.pkg");

    CHECK (script.run ("localStorage.set('n', 1.5); localStorage.get('n')") == "1.5");
    CHECK (script.run ("localStorage.set('s', 'text'); localStorage.get('s')") == "\"text\"");
    CHECK (script.run ("localStorage.set('b', false); localStorage.get('b')") == "false");
    CHECK (
	script.run ("localStorage.set('o', {a: [1, {b: null}]}); localStorage.get('o')") == "{\"a\":[1,{\"b\":null}]}"
    );
    CHECK (script.run ("localStorage.set('z', null); localStorage.get('z')") == "null");
    CHECK (script.run ("localStorage.get('missing')") == "undefined");

    // JSON.stringify gives undefined for a function and throws on a cycle, both store just the header
    CHECK (script.run ("localStorage.set('f', function () {}); localStorage.get('f')") == "null");
    CHECK (script.run ("var c = {}; c.c = c; localStorage.set('cycle', c); localStorage.get('cycle')") == "null");
}

TEST_CASE ("Script localStorage keeps the screen and global stores apart") {
    useTemporaryDirectory ("locations");
    Script script ("/scenes/locations/scene.pkg");

    CHECK (script.run ("[localStorage.LOCATION_GLOBAL, localStorage.LOCATION_SCREEN]") == "[\"global\",\"screen\"]");
    script.run ("localStorage.set('k', 'screen'); localStorage.set('k', 'global', 'global')");

    CHECK (script.run ("localStorage.get('k')") == "\"screen\"");
    CHECK (script.run ("localStorage.get('k', 'screen')") == "\"screen\"");
    // any capitalisation of "global" counts, anything else is the screen store
    CHECK (script.run ("localStorage.get('k', 'GLOBAL')") == "\"global\"");
    CHECK (script.run ("localStorage.get('k', localStorage.LOCATION_GLOBAL)") == "\"global\"");
    CHECK (script.run ("localStorage.get('k', 1)") == "\"screen\"");

    Script other ("/scenes/other/scene.pkg");
    CHECK (other.run ("localStorage.get('k', 'global')") == "undefined");
}

TEST_CASE ("Script localStorage has one store in a window") {
    useTemporaryDirectory ("window");
    // a window has no screen, both stores are the scene file's
    Script script ("/scenes/window/scene.pkg", "");

    script.run ("localStorage.set('k', 1, 'global')");
    CHECK (script.run ("localStorage.get('k')") == "1");
    CHECK (script.run ("localStorage.get('k', 'global')") == "1");
}

TEST_CASE ("Script localStorage follows WE's argument rules") {
    useTemporaryDirectory ("arguments");
    Script script ("/scenes/arguments/scene.pkg");

    // set with undefined is delete with the same arguments, whose location then is the undefined value: the screen
    script.run ("localStorage.set('k', 'screen'); localStorage.set('k', 'global', 'global')");
    script.run ("localStorage.set('k', undefined, 'global')");
    CHECK (script.run ("localStorage.get('k')") == "undefined");
    CHECK (script.run ("localStorage.get('k', 'global')") == "\"global\"");

    CHECK (script.run ("localStorage.delete('k', 'global')") == "true");
    CHECK (script.run ("localStorage.delete('k', 'global')") == "false");
    CHECK (script.run ("localStorage.set('a', 1); localStorage.clear()") == "true");
    CHECK (script.run ("localStorage.get('a')") == "undefined");

    CHECK (script.run ("localStorage.set(5, 1)") == "throws: SyntaxError: LocalStorageSet key not a string.");
    CHECK (script.run ("localStorage.get()") == "throws: SyntaxError: LocalStorageSet key not a string.");

    // 100000 byte limit
    CHECK (script.run ("localStorage.set('big', 'x'.repeat (99000))") == "undefined");
    CHECK (
	script.run ("localStorage.set('more', 'x'.repeat (2000))")
	== "throws: SyntaxError: LocalStorageSet failed, possibly out of memory."
    );
}

TEST_CASE ("Script localStorage throws at a module's top level") {
    useTemporaryDirectory ("toplevel");
    Script script ("/scenes/toplevel/scene.pkg");
    script.moduleBody = true;

    CHECK (
	script.run ("localStorage.set('k', 1)")
	== "throws: SyntaxError: LocalStorageSet cannot be cleared from global scope."
    );
    CHECK (
	script.run ("localStorage.get('k')")
	== "throws: SyntaxError: LocalStorageGet cannot be cleared from global scope."
    );
    CHECK (
	script.run ("localStorage.delete('k')")
	== "throws: SyntaxError: LocalStorageDelete cannot be cleared from global scope."
    );
    CHECK (
	script.run ("localStorage.clear()")
	== "throws: SyntaxError: LocalStorageClear cannot be cleared from global scope."
    );

    Script afterwards ("/scenes/toplevel/scene.pkg");
    CHECK (afterwards.run ("localStorage.get('k')") == "undefined");
}
