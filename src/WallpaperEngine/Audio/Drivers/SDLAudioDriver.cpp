#include "SDLAudioDriver.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

#define SDL_AUDIO_BUFFER_SIZE 1024

using namespace WallpaperEngine::Audio;
using namespace WallpaperEngine::Audio::Drivers;

namespace {
/** a stopping voice without a buffer fades over at most this many samples (voice.cpp Voice::mix) */
constexpr unsigned StopFadeSamples = 64;

/**
 * Per channel gain ramp of one update, OpenAL's MixSamples: from the current gains to the target over counter frames,
 * the target from there on. A step no bigger than FLT_EPSILON goes straight to the target
 */
struct GainRamp {
    float left, right;
    float targetLeft, targetRight;
    int counter;

    [[nodiscard]] float at (int channel, int frame) const {
	if (channel > 1) {
	    return (this->at (0, frame) + this->at (1, frame)) * 0.5f;
	}

	const float gain = channel == 0 ? this->left : this->right;
	const float target = channel == 0 ? this->targetLeft : this->targetRight;
	const float step = (target - gain) * (this->counter > 0 ? 1.0f / static_cast<float> (this->counter) : 0.0f);

	if (!(std::abs (step) > FLT_EPSILON) || frame >= this->counter) {
	    return target;
	}

	return gain + step * static_cast<float> (frame);
    }
};

void audio_callback (void* userdata, uint8_t* streamData, int length) {
    auto* driver = static_cast<SDLAudioDriver*> (userdata);

    memset (streamData, 0, length);

    if (driver->getAudioDetector ().anythingPlaying ()) {
	return;
    }

    driver->render (
	reinterpret_cast<float*> (streamData), length / static_cast<int> (sizeof (float)) / driver->getChannels ()
    );
}
}

void SDLAudioDriver::render (float* out, int frames) {
    const int channels = this->m_audioSpec.channels;
    const size_t wanted = static_cast<size_t> (frames) * channels;

    SDL_LockMutex (this->m_streamListMutex);

    // the WASAPI mixer thread renders an update once that much buffer is free, in chunks of at most BufferLineSize
    while (this->m_pending.size () < wanted && this->getApplicationContext ().state.general.keepRunning) {
	for (int done = 0; done < this->m_updateFrames;) {
	    const int count = std::min (this->m_updateFrames - done, static_cast<int> (Hrtf::BufferLineSize));
	    const size_t offset = this->m_pending.size ();

	    this->m_pending.resize (offset + static_cast<size_t> (count) * channels, 0.0f);
	    this->mixUpdate (this->m_pending.data () + offset, count);
	    done += count;
	}
    }

    const size_t available = std::min (wanted, this->m_pending.size ());

    for (size_t i = 0; i < available; i++) {
	out[i] = std::clamp (this->m_pending[i], -1.0f, 1.0f);
    }

    this->m_pending.erase (this->m_pending.begin (), this->m_pending.begin () + static_cast<long> (available));

    // TODO: do we also need to lock while audio is playing, or wait until the stream is unused?
    SDL_UnlockMutex (this->m_streamListMutex);
}

void SDLAudioDriver::mixUpdate (float* out, int frames) {
    const int channels = this->m_audioSpec.channels;
    Hrtf::Mixer* hrtf = channels >= 2 ? this->getHrtfMixer () : nullptr;
    auto& update = this->m_update;

    this->m_hrtfVoices.clear ();

    for (auto* buffer : this->m_streams | std::views::values) {
	if (!buffer->stream->isInitialized () || buffer->finished.load (std::memory_order_relaxed)) {
	    continue;
	}

	// OpenAL's Stopping state: alSourceStop and alSourcePause both mix one more update with the gain going to 0,
	// and so does a voice whose file ran out in the last update
	const bool stopping = buffer->stopping.load (std::memory_order_acquire);
	const bool pausing = !stopping && buffer->paused.load (std::memory_order_relaxed);
	const bool runOut = !stopping && !pausing && buffer->stream->ended ();

	if (buffer->over || (stopping && buffer->pauseFaded)) {
	    if (stopping) {
		buffer->finished.store (true, std::memory_order_release);
	    }

	    continue;
	}

	if (pausing && buffer->pauseFaded) {
	    continue;
	}

	buffer->pauseFaded = false;

	// a stopping voice that never mixed has no gains to fade from (Counter 0), it goes silent right away
	if ((stopping || runOut) && !buffer->mixed) {
	    if (stopping) {
		buffer->finished.store (true, std::memory_order_release);
	    } else {
		buffer->over = true;
	    }

	    continue;
	}

	const bool ending = stopping || pausing || runOut;
	// without a buffer the fade is cut short, a paused voice keeps its buffer and fades over the whole update
	const bool detached = stopping || runOut;
	bool viaHrtf = false;

	if (hrtf != nullptr) {
	    std::lock_guard lock (buffer->hrtfMutex);

	    if (buffer->hrtfSet) {
		buffer->hrtfMixing = buffer->hrtf;
		viaHrtf = true;
	    }
	}

	update.assign (static_cast<size_t> (frames) * channels, 0.0f);

	// a resumed voice replays audio it already read, its passes are not known any more
	if (viaHrtf) {
	    buffer->hrtfPasses
		= !detached && !buffer->replay.empty () ? std::vector { frames } : buffer->stream->passes (frames);
	}

	if (detached) {
	    // alSourceStop detaches the buffer before the update (ProcessVoiceChanges), so the voice plays out what it
	    // loaded last time instead
	    buffer->stream->stopTail (update.data (), frames);
	} else {
	    const size_t replayed = std::min (buffer->replay.size (), update.size ());

	    std::copy_n (buffer->replay.begin (), replayed, update.begin ());
	    buffer->replay.erase (buffer->replay.begin (), buffer->replay.begin () + static_cast<long> (replayed));

	    int filled = static_cast<int> (replayed) / channels;

	    while (filled < frames && this->getApplicationContext ().state.general.keepRunning) {
		const int read
		    = buffer->stream->read (update.data () + static_cast<size_t> (filled) * channels, frames - filled);

		if (read <= 0) {
		    if (read == 0) {
			SDL_CondSignal (buffer->stream->getWaitCondition ());
		    }

		    break;
		}

		filled += read;
	    }

	    // a stopping voice doesn't advance, a resumed one plays this update again
	    if (pausing) {
		buffer->replay.insert (
		    buffer->replay.begin (), update.begin (), update.begin () + static_cast<long> (filled) * channels
		);
	    }
	}

	const int streamVolume = buffer->volume.load (std::memory_order_relaxed);
	const int volume = streamVolume >= 0 ? streamVolume : this->getApplicationContext ().state.audio.volume;
	const float scale = static_cast<float> (volume) / static_cast<float> (SDL_MIX_MAXVOLUME);
	const int counter = detached ? std::min (frames, static_cast<int> (StopFadeSamples)) : frames;

	if (viaHrtf) {
	    for (int channel = 0; channel < buffer->hrtfMixing.channels; channel++) {
		auto& samples = buffer->hrtfSamples[channel];

		samples.resize (frames);

		for (int i = 0; i < frames; i++) {
		    samples[i] = update[static_cast<size_t> (i) * channels + channel] * scale;
		}

		if (ending) {
		    buffer->hrtfMixing.filters[channel].gain = 0.0f;
		}
	    }

	    buffer->hrtfCounter = counter;
	    this->m_hrtfVoices.push_back (buffer);
	} else {
	    const float targetLeft = ending ? 0.0f : buffer->gainLeft.load (std::memory_order_relaxed);
	    const float targetRight = ending ? 0.0f : buffer->gainRight.load (std::memory_order_relaxed);

	    // a voice's first update starts at its target gains, the later ones ramp over the update
	    if (!buffer->mixed) {
		buffer->currentLeft = targetLeft;
		buffer->currentRight = targetRight;
	    }

	    const GainRamp ramp { buffer->currentLeft, buffer->currentRight, targetLeft, targetRight, counter };

	    buffer->currentLeft = targetLeft;
	    buffer->currentRight = targetRight;

	    for (int i = 0; i < frames; i++) {
		for (int channel = 0; channel < channels; channel++) {
		    const size_t index = static_cast<size_t> (i) * channels + channel;

		    out[index] += update[index] * ramp.at (channel, i) * scale;
		}
	    }
	}

	buffer->mixed = true;

	if (pausing) {
	    buffer->pauseFaded = true;
	} else if (stopping) {
	    buffer->finished.store (true, std::memory_order_release);
	} else if (runOut) {
	    buffer->over = true;
	}
    }

    // the accumulator carries the tail of the last updates, so it runs even with no voice left
    if (hrtf != nullptr) {
	this->mixHrtf (this->m_hrtfVoices, out, frames);
    }
}

void SDLAudioDriver::mixHrtf (const std::vector<SDLAudioBuffer*>& voices, float* out, int frames) {
    Hrtf::Mixer& mixer = *this->m_hrtfMixer;
    const unsigned generation = this->getHrtfGeneration ();
    const int channels = this->m_audioSpec.channels;

    if (this->m_hrtfMixerGeneration != generation) {
	this->m_hrtfMixerGeneration = generation;
	mixer.clear ();
    }

    auto& [left, right] = this->m_hrtfOutput;

    left.assign (frames, 0.0f);
    right.assign (frames, 0.0f);

    for (auto* voice : voices) {
	if (voice->hrtfGeneration != generation) {
	    voice->hrtfGeneration = generation;
	    voice->hrtfChannels = {};
	    voice->hrtfFading = false;
	}

	unsigned counter = voice->hrtfFading ? static_cast<unsigned> (voice->hrtfCounter) : 0;
	unsigned outPos = 0;

	for (const int pass : voice->hrtfPasses) {
	    const auto count = static_cast<unsigned> (pass);

	    for (int channel = 0; channel < voice->hrtfMixing.channels; channel++) {
		mixer.mix (
		    voice->hrtfChannels[channel], voice->hrtfMixing.filters[channel],
		    voice->hrtfSamples[channel].data () + outPos, count, counter, outPos
		);
	    }

	    outPos += count;
	    counter = std::max (count, counter) - count;
	}

	voice->hrtfFading = true;
    }

    mixer.finish (left.data (), right.data (), static_cast<unsigned> (frames));

    for (int i = 0; i < frames; i++) {
	out[static_cast<size_t> (i) * channels] += left[i];
	out[static_cast<size_t> (i) * channels + 1] += right[i];
    }
}

SDLAudioDriver::SDLAudioDriver (
    Application::ApplicationContext& applicationContext, Detectors::AudioPlayingDetector& detector,
    Recorders::PlaybackRecorder& recorder
) : AudioDriver (applicationContext, detector, recorder), m_audioSpec () {
    this->m_streamListMutex = SDL_CreateMutex ();

    if (SDL_InitSubSystem (SDL_INIT_AUDIO) < 0) {
	sLog.error ("Cannot initialize SDL audio system, SDL_GetError: ", SDL_GetError ());
	sLog.error ("Continuing without audio support");

	return;
    }

    const SDL_AudioSpec requestedSpec = { .freq = 48000,
					  .format = AUDIO_F32,
					  .channels = 2,
					  .samples = SDL_AUDIO_BUFFER_SIZE,
					  .callback = audio_callback,
					  .userdata = this };

    this->m_deviceID = SDL_OpenAudioDevice (
	nullptr, false, &requestedSpec, &this->m_audioSpec,
	SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE
    );

    if (this->m_deviceID == 0) {
	sLog.error ("SDL_OpenAudioDevice: ", SDL_GetError ());
	return;
    }

    // WasapiPlayback::resetProxy: OpenAL's 20 ms default update in 100 ns units, rounded to a multiple of the
    // endpoint's default period (Windows' shared mode default, 10 ms), in frames at the device rate
    constexpr int64_t updateTime = 10000000LL * 882 / 44100;
    constexpr int64_t periodTime = 100000;
    constexpr int64_t periods = std::max<int64_t> ((updateTime + periodTime / 2) / periodTime, 1);

    this->m_updateFrames = static_cast<int> ((periodTime * periods * this->m_audioSpec.freq + 5000000) / 10000000);

    this->m_initialized = true;
    this->updateHrtf ();

    SDL_PauseAudioDevice (this->m_deviceID, 0);
}

SDLAudioDriver::~SDLAudioDriver () {
    if (this->m_initialized) {
	if (this->m_deviceID != 0) {
	    SDL_CloseAudioDevice (this->m_deviceID);
	}

	for (const auto* buffer : this->m_streams | std::views::values) {
	    delete buffer;
	}

	this->m_streams.clear ();

	SDL_QuitSubSystem (SDL_INIT_AUDIO);
    }

    // made before the audio system starts, so it is there on every path out of the constructor
    SDL_DestroyMutex (this->m_streamListMutex);
}

int SDLAudioDriver::addStream (AudioStream* stream, int volume, float left, float right) {
    const int newStreamId = this->m_lastStreamID;
    this->m_lastStreamID++;

    auto* buffer = new SDLAudioBuffer { stream };

    buffer->volume = volume;
    buffer->gainLeft = left;
    buffer->gainRight = right;

    SDL_LockMutex (this->m_streamListMutex);

    this->m_streams.insert_or_assign (newStreamId, buffer);

    SDL_UnlockMutex (this->m_streamListMutex);

    return newStreamId;
}
void SDLAudioDriver::removeStream (int streamId) {
    // same lock the audio callback holds while iterating m_streams, else erasing races it and the stream may be deleted
    // in use
    SDL_LockMutex (this->m_streamListMutex);

    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end ()) {
	delete it->second;
	this->m_streams.erase (it);
    }

    SDL_UnlockMutex (this->m_streamListMutex);
}

void SDLAudioDriver::setStreamVolume (int streamId, int volume) {
    // no lock: called every frame per sound and the callback holds it while mixing; only this thread modifies the map,
    // the volume is atomic
    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end ()) {
	it->second->volume.store (volume, std::memory_order_relaxed);
    }
}

void SDLAudioDriver::setStreamGains (int streamId, float left, float right) {
    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end ()) {
	it->second->gainLeft.store (left, std::memory_order_relaxed);
	it->second->gainRight.store (right, std::memory_order_relaxed);
    }
}

void SDLAudioDriver::setStreamPaused (int streamId, bool paused) {
    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end ()) {
	it->second->paused.store (paused, std::memory_order_relaxed);
    }
}

void SDLAudioDriver::setStreamHrtf (int streamId, const StreamHrtf* hrtf) {
    // same reasoning as setStreamVolume for the lookup, the filters themselves are copied under the stream's lock
    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end ()) {
	std::lock_guard lock (it->second->hrtfMutex);

	it->second->hrtfSet = hrtf != nullptr;

	if (hrtf != nullptr) {
	    it->second->hrtf = *hrtf;
	}
    }
}

const Hrtf::Store* SDLAudioDriver::getHrtf () const {
    return this->m_hrtfEnabled.load (std::memory_order_acquire) ? this->m_hrtfStore.get () : nullptr;
}

Hrtf::Mixer* SDLAudioDriver::getHrtfMixer () const {
    return this->m_hrtfEnabled.load (std::memory_order_acquire) ? this->m_hrtfMixer.get () : nullptr;
}

unsigned SDLAudioDriver::getHrtfGeneration () const { return this->m_hrtfGeneration.load (std::memory_order_acquire); }

void SDLAudioDriver::update (float dt) {
    AudioDriver::update (dt);
    this->releaseFinishedStreams ();
    this->updateHrtf ();
}

void SDLAudioDriver::updateHrtf () {
    if (!this->m_initialized || this->m_hrtfFailed) {
	return;
    }

    const auto mode = this->getApplicationContext ().settings.audio.hrtf;

    if (mode == Application::ApplicationContext::HRTF_AUTO && this->m_headphonesDetector == nullptr
	&& this->getApplicationContext ().settings.audio.enabled) {
	this->m_headphonesDetector = std::make_unique<Detectors::HeadphonesDetector> ();
    }

    const bool headphones = this->m_headphonesDetector != nullptr && this->m_headphonesDetector->headphones ();
    // OpenAL Soft only renders HRTF on stereo output
    const bool wanted = this->m_audioSpec.channels == 2
	&& (mode == Application::ApplicationContext::HRTF_ON
	    || (mode == Application::ApplicationContext::HRTF_AUTO && headphones));

    if (wanted == this->m_hrtfEnabled.load (std::memory_order_relaxed)) {
	return;
    }

    if (wanted && this->m_hrtfStore == nullptr) {
	this->m_hrtfStore = Hrtf::loadDefault (static_cast<unsigned> (this->m_audioSpec.freq));

	if (this->m_hrtfStore == nullptr) {
	    this->m_hrtfFailed = true;
	    return;
	}

	this->m_hrtfMixer = std::make_unique<Hrtf::Mixer> (this->m_hrtfStore->irSize);
    }

    if (wanted) {
	this->m_hrtfGeneration.fetch_add (1, std::memory_order_release);
    }

    this->m_hrtfEnabled.store (wanted, std::memory_order_release);

    sLog.out (
	"Sound objects: HRTF ", wanted ? "on" : "off",
	mode == Application::ApplicationContext::HRTF_AUTO ? (headphones ? " (headphones)" : " (speakers)")
							   : " (forced)"
    );
}

void SDLAudioDriver::stopStream (int streamId, std::unique_ptr<AudioStream> stream) {
    SDL_LockMutex (this->m_streamListMutex);

    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end () && this->m_initialized) {
	it->second->owned = std::move (stream);
	it->second->stopping.store (true, std::memory_order_release);
    }

    SDL_UnlockMutex (this->m_streamListMutex);

    if (stream != nullptr) {
	this->removeStream (streamId);
    }
}

void SDLAudioDriver::releaseFinishedStreams () {
    std::vector<SDLAudioBuffer*> finished;

    // only mixUpdate marks a stopping voice finished; while automute holds the callback back or the device is gone
    // nothing mixes, and there is no fade to hear anyway
    const bool mixing = SDL_GetAudioDeviceStatus (this->m_deviceID) == SDL_AUDIO_PLAYING
	&& !this->getAudioDetector ().anythingPlaying ();

    SDL_LockMutex (this->m_streamListMutex);

    for (auto it = this->m_streams.begin (); it != this->m_streams.end ();) {
	const bool done = it->second->finished.load (std::memory_order_acquire)
	    || (!mixing && it->second->stopping.load (std::memory_order_acquire));

	if (done && it->second->owned != nullptr) {
	    finished.push_back (it->second);
	    it = this->m_streams.erase (it);
	} else {
	    ++it;
	}
    }

    SDL_UnlockMutex (this->m_streamListMutex);

    // outside the lock, an AudioStream waits for its reader thread
    for (const auto* buffer : finished) {
	delete buffer;
    }
}

AVSampleFormat SDLAudioDriver::getFormat () const { return AV_SAMPLE_FMT_FLT; }

int SDLAudioDriver::getSampleRate () const { return this->m_audioSpec.freq; }

int SDLAudioDriver::getChannels () const { return this->m_audioSpec.channels; }
