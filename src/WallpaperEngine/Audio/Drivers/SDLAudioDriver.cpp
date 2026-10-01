#include "SDLAudioDriver.h"
#include "WallpaperEngine/Logging/Log.h"

#define SDL_AUDIO_BUFFER_SIZE 4096
#define MAX_AUDIO_FRAME_SIZE 192000

using namespace WallpaperEngine::Audio;
using namespace WallpaperEngine::Audio::Drivers;

namespace {
template <typename T> void scaleSamples (uint8_t* data, int bytes, int channels, int firstChannel, float left, float right) {
    auto* samples = reinterpret_cast<T*> (data);
    const int count = bytes / static_cast<int> (sizeof (T));

    for (int i = 0; i < count; i++) {
	const int channel = (firstChannel + i) % channels;
	const float gain = channel == 0 ? left : channel == 1 ? right : (left + right) * 0.5f;

	samples[i] = static_cast<T> (static_cast<float> (samples[i]) * gain);
    }
}

/** Copies a chunk of decoded audio with each channel scaled, false when the device format isn't handled */
bool scaleChunk (
    std::vector<uint8_t>& out, const uint8_t* data, int bytes, const SDL_AudioSpec& spec, int byteOffset, float left,
    float right
) {
    const int sampleSize = SDL_AUDIO_BITSIZE (spec.format) / 8;
    const int firstChannel = (byteOffset / sampleSize) % spec.channels;

    out.assign (data, data + bytes);

    if (SDL_AUDIO_ISBIGENDIAN (spec.format) != (SDL_BYTEORDER == SDL_BIG_ENDIAN)) {
	return false;
    }

    if (SDL_AUDIO_ISFLOAT (spec.format) && sampleSize == 4) {
	scaleSamples<float> (out.data (), bytes, spec.channels, firstChannel, left, right);
    } else if (SDL_AUDIO_ISSIGNED (spec.format) && sampleSize == 2) {
	scaleSamples<int16_t> (out.data (), bytes, spec.channels, firstChannel, left, right);
    } else if (SDL_AUDIO_ISSIGNED (spec.format) && sampleSize == 4) {
	scaleSamples<int32_t> (out.data (), bytes, spec.channels, firstChannel, left, right);
    } else {
	return false;
    }

    return true;
}
} // namespace

void audio_callback (void* userdata, uint8_t* streamData, int length) {
    auto* driver = static_cast<SDLAudioDriver*> (userdata);

    memset (streamData, 0, length);

    // if audio is playing do not do anything here!
    if (driver->getAudioDetector ().anythingPlaying ()) {
	return;
    }

    SDL_LockMutex (driver->getStreamMutex ());

    for (const auto& buffer : driver->getStreams () | std::views::values) {
	uint8_t* streamDataPointer = streamData;
	int streamLength = length;

	if (!buffer->stream->isInitialized () || buffer->paused.load (std::memory_order_relaxed)) {
	    continue;
	}

	if (buffer->stream->isQueueEmpty ()) {
	    SDL_CondSignal (buffer->stream->getWaitCondition ());
	    continue;
	}

	while (streamLength > 0 && driver->getApplicationContext ().state.general.keepRunning) {
	    if (buffer->audio_buf_index >= buffer->audio_buf_size) {
		int audio_size = buffer->stream->decodeFrame (buffer->audio_buf, sizeof (buffer->audio_buf));

		if (audio_size < 0) {
		    // fallback for errors, silence
		    buffer->audio_buf_size = 1024;
		    memset (buffer->audio_buf, 0, buffer->audio_buf_size);
		} else {
		    buffer->audio_buf_size = audio_size;
		}

		buffer->audio_buf_index = 0;
	    }

	    int len1 = buffer->audio_buf_size - buffer->audio_buf_index;

	    if (len1 > streamLength) {
		len1 = streamLength;
	    }

	    // mix the audio, using this stream's own volume override if it has one
	    const int streamVolume = buffer->volume.load (std::memory_order_relaxed);
	    const float left = buffer->gainLeft.load (std::memory_order_relaxed);
	    const float right = buffer->gainRight.load (std::memory_order_relaxed);
	    const uint8_t* source = &buffer->audio_buf[buffer->audio_buf_index];
	    static std::vector<uint8_t> scaled;

	    if ((left != 1.0f || right != 1.0f)
		&& scaleChunk (
		    scaled, source, len1, driver->getSpec (), static_cast<int> (buffer->audio_buf_index), left, right
		)) {
		source = scaled.data ();
	    }

	    SDL_MixAudioFormat (
		streamDataPointer, source, driver->getSpec ().format, len1,
		streamVolume >= 0 ? streamVolume : driver->getApplicationContext ().state.audio.volume
	    );

	    streamLength -= len1;
	    streamDataPointer += len1;
	    buffer->audio_buf_index += len1;
	}
    }

    // TODO: do we also need to lock while audio is playing, or wait until the stream is unused?
    SDL_UnlockMutex (driver->getStreamMutex ());
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

    this->m_deviceID
	= SDL_OpenAudioDevice (nullptr, false, &requestedSpec, &this->m_audioSpec, SDL_AUDIO_ALLOW_ANY_CHANGE);

    if (this->m_deviceID == 0) {
	sLog.error ("SDL_OpenAudioDevice: ", SDL_GetError ());
	return;
    }

    SDL_PauseAudioDevice (this->m_deviceID, 0);

    this->m_initialized = true;
}

SDLAudioDriver::~SDLAudioDriver () {
    if (!this->m_initialized) {
	return;
    }

    if (this->m_deviceID != 0) {
	SDL_CloseAudioDevice (this->m_deviceID);
    }

    SDL_QuitSubSystem (SDL_INIT_AUDIO);
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
    // must hold the same lock the SDL audio callback thread holds while it iterates m_streams and
    // calls into each stream (decodeFrame, isQueueEmpty, ...) - without it, erasing here can race
    // the callback's map iteration (heap corruption) and the caller may go on to delete the
    // AudioStream while the callback thread is still using it
    SDL_LockMutex (this->m_streamListMutex);

    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end ()) {
	delete it->second;
	this->m_streams.erase (it);
    }

    SDL_UnlockMutex (this->m_streamListMutex);
}

void SDLAudioDriver::setStreamVolume (int streamId, int volume) {
    // no stream list lock here: every sound calls this each frame, and the audio callback holds that lock
    // while it waits on packets for all streams, which stalled the render thread for up to ~200ms at a time.
    // The map is only modified by addStream/removeStream on the render thread, the same
    // thread calling this, and the callback only reads it, so the lookup is safe and the volume is atomic
    if (const auto it = this->m_streams.find (streamId); it != this->m_streams.end ()) {
	it->second->volume.store (volume, std::memory_order_relaxed);
    }
}

void SDLAudioDriver::setStreamGains (int streamId, float left, float right) {
    // same reasoning as setStreamVolume, the gains are atomics read by the callback
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

const std::map<int, SDLAudioBuffer*>& SDLAudioDriver::getStreams () { return this->m_streams; }

AVSampleFormat SDLAudioDriver::getFormat () const {
    switch (this->m_audioSpec.format) {
	case AUDIO_U8:
	case AUDIO_S8:
	    return AV_SAMPLE_FMT_U8;
	case AUDIO_U16MSB:
	case AUDIO_U16LSB:
	case AUDIO_S16LSB:
	case AUDIO_S16MSB:
	    return AV_SAMPLE_FMT_S16;
	case AUDIO_S32LSB:
	case AUDIO_S32MSB:
	    return AV_SAMPLE_FMT_S32;
	case AUDIO_F32LSB:
	case AUDIO_F32MSB:
	    return AV_SAMPLE_FMT_FLT;
	default:
	    sLog.exception ("Cannot convert from SDL format to ffmpeg format, aborting...");
    }
}

int SDLAudioDriver::getSampleRate () const { return this->m_audioSpec.freq; }

int SDLAudioDriver::getChannels () const { return this->m_audioSpec.channels; }

const SDL_AudioSpec& SDLAudioDriver::getSpec () const { return this->m_audioSpec; }

SDL_mutex* SDLAudioDriver::getStreamMutex () const { return this->m_streamListMutex; }