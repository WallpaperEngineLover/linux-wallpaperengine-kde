#pragma once

#include "PlaybackRecorder.h"
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
	kiss_fftr_cfg kisscfg;
	uint8_t* audioBuffer;
	uint8_t* audioBufferTmp;
	size_t currentWritePointer;
	bool fullFrameReady;
	pa_stream* captureStream;
	std::string monitorName;
	bool captureLost;
    };

    PulseAudioPlaybackRecorder ();
    ~PulseAudioPlaybackRecorder () override;

    void update () override;
    void lock () const override;
    void unlock () const override;

private:
    static int captureThreadEntry (void* userdata);
    void captureLoop ();
    void processFrame ();

    pa_mainloop* m_mainloop;
    pa_mainloop_api* m_mainloopApi;
    pa_context* m_context;
    PulseAudioData m_captureData;

    // only ever touched from the capture thread
    WallpaperEngine::Audio::SpectrumNormalizer m_normalizer;
    std::chrono::steady_clock::time_point m_lastFrame = std::chrono::steady_clock::now ();

    float m_audioFFTbuffer[WAVE_BUFFER_SIZE] = { 0.0f };
    kiss_fft_cpx m_FFTinfo[WAVE_BUFFER_SIZE / 2 + 1] = { { .r = 0.0f, .i = 0.0f } };

    // Capture runs on its own thread (see the constructor) so it keeps draining PulseAudio
    // regardless of how long a render frame takes - see processFrame()'s comment for why.
    SDL_Thread* m_captureThread = nullptr;
    mutable SDL_mutex* m_dataMutex = nullptr;
    std::atomic<bool> m_running { true };
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
