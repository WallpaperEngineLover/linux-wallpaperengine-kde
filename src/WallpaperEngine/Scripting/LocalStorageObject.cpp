#include "LocalStorageObject.h"

#include "JS.h"

#include <strings.h>

using namespace WallpaperEngine::Scripting;

namespace {
constexpr char VALUE_MAGIC[] = "LSKV0001";

LocalStorageObject& storageOf (const v8::FunctionCallbackInfo<v8::Value>& info) {
    return *static_cast<LocalStorageObject*> (info.Data ().As<v8::External> ()->Value ());
}

// WE throws at module top level, messages copied as is
bool rejectGlobalScope (const v8::FunctionCallbackInfo<v8::Value>& info, const char* message) {
    if (!storageOf (info).evaluatingModuleBody ()) {
	return false;
    }

    JS::throwSyntaxError (info.GetIsolate (), message);
    return true;
}

bool rejectKey (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length () > 0 && info[0]->IsString ()) {
	return false;
    }

    JS::throwSyntaxError (info.GetIsolate (), "LocalStorageSet key not a string.");
    return true;
}

// only a string matching "global" (_stricmp), anything else is the screen store
bool isGlobal (const v8::FunctionCallbackInfo<v8::Value>& info, int index) {
    if (info.Length () <= index || !info[index]->IsString ()) {
	return false;
    }

    return strcasecmp (JS::toString (info.GetIsolate (), info[index]).c_str (), "global") == 0;
}

std::string storageKey (const v8::FunctionCallbackInfo<v8::Value>& info) {
    return LocalStorage::sha1Hex (JS::toString (info.GetIsolate (), info[0]));
}

void localstorage_delete (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (rejectGlobalScope (info, "LocalStorageDelete cannot be cleared from global scope.") || rejectKey (info)) {
	return;
    }

    const bool removed = storageOf (info).getStore (isGlobal (info, 1)).remove (storageKey (info));
    info.GetReturnValue ().Set (removed);
}

void localstorage_set (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (rejectGlobalScope (info, "LocalStorageSet cannot be cleared from global scope.") || rejectKey (info)) {
	return;
    }

    const auto isolate = info.GetIsolate ();
    const v8::Local<v8::Value> value = info.Length () > 1 ? info[1] : v8::Undefined (isolate).As<v8::Value> ();

    // WE passes the same args to delete, whose location is then the undefined value: always the screen store
    if (value->IsUndefined ()) {
	localstorage_delete (info);
	return;
    }

    const bool global = isGlobal (info, 2);
    std::string json;

    {
	const v8::TryCatch tryCatch (isolate);
	v8::Local<v8::String> text;

	if (v8::JSON::Stringify (isolate->GetCurrentContext (), value).ToLocal (&text)) {
	    json = JS::toString (isolate, text);
	}
    }

    if (!storageOf (info).getStore (global).set (storageKey (info), VALUE_MAGIC + json)) {
	JS::throwSyntaxError (isolate, "LocalStorageSet failed, possibly out of memory.");
    }
}

void localstorage_get (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (rejectGlobalScope (info, "LocalStorageGet cannot be cleared from global scope.") || rejectKey (info)) {
	return;
    }

    const auto isolate = info.GetIsolate ();
    const auto data = storageOf (info).getStore (isGlobal (info, 1)).get (storageKey (info));

    if (!data.has_value () || data->size () < 8 || data->compare (0, 8, VALUE_MAGIC) != 0) {
	return;
    }

    // stops at the first NUL like WE, unparsable values give null
    const v8::TryCatch tryCatch (isolate);
    v8::Local<v8::Value> parsed;

    if (v8::JSON::Parse (isolate->GetCurrentContext (), JS::string (isolate, data->c_str () + 8)).ToLocal (&parsed)) {
	info.GetReturnValue ().Set (parsed);
    } else {
	info.GetReturnValue ().SetNull ();
    }
}

void localstorage_clear (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (rejectGlobalScope (info, "LocalStorageClear cannot be cleared from global scope.")) {
	return;
    }

    storageOf (info).getStore (isGlobal (info, 0)).clear ();
    info.GetReturnValue ().Set (true);
}
}

LocalStorageObject::LocalStorageObject (v8::Isolate* isolate, v8::Local<v8::Context> context, Host host) :
    m_isolate (isolate), m_host (std::move (host)) {
    const v8::Local<v8::Object> instance = v8::Object::New (isolate);
    const auto constant = static_cast<v8::PropertyAttribute> (v8::ReadOnly | v8::DontEnum | v8::DontDelete);
    const v8::Local<v8::External> self = v8::External::New (isolate, this);

    JS::set (context, instance, "set", JS::function (context, localstorage_set, self));
    JS::set (context, instance, "get", JS::function (context, localstorage_get, self));
    JS::set (context, instance, "delete", JS::function (context, localstorage_delete, self));
    JS::set (context, instance, "clear", JS::function (context, localstorage_clear, self));
    instance
	->DefineOwnProperty (context, JS::name (isolate, "LOCATION_GLOBAL"), JS::string (isolate, "global"), constant)
	.Check ();
    instance
	->DefineOwnProperty (context, JS::name (isolate, "LOCATION_SCREEN"), JS::string (isolate, "screen"), constant)
	.Check ();

    this->m_instance.Reset (isolate, instance);
}

v8::Local<v8::Object> LocalStorageObject::getInstance () const { return this->m_instance.Get (this->m_isolate); }

LocalStorage::Store& LocalStorageObject::getStore (const bool global) {
    auto& store = global ? this->m_globalStore : this->m_screenStore;

    if (!store) {
	// sub_140113C80: sha1 of the scene path, plus the screen string for the screen store
	const std::string identity = this->m_host.identity ();
	store = LocalStorage::open (LocalStorage::sha1Hex (global ? identity : identity + this->m_host.screen ()));
    }

    return *store;
}
