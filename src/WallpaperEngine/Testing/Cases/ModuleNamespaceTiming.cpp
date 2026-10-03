#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Scripting/JS.h"

#include <memory>
#include <string>
#include <tuple>

// The V8 mechanisms ScriptEngine relies on, checked without the engine/scene/GL stack: queueScript() sets
// m_runningModule before evaluating a module so createScriptProperties()/finish() can find it, and detects a module
// body that threw through the module's status; layers and WE's native objects are plain objects with native data
// properties.

using namespace WallpaperEngine::Scripting;

namespace {
int g_probeValue = -1;

void probe (const v8::FunctionCallbackInfo<v8::Value>& info) { info.GetReturnValue ().Set (g_probeValue); }

struct Isolate {
    Isolate () {
	JS::platform ();
	allocator.reset (v8::ArrayBuffer::Allocator::NewDefaultAllocator ());
	v8::Isolate::CreateParams params;
	params.array_buffer_allocator = allocator.get ();
	isolate = v8::Isolate::New (params);
    }

    ~Isolate () { isolate->Dispose (); }

    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    v8::Isolate* isolate;
};

v8::MaybeLocal<v8::Module>
noImports (v8::Local<v8::Context>, v8::Local<v8::String>, v8::Local<v8::FixedArray>, v8::Local<v8::Module>) {
    return {};
}

v8::Local<v8::Module> compileModule (v8::Isolate* isolate, const char* source, const char* name) {
    v8::ScriptCompiler::Source compileSource (JS::string (isolate, source), JS::origin (isolate, name, true));

    return v8::ScriptCompiler::CompileModule (isolate, &compileSource).ToLocalChecked ();
}
} // namespace

TEST_CASE ("V8 module top-level code sees external state set before Evaluate") {
    Isolate holder;
    auto* isolate = holder.isolate;
    const v8::Isolate::Scope isolateScope (isolate);
    const v8::HandleScope handleScope (isolate);
    const auto context = v8::Context::New (isolate);
    const v8::Context::Scope contextScope (context);

    JS::set (context, context->Global (), "probe", JS::function (context, probe));

    const auto module = compileModule (isolate, "export var x = probe();", "<test-module>");

    REQUIRE (module->InstantiateModule (context, noImports).FromMaybe (false));

    // what ScriptEngine::queueScript() does with m_runningModule right before Evaluate
    g_probeValue = 777;

    REQUIRE_FALSE (module->Evaluate (context).IsEmpty ());
    REQUIRE (module->GetStatus () == v8::Module::kEvaluated);

    const auto exports = module->GetModuleNamespace ().As<v8::Object> ();

    REQUIRE (JS::get (context, exports, "x")->Int32Value (context).FromMaybe (0) == 777);
}

TEST_CASE ("A module-level throw shows in the module's status instead of an empty Evaluate result") {
    Isolate holder;
    auto* isolate = holder.isolate;
    const v8::Isolate::Scope isolateScope (isolate);
    const v8::HandleScope handleScope (isolate);
    const auto context = v8::Context::New (isolate);
    const v8::Context::Scope contextScope (context);
    const v8::TryCatch tryCatch (isolate);

    // a script chaining calls on createScriptProperties() when that returned undefined throws at its top level
    const auto module = compileModule (isolate, "export var x = undefined.someMethod();", "<test-throwing-module>");

    REQUIRE (module->InstantiateModule (context, noImports).FromMaybe (false));

    // with top level await V8 hands back a rejected promise and keeps the exception in the module
    std::ignore = module->Evaluate (context);

    REQUIRE (module->GetStatus () == v8::Module::kErrored);
    CHECK (JS::toString (isolate, module->GetException ()).find ("undefined") != std::string::npos);
}

namespace {
void readOnlyGetter (v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (42);
}
} // namespace

TEST_CASE ("Native data properties behave like WE's template members in module code") {
    Isolate holder;
    auto* isolate = holder.isolate;
    const v8::Isolate::Scope isolateScope (isolate);
    const v8::HandleScope handleScope (isolate);
    const auto context = v8::Context::New (isolate);
    const v8::Context::Scope contextScope (context);
    const v8::TryCatch tryCatch (isolate);

    const v8::Local<v8::Object> layer = v8::Object::New (isolate);
    REQUIRE (
	layer
	    ->SetNativeDataProperty (context, JS::name (isolate, "instance"), readOnlyGetter, nullptr, {}, v8::ReadOnly)
	    .FromMaybe (false)
    );
    JS::set (context, context->Global (), "layer", layer);

    const auto module = compileModule (
	isolate,
	"export const keys = Object.keys(layer).join();\n"
	"export const value = layer.instance;\n"
	"layer.custom = 5;\n"
	"export const custom = layer.custom;\n"
	"let error = '';\n"
	"try { layer.instance = 1; } catch (e) { error = e.constructor.name; }\n"
	"export { error };\n",
	"<test-members>"
    );

    REQUIRE (module->InstantiateModule (context, noImports).FromMaybe (false));
    std::ignore = module->Evaluate (context);
    REQUIRE (module->GetStatus () == v8::Module::kEvaluated);

    const auto exports = module->GetModuleNamespace ().As<v8::Object> ();

    CHECK (JS::toString (isolate, JS::get (context, exports, "keys")) == "instance");
    CHECK (JS::get (context, exports, "value")->Int32Value (context).FromMaybe (0) == 42);
    CHECK (JS::get (context, exports, "custom")->Int32Value (context).FromMaybe (0) == 5);
    // modules are strict, writing a read only member throws like on WE's
    CHECK (JS::toString (isolate, JS::get (context, exports, "error")) == "TypeError");
}
