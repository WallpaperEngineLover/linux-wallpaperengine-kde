#pragma once

#include <memory>
#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Render/CObject.h"

using namespace WallpaperEngine;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * WE's sound object (wallpaper64.exe 2.8.42, vtable off_140490AE8). Every file of the object is a voice, only one plays
 * at a time: play() stops them all and starts a random one. Loop mode loops a single file and otherwise moves on to
 * another random file when the current one is over, random mode waits mintime..maxtime between files, single plays
 * one file once. WE plays them through OpenAL Soft 1.21.1 (mediaextensions64.dll), whose speaker panning and distance
 * attenuation are reproduced here for mono files.
 */
class CSound final : public CObject {
public:
    CSound (Wallpapers::CScene& scene, const Sound& sound);
    ~CSound () override;

    /** Timers, volume changes and the spatial position, every frame for visible and hidden sounds (sub_1401F4F50) */
    void update (float dt);

    /** Overrides the volume (0-128) this sound's streams mix at instead of the global volume; nullopt = global */
    void setVolumeOverride (std::optional<int> volume);

    /** Script-facing playback control, thisScene.getLayer(<sound name>).play()/stop()/pause()/isPlaying() */
    void play ();
    void stop ();
    void pause ();
    [[nodiscard]] bool isPlaying () const;

private:
    enum class VoiceState { Stopped, Paused, Playing };

    struct Voice {
	std::string file;
	std::unique_ptr<Audio::AudioStream> stream;
	int streamId = -1;
	float duration = 0.0f;
	bool mono = false;
	VoiceState state = VoiceState::Stopped;
    };

    void loadVoices ();
    void playVoice (Voice& voice, bool loop);
    void pauseVoice (Voice& voice);
    void stopVoice (Voice& voice);
    void stopAllVoices ();
    /** Random voice, like WE's rand () / 32767 * count clamped to the last one */
    [[nodiscard]] Voice& pickVoice ();
    [[nodiscard]] bool hasActiveVoice () const;
    void volumeChanged ();

    /** WE's master volume (renderer +728), 0-1 */
    [[nodiscard]] float masterVolume () const;
    /** AL_GAIN WE gives every voice: volume squared times the master volume */
    [[nodiscard]] float sourceGain () const;
    /** Where WE puts the sound in OpenAL's listener space (sub_1401F5460) */
    [[nodiscard]] glm::vec3 spatialPosition () const;
    /** Left and right gain of a voice: AL_GAIN, and for mono files OpenAL's distance attenuation and panning */
    [[nodiscard]] glm::vec2 outputGains (const Voice& voice) const;
    void applyOutput (const Voice& voice) const;
    void applyOutput () const;

    std::vector<Voice> m_voices = {};

    const Sound& m_sound;
    /** Screen-level mute/ambient-volume policy from CScene::setAudioPolicy; nullopt = no override */
    std::optional<int> m_screenVolumeOverride;

    /** +764: countdown to the next play () (next file in loop mode, next play after the random delay) */
    float m_timer = 0.0f;
    /** +768: time left of the file that was started last */
    float m_remaining = 0.0f;
    /** +784 flags 0x40000000, 0x80000000 and 2 */
    bool m_paused = false;
    bool m_stopped = true;
    bool m_startSilent = false;

    float m_lastVolume = 0.0f;
    float m_lastMaster = 0.0f;
    /** startsilent changes rewrite the flag even after play () cleared it (sub_14019B6B0) */
    bool m_lastStartSilent = false;
    /** OpenAL source position, the origin (the listener) until spatialization moves it */
    glm::vec3 m_position = glm::vec3 (0.0f);
    /** only set by play () on spatialized sounds */
    float m_reference = 1.0f;
    float m_rolloff = 1.0f;
};
} // namespace WallpaperEngine::Render::Objects
