#pragma once

#include <functional>
#include <map>
#include <mutex>

#include "WallpaperEngine/Audio/SpectrumProcessor.h"
#include "WallpaperEngine/Debugging/ThreadSafety.h"

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder {
    // declared first, audio16/32/64 below point into it
    Audio::SpectrumProcessor m_processor;

public:
    using SpectrumListener = std::function<void (const float* bands)>;

    virtual ~PlaybackRecorder () = default;

    virtual void update (float dt);

    /** Guards the captured spectrum for recorders that fill it from a background capture thread; no-op by default */
    virtual void lock () const { }
    virtual void unlock () const { }

    // pushes every new spectrum instead of polling audio64 per frame (a frame of latency at low --fps); not running
    // anymore once removeSpectrumListener returns
    int addSpectrumListener (SpectrumListener listener);
    void removeSpectrumListener (int id);

    // [left | right | average], only written from update() on the render thread
    const float* audio16 = m_processor.audio16;
    const float* audio32 = m_processor.audio32;
    const float* audio64 = m_processor.audio64;

protected:
    void notifySpectrumListeners (const float* bands);

    float m_captured[128] = { 0 };

private:
    Debugging::Mutex m_listenersMutex;
    std::map<int, SpectrumListener> m_listeners LWE_GUARDED_BY (m_listenersMutex);
    int m_nextListenerId LWE_GUARDED_BY (m_listenersMutex) = 0;
};
}
