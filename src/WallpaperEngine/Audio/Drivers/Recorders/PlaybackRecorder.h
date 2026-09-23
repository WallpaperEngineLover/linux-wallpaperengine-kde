#pragma once

#include <functional>
#include <map>
#include <mutex>

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder {
public:
    /** Called with the 64 band spectrum every time a fresh one has been computed, from the recorder's capture thread */
    using SpectrumListener = std::function<void (const float* audio64)>;

    virtual ~PlaybackRecorder () = default;

    virtual void update ();

    /** Guards audio16/32/64 for recorders that fill them from a background capture thread; no-op by default */
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

    float audio16[16] = { 0 };
    float audio32[32] = { 0 };
    float audio64[64] = { 0 };

protected:
    void notifySpectrumListeners (const float* audio64);

private:
    std::mutex m_listenersMutex;
    std::map<int, SpectrumListener> m_listeners;
    int m_nextListenerId = 0;
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
