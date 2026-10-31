#pragma once

#include <memory>
#include <string>

#include "WallpaperEngine/Scripting/JS.h"

namespace WallpaperEngine::Testing {
struct BareIsolate {
    BareIsolate () {
	Scripting::JS::platform ();
	allocator.reset (v8::ArrayBuffer::Allocator::NewDefaultAllocator ());
	v8::Isolate::CreateParams params;
	params.array_buffer_allocator = allocator.get ();
	isolate = v8::Isolate::New (params);

	const v8::Isolate::Scope isolateScope (isolate);
	const v8::HandleScope handleScope (isolate);
	context.Reset (isolate, v8::Context::New (isolate));
    }

    ~BareIsolate () {
	context.Reset ();
	isolate->Dispose ();
    }

    BareIsolate (const BareIsolate&) = delete;
    BareIsolate& operator= (const BareIsolate&) = delete;

    template <typename Fn> void inContext (Fn&& fn) {
	const v8::Isolate::Scope isolateScope (isolate);
	const v8::HandleScope handleScope (isolate);
	const auto local = context.Get (isolate);
	const v8::Context::Scope contextScope (local);
	fn (local);
    }

    template <typename Format> std::string run (const std::string& source, Format&& format) {
	std::string answer;

	this->inContext ([&] (v8::Local<v8::Context> local) {
	    const v8::TryCatch tryCatch (isolate);
	    v8::Local<v8::Script> script;
	    v8::Local<v8::Value> result;

	    if (!v8::Script::Compile (local, Scripting::JS::string (isolate, source)).ToLocal (&script)
		|| !script->Run (local).ToLocal (&result)) {
		answer = "throws: " + Scripting::JS::toString (isolate, tryCatch.Exception ());
		return;
	    }

	    answer = format (local, result);
	});

	return answer;
    }

    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    v8::Isolate* isolate = nullptr;
    v8::Global<v8::Context> context;
};
}
