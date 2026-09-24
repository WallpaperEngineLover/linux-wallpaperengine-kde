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
    /** Called with the 64 band spectrum every time a fresh one has been computed, from the recorder's capture thread */
    using SpectrumListener = std::function<void (const float* audio64)>;

    virtual ~PlaybackRecorder () = default;

    /**
     * Runs once per rendered frame, turns the latest captured spectrum into audio16/32/64
     *
     * @param dt Seconds since the previous frame, scaled by the playback speed
     */
    virtual void update (float dt);

    /** Guards the captured spectrum for recorders that fill it from a background capture thread; no-op by default */
    virtual void lock () const { }
    virtual void unlock () const { }

    /**
     * Registers a callback to be pushed every new spectrum instead of polling audio64 once per rendered frame,
     * which at low --fps values adds up to a whole frame of latency. Does nothing for recorders that don't capture.
     *
     * @return An id for removeSpectrumListener. After that call returns the callback is guaranteed not to be running
     */
    int addSpectrumListener (SpectrumListener listener);
    void removeSpectrumListener (int id);

    // [left | right | average], only written from update() on the render thread
    const float* audio16 = m_processor.audio16;
    const float* audio32 = m_processor.audio32;
    const float* audio64 = m_processor.audio64;

protected:
    void notifySpectrumListeners (const float* audio64);

    /** [left 64 | right 64] linear band levels from the capture thread, guarded by lock()/unlock() */
    float m_captured[128] = { 0 };

private:
    std::mutex m_listenersMutex;
    std::map<int, SpectrumListener> m_listeners;
    int m_nextListenerId = 0;
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
