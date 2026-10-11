#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <sys/types.h>

#include <pulse/pulseaudio.h>

namespace WallpaperEngine::Audio::Drivers {
/**
 * Sets the volume of the PulseAudio streams of one process tree (matched by application.process.id), which is
 * how --volume reaches a web wallpaper since CEF can only mute. Never blocks, failed connections are retried.
 */
class PulseProcessVolume {
public:
    explicit PulseProcessVolume (pid_t root);
    ~PulseProcessVolume ();

    PulseProcessVolume (const PulseProcessVolume&) = delete;
    PulseProcessVolume& operator= (const PulseProcessVolume&) = delete;

    /** amplitude factor like SDL's volume / 128 */
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
    /** sink input index -> whether it is ours */
    std::map<uint32_t, bool> m_known;
    std::chrono::steady_clock::time_point m_retryAt {};
};
} // namespace WallpaperEngine::Audio::Drivers
