#pragma once

#include <atomic>
#include <pulse/pulseaudio.h>
#include <string>

namespace WallpaperEngine::Audio::Drivers::Detectors {
// whether the default output is headphones, what OpenAL Soft's WASAPI backend asks Windows (Headphones/Headset form
// factor on stereo, sub_180186400); follows sink changes like WE's reloadaudio a server that goes away is waited for
// with PA_CONTEXT_NOFAIL over D-Bus, without D-Bus or with PULSE_SERVER it retries every 2 s
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
}
