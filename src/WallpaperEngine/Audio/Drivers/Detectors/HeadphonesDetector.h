#pragma once

#include <atomic>
#include <pulse/pulseaudio.h>
#include <string>

namespace WallpaperEngine::Audio::Drivers::Detectors {
/**
 * Whether the default output is headphones, the question OpenAL Soft's WASAPI backend asks Windows when WE opens its
 * audio device (endpoint form factor Headphones or Headset on a stereo device, mediaextensions64.dll sub_180186400).
 * Here it is the default sink's form factor or active port type. WE reopens the device on every endpoint change while
 * its "reloadaudio" setting is on (sub_14006CEA0), so the answer follows sink and default sink changes.
 *
 * A sound server that goes away (a PipeWire restart) is waited for, not polled: the new context connects with
 * PA_CONTEXT_NOFAIL, and libpulse then waits for org.pulseaudio.Server to appear on D-Bus, which pipewire-pulse and
 * PulseAudio claim once they listen. Without D-Bus, or with PULSE_SERVER set, libpulse can't wait that way and the
 * detector tries again every 2 s.
 */
class HeadphonesDetector {
public:
    HeadphonesDetector ();
    ~HeadphonesDetector ();

    HeadphonesDetector (const HeadphonesDetector&) = delete;
    HeadphonesDetector& operator= (const HeadphonesDetector&) = delete;

    [[nodiscard]] bool headphones () const { return this->m_headphones.load (std::memory_order_relaxed); }

private:
    static void contextState (pa_context* context, void* userdata);
    static void subscription (pa_context* context, pa_subscription_event_type_t type, uint32_t index, void* userdata);
    static void serverInfo (pa_context* context, const pa_server_info* info, void* userdata);
    static void sinkInfo (pa_context* context, const pa_sink_info* info, int eol, void* userdata);
    static void startupTimeout (pa_mainloop_api* api, pa_time_event* event, const struct timeval* tv, void* userdata);
    static void reconnectTimer (pa_mainloop_api* api, pa_time_event* event, const struct timeval* tv, void* userdata);

    void connect (pa_context_flags_t flags);
    void scheduleReconnect (pa_usec_t delay);
    void query ();

    pa_threaded_mainloop* m_mainloop = nullptr;
    pa_context* m_context = nullptr;
    pa_time_event* m_reconnect = nullptr;
    std::atomic<bool> m_headphones { false };
    /** mainloop thread only */
    std::string m_lastReport;
    int m_pending = 0;
    bool m_wasReady = false;
    bool m_startupExpired = false;
};
} // namespace WallpaperEngine::Audio::Drivers::Detectors
