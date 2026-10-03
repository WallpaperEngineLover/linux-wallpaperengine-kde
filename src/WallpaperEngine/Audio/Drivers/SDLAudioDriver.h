#pragma once

#include <atomic>
#include <map>
#include <vector>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"

#include <SDL.h>

#define MAX_AUDIO_FRAME_SIZE 192000

namespace WallpaperEngine::Audio::Drivers {
struct SDLAudioBuffer {
    AudioStream* stream = nullptr;
    uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2] = { 0 };
    unsigned int audio_buf_size = 0;
    unsigned int audio_buf_index = 0;
    /** Per-stream volume override (0-128), -1 = use the driver's global volume */
    std::atomic<int> volume { -1 };
    std::atomic<float> gainLeft { 1.0f };
    std::atomic<float> gainRight { 1.0f };
    std::atomic<bool> paused { false };
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
    const std::map<int, SDLAudioBuffer*>& getStreams ();

    [[nodiscard]] AVSampleFormat getFormat () const override;
    [[nodiscard]] int getSampleRate () const override;
    [[nodiscard]] int getChannels () const override;
    [[nodiscard]] const SDL_AudioSpec& getSpec () const;

    [[nodiscard]] SDL_mutex* getStreamMutex () const;

private:
    SDL_mutex* m_streamListMutex;
    int m_lastStreamID = 0;
    SDL_AudioDeviceID m_deviceID;
    bool m_initialized = false;
    SDL_AudioSpec m_audioSpec {};
    std::map<int, SDLAudioBuffer*> m_streams {};
};
} // namespace WallpaperEngine::Audio::Drivers