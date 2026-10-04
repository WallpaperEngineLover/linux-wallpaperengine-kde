#pragma once

#include <v8.h>

#include <initializer_list>
#include <string>
#include <string_view>

/**
 * Small helpers over the V8 API the scripting code uses everywhere, plus the few calls whose signature changed
 * between the V8 versions distributions ship (10.2 in Ubuntu 24.04's libnode up to 14.x in current Node.js)
 */
namespace WallpaperEngine::Scripting::JS {
/** Creates the process wide V8 platform once, every isolate shares it */
v8::Platform& platform ();

/**
 * Sets TZ to the local IANA zone name when unset or a path, ICU loses DST otherwise. Call before other threads start
 */
void useNamedTimeZone ();

inline v8::Local<v8::String> string (v8::Isolate* isolate, std::string_view text) {
    return v8::String::NewFromUtf8 (isolate, text.data (), v8::NewStringType::kNormal, static_cast<int> (text.size ()))
	.ToLocalChecked ();
}

/** For names used over and over (property keys, export names) */
inline v8::Local<v8::String> name (v8::Isolate* isolate, std::string_view text) {
    return v8::String::NewFromUtf8 (
	       isolate, text.data (), v8::NewStringType::kInternalized, static_cast<int> (text.size ())
    )
	.ToLocalChecked ();
}

/** ToString of any value, empty when that throws */
inline std::string toString (v8::Isolate* isolate, v8::Local<v8::Value> value) {
    const v8::String::Utf8Value text (isolate, value);

    return *text == nullptr ? std::string () : std::string (*text, text.length ());
}

inline v8::Local<v8::External> external (v8::Isolate* isolate, void* pointer) {
#if V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 6)
    return v8::External::New (isolate, pointer, v8::kExternalPointerTypeTagDefault);
#else
    return v8::External::New (isolate, pointer);
#endif
}

template <typename T> T* unwrap (v8::Local<v8::Value> value) {
    if (value.IsEmpty () || !value->IsExternal ()) {
	return nullptr;
    }

#if V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 6)
    return static_cast<T*> (value.As<v8::External> ()->Value (v8::kExternalPointerTypeTagDefault));
#else
    return static_cast<T*> (value.As<v8::External> ()->Value ());
#endif
}

/** Function data holding several values, read back with dataAt () */
inline v8::Local<v8::Array> data (v8::Isolate* isolate, std::initializer_list<v8::Local<v8::Value>> values) {
    // Array::New wants a mutable pointer
    v8::Local<v8::Value> elements[8];
    size_t count = 0;

    for (const auto& value : values) {
	elements[count++] = value;
    }

    return v8::Array::New (isolate, elements, count);
}

inline v8::Local<v8::Value> dataAt (const v8::FunctionCallbackInfo<v8::Value>& info, uint32_t index) {
    return info.Data ().As<v8::Array> ()->Get (info.GetIsolate ()->GetCurrentContext (), index).ToLocalChecked ();
}

/** A native function the way scenescript64 hands them out: no name, length as given (0 for WE's own methods) */
inline v8::Local<v8::Function> function (
    v8::Local<v8::Context> context, v8::FunctionCallback callback, v8::Local<v8::Value> data = {}, int length = 0
) {
    return v8::Function::New (context, callback, data, length, v8::ConstructorBehavior::kThrow).ToLocalChecked ();
}

/** The same callback for every `magic` value, so one switch can serve several methods */
template <void (*Callback) (const v8::FunctionCallbackInfo<v8::Value>&, int), int Magic>
void bind (const v8::FunctionCallbackInfo<v8::Value>& info) {
    Callback (info, Magic);
}

inline void
set (v8::Local<v8::Context> context, v8::Local<v8::Object> object, std::string_view key, v8::Local<v8::Value> value) {
    object->Set (context, name (v8::Isolate::GetCurrent (), key), value).Check ();
}

/** A plain data property, no setters or prototype setters run */
inline void define (
    v8::Local<v8::Context> context, v8::Local<v8::Object> object, std::string_view key, v8::Local<v8::Value> value
) {
    object->CreateDataProperty (context, name (v8::Isolate::GetCurrent (), key), value).Check ();
}

/** undefined when the lookup throws */
inline v8::Local<v8::Value> get (v8::Local<v8::Context> context, v8::Local<v8::Value> object, std::string_view key) {
    if (!object->IsObject ()) {
	return v8::Undefined (v8::Isolate::GetCurrent ());
    }

    v8::Local<v8::Value> result;

    if (!object.As<v8::Object> ()->Get (context, name (v8::Isolate::GetCurrent (), key)).ToLocal (&result)) {
	return v8::Undefined (v8::Isolate::GetCurrent ());
    }

    return result;
}

inline v8::Local<v8::Value> get (v8::Local<v8::Context> context, v8::Local<v8::Value> object, uint32_t index) {
    v8::Local<v8::Value> result;

    if (!object->IsObject () || !object.As<v8::Object> ()->Get (context, index).ToLocal (&result)) {
	return v8::Undefined (v8::Isolate::GetCurrent ());
    }

    return result;
}

inline double toNumber (v8::Local<v8::Context> context, v8::Local<v8::Value> value, double fallback = 0.0) {
    return value->NumberValue (context).FromMaybe (fallback);
}

inline void
setPrototype (v8::Local<v8::Context> context, v8::Local<v8::Object> object, v8::Local<v8::Value> prototype) {
#if V8_MAJOR_VERSION > 13 || (V8_MAJOR_VERSION == 13 && V8_MINOR_VERSION >= 6)
    object->SetPrototypeV2 (context, prototype).Check ();
#else
    object->SetPrototype (context, prototype).Check ();
#endif
}

inline void throwTypeError (v8::Isolate* isolate, std::string_view message) {
    isolate->ThrowException (v8::Exception::TypeError (string (isolate, message)));
}

inline void throwSyntaxError (v8::Isolate* isolate, std::string_view message) {
    isolate->ThrowException (v8::Exception::SyntaxError (string (isolate, message)));
}

inline void throwReferenceError (v8::Isolate* isolate, std::string_view message) {
    isolate->ThrowException (v8::Exception::ReferenceError (string (isolate, message)));
}

inline v8::ScriptOrigin origin (v8::Isolate* isolate, std::string_view resource, bool module) {
    // V8 12 added the constructor without the isolate, 13 dropped the old one
#if V8_MAJOR_VERSION >= 12
    return { string (isolate, resource), 0, 0, false, -1, {}, false, false, module };
#else
    return { isolate, string (isolate, resource), 0, 0, false, -1, {}, false, false, module };
#endif
}
} // namespace WallpaperEngine::Scripting::JS
