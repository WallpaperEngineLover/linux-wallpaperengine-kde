#include <SDL.h>

#include "CSound.h"

#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>

using namespace WallpaperEngine::Render::Objects;

namespace {
std::mt19937& generator () {
    static std::mt19937 engine { std::random_device {}() };
    return engine;
}

/** 0..32767 like the CRT rand () WE picks the file with (sub_1402C97A0) */
int crtRand () { return static_cast<int> (generator () () >> 17); }

/** [0, 1) from a Mersenne twister draw, sub_1401F4BC0 with the renderer's range 0..1 */
float uniform01 () { return static_cast<float> (generator () () >> 8) * 5.9604645e-8f; }

/** OpenAL Soft 1.21.1 alu.cpp: pulls azimuths in front towards the sides */
float scaleAzimuthFront (float azimuth, float scale) {
    const float absolute = std::fabs (azimuth);

    if (!(absolute >= glm::half_pi<float> ())) {
	return std::copysign (std::min (absolute * scale, glm::half_pi<float> ()), azimuth);
    }

    return azimuth;
}

/**
 * Speaker gains of a mono source for OpenAL Soft 1.21.1's stereo output without HRTF (pairwise render mode, alu.cpp
 * CalcPanningAndFilters): first order ambisonic coefficients of the direction through panning.cpp's StereoConfig
 * decoder. A source at the listener takes the channel's own angle, 0 for mono
 */
glm::vec2 stereoPan (const glm::vec3& direction, float distance) {
    float azimuth = 0.0f;
    float elevation = 0.0f;

    if (distance > FLT_EPSILON) {
	elevation = std::asin (std::clamp (direction.y, -1.0f, 1.0f));
	azimuth = scaleAzimuthFront (std::atan2 (direction.x, -direction.z), 1.5f);
    }

    const float acn1 = 1.732050808f * -std::sin (azimuth) * std::cos (elevation);
    const float acn3 = 1.732050808f * std::cos (azimuth) * std::cos (elevation);

    return { 5.00000000e-1f + 2.88675135e-1f * acn1 + 5.52305643e-2f * acn3,
	     5.00000000e-1f - 2.88675135e-1f * acn1 + 5.52305643e-2f * acn3 };
}
} // namespace

CSound::CSound (Wallpapers::CScene& scene, const Sound& sound) :
    CObject (scene, sound), m_sound (sound), m_startSilent (sound.startsilent.value_or (false)) {
    this->m_lastVolume
	= this->m_sound.volume && this->m_sound.volume->value ? this->m_sound.volume->value->getFloat () : 1.0f;
    this->m_lastMaster = this->masterVolume ();

    // sub_1401F4F20, when the object is added to the scene
    if (!this->m_startSilent) {
	this->play ();
    }

    // a script asked for this sound before it existed
    if (const auto request = scene.getSoundPlayRequest (sound.id); request.has_value ()) {
	if (request.value ()) {
	    this->play ();
	} else {
	    this->stop ();
	}
    }
}

CSound::~CSound () {
    for (auto& voice : this->m_voices) {
	this->stopVoice (voice);
    }
}

void CSound::loadVoices () {
    if (!this->getContext ().getApp ().getContext ().settings.audio.enabled) {
	return;
    }

    // WE loads every file the first time it plays and drops the ones that fail (sub_1401F5980)
    for (const auto& file : this->m_sound.sounds) {
	try {
	    const Audio::AudioStream probe (this->getScene ().getAudioContext (), this->getAssetLocator ().read (file));

	    this->m_voices.push_back (
		{ .file = file,
		  .duration = static_cast<float> (probe.getDuration ()),
		  .mono = probe.getSourceChannels () == 1 }
	    );
	} catch (const std::exception& e) {
	    sLog.error ("Cannot load sound ", file, ": ", e.what ());
	}
    }
}

void CSound::playVoice (Voice& voice, bool loop) {
    auto& audio = this->getScene ().getAudioContext ();

    if (voice.state == VoiceState::Paused && voice.streamId >= 0) {
	voice.stream->setRepeat (loop);
	audio.setStreamPaused (voice.streamId, false);
	voice.state = VoiceState::Playing;
	return;
    }

    this->stopVoice (voice);

    try {
	voice.stream = std::make_unique<Audio::AudioStream> (audio, this->getAssetLocator ().read (voice.file), loop);
    } catch (const std::exception& e) {
	sLog.error ("Cannot play sound ", voice.file, ": ", e.what ());
	return;
    }

    const glm::vec2 gains = this->outputGains (voice);

    voice.streamId = audio.addStream (voice.stream.get (), SDL_MIX_MAXVOLUME, gains.x, gains.y);
    voice.state = VoiceState::Playing;
}

void CSound::pauseVoice (Voice& voice) {
    if (voice.streamId >= 0) {
	this->getScene ().getAudioContext ().setStreamPaused (voice.streamId, true);
    }

    voice.state = VoiceState::Paused;
}

void CSound::stopVoice (Voice& voice) {
    if (voice.streamId >= 0) {
	this->getScene ().getAudioContext ().removeStream (voice.streamId);
    }

    voice.stream.reset ();
    voice.streamId = -1;
    voice.state = VoiceState::Stopped;
}

void CSound::stopAllVoices () {
    // sub_1401F58E0
    this->m_paused = false;
    this->m_stopped = true;
    this->m_timer = 0.0f;

    for (auto& voice : this->m_voices) {
	if (voice.state != VoiceState::Stopped) {
	    this->stopVoice (voice);
	}
    }
}

CSound::Voice& CSound::pickVoice () {
    const int count = static_cast<int> (this->m_voices.size ());
    const int index = static_cast<int> (static_cast<float> (crtRand ()) / 32767.0f * static_cast<float> (count) + 0.0f);

    return this->m_voices[std::max (std::min (count - 1, index), 0)];
}

bool CSound::hasActiveVoice () const {
    return std::ranges::any_of (this->m_voices, [] (const Voice& voice) { return voice.state != VoiceState::Stopped; });
}

void CSound::play () {
    // sub_1401F5980
    if (this->sourceGain () <= 0.0f) {
	this->m_paused = false;
	this->m_stopped = false;
	this->m_startSilent = false;
	return;
    }

    if (this->m_voices.empty ()) {
	this->loadVoices ();
    }

    if (this->m_sound.spatialization) {
	this->m_position = this->spatialPosition ();
    }

    const bool loop = this->m_voices.size () <= 1 && this->m_sound.playbackmode == PlaybackMode_Loop;

    if (this->m_paused) {
	this->m_paused = false;

	const auto paused = std::ranges::find_if (this->m_voices, [] (const Voice& voice) {
	    return voice.state == VoiceState::Paused;
	});

	if (paused != this->m_voices.end ()) {
	    this->playVoice (*paused, loop);
	}

	return;
    }

    if (!this->m_voices.empty ()) {
	switch (this->m_sound.playbackmode) {
	    case PlaybackMode_Random:
		if (this->m_timer != 0.0f) {
		    break;
		}

		this->stopAllVoices ();
		{
		    Voice& voice = this->pickVoice ();

		    this->playVoice (voice, false);
		    this->m_remaining = voice.duration;
		}
		this->m_timer = uniform01 () * (this->m_sound.maxtime - this->m_sound.mintime) + this->m_sound.mintime
		    + this->m_remaining;
		break;
	    case PlaybackMode_Single:
		{
		    this->stopAllVoices ();
		    this->m_timer = 0.0f;

		    Voice& voice = this->pickVoice ();

		    this->playVoice (voice, false);
		    this->m_remaining = voice.duration;
		    break;
		}
	    case PlaybackMode_Loop:
		{
		    this->stopAllVoices ();

		    Voice& voice = this->pickVoice ();

		    this->playVoice (voice, loop);
		    // several files: the next random one starts when this one is over
		    this->m_timer = this->m_voices.size () > 1 ? voice.duration : 0.0f;
		    break;
		}
	}
    }

    this->m_stopped = false;
    this->m_startSilent = false;
}

void CSound::stop () {
    // sub_1401F6E60
    this->m_paused = false;
    this->m_stopped = true;
    this->m_timer = 0.0f;

    for (auto& voice : this->m_voices) {
	if (voice.state != VoiceState::Stopped) {
	    this->stopVoice (voice);
	}
    }
}

void CSound::pause () {
    // sub_1401F6F00
    if (this->m_paused || this->m_stopped) {
	return;
    }

    this->m_paused = true;

    for (auto& voice : this->m_voices) {
	if (voice.state == VoiceState::Playing) {
	    this->pauseVoice (voice);
	}
    }
}

bool CSound::isPlaying () const {
    // sub_1401F6FB0
    if (this->m_sound.playbackmode == PlaybackMode_Single && this->m_remaining <= 0.0f) {
	return false;
    }

    if (this->m_sound.playbackmode == PlaybackMode_Random && this->m_timer <= 0.0f) {
	return false;
    }

    return this->hasActiveVoice () && !this->m_paused;
}

void CSound::volumeChanged () {
    // sub_1401F4C20, also what a master volume change does to every sound (sub_1401816D0)
    const auto mode = this->m_sound.playbackmode;

    if (mode != PlaybackMode_Single) {
	const bool active
	    = !(mode == PlaybackMode_Random && this->m_timer <= 0.0f) && this->hasActiveVoice () && !this->m_paused;

	if (!active && !this->m_paused && !this->m_startSilent && !this->m_stopped) {
	    this->play ();
	}
    }

    const bool audible = this->masterVolume () > 0.0f;

    for (auto& voice : this->m_voices) {
	this->applyOutput (voice);

	if (audible) {
	    if (!this->m_paused && voice.state == VoiceState::Paused) {
		this->playVoice (voice, mode == PlaybackMode_Loop);
		return;
	    }
	} else if (voice.state == VoiceState::Playing) {
	    this->pauseVoice (voice);

	    if (mode != PlaybackMode_Loop && this->m_remaining <= 0.0f) {
		this->stopVoice (voice);
	    }
	}
    }
}

void CSound::update (float dt) {
    const float volume
	= this->m_sound.volume && this->m_sound.volume->value ? this->m_sound.volume->value->getFloat () : 1.0f;
    const float master = this->masterVolume ();

    if (volume != this->m_lastVolume || master != this->m_lastMaster) {
	this->m_lastVolume = volume;
	this->m_lastMaster = master;
	this->volumeChanged ();
    }

    // sub_1401F4F50, dt / renderer +340, which stays 1
    if (!this->m_paused && !this->m_stopped) {
	if (this->sourceGain () > 0.0f && this->m_timer > 0.0f) {
	    this->m_timer -= dt;

	    if (this->m_timer <= 0.0f) {
		this->m_timer = 0.0f;
		this->play ();
	    }
	}

	if (master > 0.0f && this->m_remaining > 0.0f) {
	    this->m_remaining -= dt;
	}

	if (this->m_sound.spatialization) {
	    this->m_position = this->spatialPosition ();
	}
    }

    this->applyOutput ();
}

void CSound::setVolumeOverride (std::optional<int> volume) { this->m_screenVolumeOverride = volume; }

float CSound::masterVolume () const {
    const int base
	= this->m_screenVolumeOverride.value_or (this->getContext ().getApp ().getContext ().state.audio.volume);

    return static_cast<float> (base) / static_cast<float> (SDL_MIX_MAXVOLUME);
}

float CSound::sourceGain () const {
    const float volume
	= this->m_sound.volume && this->m_sound.volume->value ? this->m_sound.volume->value->getFloat () : 1.0f;

    // the voice volume call (+208) gets volume * volume * renderer +728, mediaextensions64 turns it into AL_GAIN as is
    return volume * volume * this->masterVolume ();
}

glm::vec3 CSound::spatialPosition () const {
    const auto& scene = this->getScene ();

    // renderer +324 is the frame counter, 0 while the scene is still loading
    if (scene.getFrameCounter () == 0) {
	return { 0.0f, 0.0f, 99999.0f };
    }

    const auto& camera = scene.getCamera ();
    const glm::vec3 origin (scene.objectWorldMatrix (this->m_sound)[3]);
    const glm::mat4& view = camera.isPerspective () ? camera.getView () : camera.getWorldView ();
    // renderer +364 / +376 / +352: rows 0 and 1 of the view and minus row 2 (sub_14017FA70), the eye is +104
    const glm::vec3 right (glm::row (view, 0));
    const glm::vec3 up (glm::row (view, 1));
    const glm::vec3 forward = -glm::vec3 (glm::row (view, 2));
    const glm::vec3 offset = origin - scene.getFog ().eyeWorld;
    glm::vec3 position (glm::dot (offset, right), -glm::dot (offset, up), glm::dot (offset, forward));

    // orthographic scenes only keep the direction, scaled down by the sound's depth over a quarter of the scene size
    if (camera.isOrthogonal ()) {
	const float length = std::sqrt (glm::dot (position, position));
	const float reach
	    = static_cast<float> (static_cast<int> (camera.getWidth ()) + static_cast<int> (camera.getHeight ()))
	    * 0.25f;

	position *= 1.0f / length;
	position *= std::max (reach - origin.z, 0.0f) / reach;
    }

    return position;
}

glm::vec2 CSound::outputGains (const Voice& voice) const {
    float gain = this->sourceGain ();
    glm::vec2 channels (1.0f);

    // OpenAL Soft only spatializes mono sources (AL_SOURCE_SPATIALIZE_SOFT defaults to auto), the others go straight
    // to their speakers; the source gain is clamped to AL_MAX_GAIN 1 on both paths
    if (voice.mono && this->getScene ().getAudioContext ().getChannels () == 2) {
	// WE only sets the reference distance and rolloff on spatialized sounds, the others keep OpenAL's 1 and 1 and
	// sit on the listener
	const float reference = this->m_sound.spatialization ? this->m_sound.mindistance : 1.0f;
	const float rolloff = this->m_sound.spatialization ? this->m_sound.attenuation : 1.0f;
	const float limit = std::max (reference / 1024.0f, FLT_EPSILON);
	glm::vec3 direction = this->m_position;
	float distance = 0.0f;

	if (glm::dot (direction, direction) > limit * limit) {
	    distance = std::sqrt (glm::dot (direction, direction));
	    direction *= 1.0f / distance;
	}

	// AL_INVERSE_DISTANCE_CLAMPED, WE never changes the distance model or AL_MAX_DISTANCE
	const float clamped = std::clamp (distance, reference, FLT_MAX);

	if (reference > 0.0f) {
	    const float scaled = reference + (clamped - reference) * rolloff;

	    if (scaled > 0.0f) {
		gain *= reference / scaled;
	    }
	}

	channels = stereoPan (direction, distance);
    }

    return std::clamp (gain, 0.0f, 1.0f) * channels;
}

void CSound::applyOutput (const Voice& voice) const {
    if (voice.streamId < 0) {
	return;
    }

    const glm::vec2 gains = this->outputGains (voice);

    this->getScene ().getAudioContext ().setStreamGains (voice.streamId, gains.x, gains.y);
}

void CSound::applyOutput () const {
    for (const auto& voice : this->m_voices) {
	this->applyOutput (voice);
    }
}
