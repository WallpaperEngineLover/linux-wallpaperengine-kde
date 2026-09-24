#pragma once

#include "PlaybackRecorder.h"
#include "WallpaperEngine/Audio/SpectrumAnalyzer.h"
#include "WallpaperEngine/Audio/SpectrumNormalizer.h"
#include "kiss_fftr.h"
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <string>
#include <pulse/pulseaudio.h>

#define WAVE_BUFFER_SIZE 1024

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder;

class PulseAudioPlaybackRecorder final : public PlaybackRecorder {
public:
    /**
     * Struct that contains all the required data for the PulseAudio callbacks
     */
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
    static int captureThreadEntry (void* userdata);
    void captureLoop ();
    void processWebFrame ();
    void clearCaptured ();

    pa_mainloop* m_mainloop;
    pa_mainloop_api* m_mainloopApi;
    pa_context* m_context;
    PulseAudioData m_captureData;

    // only ever touched from the capture thread
    WallpaperEngine::Audio::SpectrumAnalyzer m_analyzer;
    std::chrono::steady_clock::time_point m_lastSamples = std::chrono::steady_clock::now ();

    // web wallpapers get their own spectrum through the listeners, WE computes that one in its web process with
    // different rules, so it keeps the older mono FFT and normalizer
    kiss_fftr_cfg m_webFFT;
    WallpaperEngine::Audio::SpectrumNormalizer m_normalizer;
    std::chrono::steady_clock::time_point m_lastWebFrame = std::chrono::steady_clock::now ();
    float m_webSamples[WAVE_BUFFER_SIZE] = { 0.0f };
    std::size_t m_webSampleCount = 0;
    kiss_fft_cpx m_FFTinfo[WAVE_BUFFER_SIZE / 2 + 1] = { { .r = 0.0f, .i = 0.0f } };

    // Capture runs on its own thread (see the constructor) so it keeps draining PulseAudio
    // regardless of how long a render frame takes
    SDL_Thread* m_captureThread = nullptr;
    mutable SDL_mutex* m_dataMutex = nullptr;
    std::atomic<bool> m_running { true };
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
