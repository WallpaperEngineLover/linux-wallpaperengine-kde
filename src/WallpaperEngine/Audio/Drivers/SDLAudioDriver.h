#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/HeadphonesDetector.h"
#include "WallpaperEngine/Audio/Hrtf.h"

#include "WallpaperEngine/Debugging/ThreadSafety.h"
#include <SDL.h>

namespace WallpaperEngine::Audio::Drivers {
struct SDLAudioBuffer {
    AudioStream* stream = nullptr;
    /** Per-stream volume override (0-128), -1 = use the driver's global volume */
    std::atomic<int> volume { -1 };
    std::atomic<float> gainLeft { 1.0f };
    std::atomic<float> gainRight { 1.0f };
    std::atomic<bool> paused { false };
    /** stopStream: one more update fading to silence, then finished and freed by update () with the stream */
    std::atomic<bool> stopping { false };
    std::atomic<bool> finished { false };
    std::unique_ptr<AudioStream> owned;
    /** audio callback only: the file ran out and its stopping update was mixed, the voice stays silent from here */
    bool over = false;

    /** audio callback only: OpenAL's voice state for the gain path, set after a voice's first update */
    bool mixed = false;
    float currentLeft = 1.0f;
    float currentRight = 1.0f;
    /** the update a pause faded out, played again on resume (a stopping OpenAL voice keeps its position) */
    bool pauseFaded = false;
    std::vector<float> replay;

    Debugging::Mutex hrtfMutex;
    bool hrtfSet LWE_GUARDED_BY (hrtfMutex) = false;
    StreamHrtf hrtf LWE_GUARDED_BY (hrtfMutex) {};

    /** audio callback only: the filters of this callback, OpenAL's voice state and the decoded input */
    StreamHrtf hrtfMixing {};
    std::array<Hrtf::Channel, 2> hrtfChannels {};
    bool hrtfFading = false;
    int hrtfCounter = 0;
    std::vector<int> hrtfPasses;
    unsigned hrtfGeneration = 0;
    std::array<std::vector<float>, 2> hrtfSamples {};
};

class SDLAudioDriver final : public AudioDriver {
public:
    SDLAudioDriver (
	Application::ApplicationContext& applicationContext, Detectors::AudioPlayingDetector& detector,
	Recorders::PlaybackRecorder& recorder
    );
    ~SDLAudioDriver () override;

    int addStream (AudioStream* stream, int volume, float left, float right) override;
    void removeStream (int streamId) override;
    void setStreamVolume (int streamId, int volume) override;
    void setStreamGains (int streamId, float left, float right) override;
    void setStreamPaused (int streamId, bool paused) override;
    void stopStream (int streamId, std::unique_ptr<AudioStream> stream) override;
    void setStreamHrtf (int streamId, const StreamHrtf* hrtf) override;
    [[nodiscard]] const Hrtf::Store* getHrtf () const override;

    void update (float dt) override;

    void render (float* out, int frames);

    [[nodiscard]] AVSampleFormat getFormat () const override;
    [[nodiscard]] int getSampleRate () const override;
    [[nodiscard]] int getChannels () const override;

private:
    SDL_mutex* m_streamListMutex;
    int m_lastStreamID = 0;
    SDL_AudioDeviceID m_deviceID = 0;
    bool m_initialized = false;
    SDL_AudioSpec m_audioSpec {};
    std::map<int, SDLAudioBuffer*> m_streams {};

    [[nodiscard]] Hrtf::Mixer* getHrtfMixer () const;
    /** Changes every time HRTF is turned on, voices and the mixer start over then */
    [[nodiscard]] unsigned getHrtfGeneration () const;
    void mixUpdate (float* out, int frames);
    void mixHrtf (const std::vector<SDLAudioBuffer*>& voices, float* out, int frames);

    // frames WE's OpenAL Soft mixes per update: its 20 ms default (882 at 44.1 kHz) as a multiple of the WASAPI period
    // at the device rate (sub_180186400)
    int m_updateFrames = 960;
    std::vector<float> m_pending;
    /** audio callback only */
    std::vector<float> m_update;
    std::vector<SDLAudioBuffer*> m_hrtfVoices;
    std::array<std::vector<float>, 2> m_hrtfOutput;
    unsigned m_hrtfMixerGeneration = 0;

    void releaseFinishedStreams ();
    void updateHrtf ();

    std::unique_ptr<Detectors::HeadphonesDetector> m_headphonesDetector;
    /** loaded the first time HRTF turns on and kept, the callback may still be using them */
    std::unique_ptr<Hrtf::Store> m_hrtfStore;
    std::unique_ptr<Hrtf::Mixer> m_hrtfMixer;
    bool m_hrtfFailed = false;
    std::atomic<bool> m_hrtfEnabled { false };
    std::atomic<unsigned> m_hrtfGeneration { 0 };
};
}