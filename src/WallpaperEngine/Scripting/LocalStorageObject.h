#pragma once

#include "LocalStorage.h"

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

#include <functional>
#include <memory>
#include <string>

namespace v8 {
class Context;
class Isolate;
class Object;
}

namespace WallpaperEngine::Scripting {
/** scenescript64 2.8.42 localStorage (sub_181647AA0, sub_181658680..sub_181659150) */
class LocalStorageObject {
public:
    struct Host {
	std::function<std::string ()> identity;
	/** appended for the screen store; lazy, the screen is known after the engine */
	std::function<std::string ()> screen;
	/** calls throw while a module body runs */
	std::function<bool ()> evaluatingModuleBody;
    };

    LocalStorageObject (v8::Isolate* isolate, v8::Local<v8::Context> context, Host host);

    v8::Local<v8::Object> getInstance () const;
    LocalStorage::Store& getStore (bool global);
    [[nodiscard]] bool evaluatingModuleBody () const { return this->m_host.evaluatingModuleBody (); }

protected:
    v8::Isolate* m_isolate;
    Host m_host;
    std::shared_ptr<LocalStorage::Store> m_screenStore;
    std::shared_ptr<LocalStorage::Store> m_globalStore;

    v8::Global<v8::Object> m_instance;
};
}
