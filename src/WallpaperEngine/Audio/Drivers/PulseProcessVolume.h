#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <sys/types.h>

#include <pulse/pulseaudio.h>

namespace WallpaperEngine::Audio::Drivers {
// volume of one process tree's PulseAudio streams, how --volume reaches web wallpapers since CEF can only mute; never
// blocks
class PulseProcessVolume {
public:
    explicit PulseProcessVolume (pid_t root);
    ~PulseProcessVolume ();

    PulseProcessVolume (const PulseProcessVolume&) = delete;
    PulseProcessVolume& operator= (const PulseProcessVolume&) = delete;

    void setVolume (float factor);

    void update ();

private:
    void connect ();
    void disconnect ();
    [[nodiscard]] bool descendsFromRoot (pid_t pid) const;
    void adjust (const pa_sink_input_info& info);

    static void onState (pa_context* context, void* userdata);
    static void onEvent (pa_context* context, pa_subscription_event_type_t type, uint32_t index, void* userdata);
    static void onStream (pa_context* context, const pa_sink_input_info* info, int last, void* userdata);

    pid_t m_root;
    float m_factor = 1.0f;
    pa_mainloop* m_loop = nullptr;
    pa_context* m_context = nullptr;
    std::map<uint32_t, bool> m_known;
    std::chrono::steady_clock::time_point m_retryAt {};
};
}
