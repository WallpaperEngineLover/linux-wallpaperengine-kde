#pragma once

#include "PlaybackRecorder.h"
#include "WallpaperEngine/Audio/AudioInput.h"
#include "WallpaperEngine/Audio/SpectrumAnalyzer.h"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <optional>
#include <pulse/pulseaudio.h>
#include <string>
#include <thread>
#include <vector>

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder;

class PulseAudioPlaybackRecorder final : public PlaybackRecorder {
public:
    using Input = WallpaperEngine::Audio::AudioInput;

    struct PulseAudioData {
	PulseAudioPlaybackRecorder* owner = nullptr;
	pa_stream* captureStream = nullptr;
	std::string monitorName;
	bool captureLost = false;
	pa_context* context = nullptr;
	/** capture thread only: the device asked for and what the source list lookup found so far */
	std::string wantedInput = "default";
	std::string defaultMonitor;
	std::string foundSource;
	bool exactMatch = false;
	bool missingLogged = false;
    };

    struct InputInfo {
	std::string name;
	std::string description;
	bool monitor = false;
    };

    /** the server's sources, what --audio-input takes; nothing when the sound server did not answer in time */
    [[nodiscard]] static std::optional<std::vector<InputInfo>> listInputs ();

    PulseAudioPlaybackRecorder ();
    explicit PulseAudioPlaybackRecorder (const Input& input);
    ~PulseAudioPlaybackRecorder () override;

    void lock () const override;
    void unlock () const override;

    /** applied by the capture thread within 100 ms, a changed device re-takes the stream */
    void setInput (const Input& input);

    [[nodiscard]] std::string wantedInput () const;

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

    mutable Debugging::Mutex m_inputMutex;
    std::string m_inputDevice LWE_GUARDED_BY (m_inputMutex) = "default";
    std::atomic<float> m_threshold { 0.0f };
    std::atomic<bool> m_inputChanged { false };

    std::thread m_captureThread;
    mutable SDL_mutex* m_dataMutex = nullptr;
    std::atomic<bool> m_running { true };
};
}
