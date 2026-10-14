#pragma once

#include <functional>
#include <map>
#include <mutex>

#include "WallpaperEngine/Audio/SpectrumProcessor.h"

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder {
    // declared first, audio16/32/64 below point into it
    Audio::SpectrumProcessor m_processor;

public:
    /** [left 64 | right 64] bands per analyzed block (zeros once quiet), called on the capture thread */
    using SpectrumListener = std::function<void (const float* bands)>;

    virtual ~PlaybackRecorder () = default;

    // once per rendered frame, latest spectrum into audio16/32/64
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

    /** [left 64 | right 64] linear band levels from the capture thread, guarded by lock()/unlock() */
    float m_captured[128] = { 0 };

private:
    std::mutex m_listenersMutex;
    std::map<int, SpectrumListener> m_listeners;
    int m_nextListenerId = 0;
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
