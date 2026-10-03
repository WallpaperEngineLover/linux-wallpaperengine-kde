#pragma once

#include <v8-array-buffer.h>
#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace v8 {
class Array;
class Function;
class Object;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class EngineObject {
public:
    EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);

    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    v8::Local<v8::Object> getInstance () const;
    ScriptEngine& getEngine () const { return m_engine; }

    /** engine.setTimeout/setInterval: the function that stops the timer, undefined when there is none */
    v8::Local<v8::Value> addTimer (v8::Local<v8::Function> callback, float seconds, bool interval);
    /** What a timer's stop function does, false when its timer is gone */
    bool stopTimer (v8::Local<v8::Array> data);

    void tick ();
    v8::Local<v8::Object> registerAudioBuffers (int resolution);

protected:
    // scenescript64 2.8.42 (sub_181655E10 / sub_1816562D0, run by sub_18164F800): seconds counted down by the
    // frame time, an interval goes back to its full duration after each call
    struct Timer {
	uint64_t id;
	// the script that started it, its callbacks run as that script
	std::string owner;
	uint64_t ownerOrder;
	float remaining;
	float duration;
	bool interval;
	v8::Global<v8::Function> callback;
	// the stop function's data, [id] until the timer is stopped or a timeout ran
	v8::Global<v8::Array> stopData;
    };

    std::vector<Timer> m_timers;
    uint64_t m_nextTimerId = 0;
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    v8::Global<v8::Object> m_instance;
    // left, right and average for 16, 32 and 64 bands, created by the first registerAudioBuffers call
    std::array<std::shared_ptr<v8::BackingStore>, 9> m_audioBuffers;
};
}
