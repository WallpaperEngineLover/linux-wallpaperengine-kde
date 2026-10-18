#pragma once

#include "PlaybackRecorder.h"
#include "WallpaperEngine/Audio/SpectrumAnalyzer.h"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <pulse/pulseaudio.h>
#include <string>
#include <thread>

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder;

class PulseAudioPlaybackRecorder final : public PlaybackRecorder {
public:
    struct PulseAudioData {
	PulseAudioPlaybackRecorder* owner;
	pa_stream* captureStream;
	std::string monitorName;
	bool captureLost;
    };

    PulseAudioPlaybackRecorder ();
    ~PulseAudioPlaybackRecorder () override;

    void lock () const override;
    void unlock () const override;

    void consumeSamples (const float* samples, std::size_t frames);
    /** A gap in the capture, the block being collected is thrown away like WE does on a silent packet */
    void dropBlock ();

private:
    void captureLoop ();
    void clearCaptured ();

    pa_mainloop* m_mainloop;
    pa_mainloop_api* m_mainloopApi;
    pa_context* m_context;
    PulseAudioData m_captureData;

    // only ever touched from the capture thread
    WallpaperEngine::Audio::SpectrumAnalyzer m_analyzer;
    std::chrono::steady_clock::time_point m_lastSamples = std::chrono::steady_clock::now ();

    // own thread so capture keeps draining however long a frame takes
    std::thread m_captureThread;
    mutable SDL_mutex* m_dataMutex = nullptr;
    std::atomic<bool> m_running { true };
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
