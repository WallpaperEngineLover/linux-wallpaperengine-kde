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
	PulseAudioPlaybackRecorder* owner = nullptr;
	pa_stream* captureStream = nullptr;
	std::string monitorName;
	bool captureLost = false;
	pa_context* context = nullptr;
    };

    PulseAudioPlaybackRecorder ();
    ~PulseAudioPlaybackRecorder () override;

    void lock () const override;
    void unlock () const override;

    void consumeSamples (const float* samples, std::size_t frames);
    void dropBlock ();

private:
    void captureLoop ();
    void clearCaptured ();
    void logStats ();

    pa_mainloop* m_mainloop;
    pa_mainloop_api* m_mainloopApi;
    pa_context* m_context;
    PulseAudioData m_captureData;

    // only ever touched from the capture thread
    WallpaperEngine::Audio::SpectrumAnalyzer m_analyzer;
    std::chrono::steady_clock::time_point m_lastSamples = std::chrono::steady_clock::now ();
    std::chrono::steady_clock::time_point m_silentSince {};
    bool m_silenceCleared = false;

    struct CaptureStats {
	bool enabled = false;
	std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now ();
	unsigned packets = 0;
	unsigned holes = 0;
	unsigned blocks = 0;
	double sum = 0.0;
	double count = 0.0;
	float peak = 0.0f;
	float lowMax = 0.0f;
	float highMax = 0.0f;
    } m_stats;

    std::thread m_captureThread;
    mutable SDL_mutex* m_dataMutex = nullptr;
    std::atomic<bool> m_running { true };
};
}
