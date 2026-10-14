#pragma once

#include <memory>
#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

using namespace WallpaperEngine;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

// WE's sound object (2.8.42, off_140490AE8): one voice per file, play() stops all and starts a random one; loop moves
// on to another random file when one ends, random waits mintime..maxtime between files, single plays once. OpenAL
// Soft 1.21.1 panning and attenuation reproduced for mono files
class CSound final : public Scripting::ScriptableObject {
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
    [[nodiscard]] bool isPlaying () const override;
    void applyPlayback (Playback playback) override;

private:
    enum class VoiceState { Stopped, Paused, Playing };

    struct SourceParams {
	float gain;
	glm::vec3 direction;
	float distance;

	bool operator== (const SourceParams&) const = default;
    };

    /** What applyOutput last handed the driver, so an unchanged voice skips the HRTF coefficients and the lock */
    struct AppliedOutput {
	int streamId = -1;
	const Audio::Hrtf::Store* hrtf = nullptr;
	SourceParams source {};
    };

    struct Voice {
	std::string file;
	std::unique_ptr<Audio::AudioStream> stream;
	int streamId = -1;
	float duration = 0.0f;
	bool mono = false;
	VoiceState state = VoiceState::Stopped;
	mutable AppliedOutput applied;
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
    /** What OpenAL's source update works out for a voice: the clamped gain, and for mono files direction and distance
     */
    [[nodiscard]] SourceParams sourceParams (const Voice& voice) const;
    /** Left and right gain of a voice: AL_GAIN, and for mono files OpenAL's distance attenuation and panning */
    [[nodiscard]] glm::vec2 outputGains (const Voice& voice) const;
    /** The voice's HRTF filters while the output renders for headphones (OpenAL's "full" HRTF mode) */
    [[nodiscard]] Audio::Drivers::StreamHrtf hrtfOutput (const Voice& voice, const Audio::Hrtf::Store& hrtf) const;
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
