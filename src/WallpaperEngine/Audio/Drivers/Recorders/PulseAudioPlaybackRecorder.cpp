#include "PulseAudioPlaybackRecorder.h"
#include "WallpaperEngine/Logging/Log.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace WallpaperEngine::Audio::Drivers::Recorders {
namespace {
// Timestamp helper backing the debug-only capture markers below - useful for tracking down
// audio-to-visual delay regressions in the future.
std::string wallClockTimestamp () {
    const auto now = std::chrono::system_clock::now ();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (now.time_since_epoch ()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t (now);
    std::tm tmBuf {};
    localtime_r (&t, &tmBuf);

    std::ostringstream oss;
    oss << std::put_time (&tmBuf, "%H:%M:%S") << '.' << std::setfill ('0') << std::setw (3) << ms.count ();
    return oss.str ();
}
} // namespace

void pa_stream_notify_cb (pa_stream* stream, void* /*userdata*/) {
    switch (pa_stream_get_state (stream)) {
	case PA_STREAM_FAILED:
	    sLog.error ("Cannot open stream for capture. Audio processing is disabled");
	    break;
	case PA_STREAM_READY:
	    sLog.debug ("[", wallClockTimestamp (), "] Audio processing: capture stream ready");
	    break;
	default:
	    break;
    }
}

void pa_stream_read_cb (pa_stream* stream, const size_t /*nbytes*/, void* userdata) {
    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);

    // Careful when to pa_stream_peek() and pa_stream_drop()!
    // c.f. https://www.freedesktop.org/software/pulseaudio/doxygen/stream_8h.html#ac2838c449cde56e169224d7fe3d00824
    const uint8_t* data = nullptr;
    size_t currentSize;
    if (pa_stream_peek (stream, reinterpret_cast<const void**> (&data), &currentSize) != 0) {
	sLog.error ("Failed to peek at stream data...");
	return;
    }

    if (data == nullptr && currentSize == 0) {
	// No data in the buffer, ignore.
	return;
    }

    if (data == nullptr && currentSize > 0) {
	// Hole in the buffer. We must drop it.
	if (pa_stream_drop (stream) != 0) {
	    sLog.error ("Failed to drop a hole while capturing!");
	    return;
	}
    } else if (currentSize > 0 && data) {
	const size_t dataToCopy = std::min (currentSize, WAVE_BUFFER_SIZE - recorder->currentWritePointer);

	// depending on the amount of data available, we might want to read one or multiple frames
	const size_t end = recorder->currentWritePointer + dataToCopy;

	// this packet will fill the buffer, perform some extra checks for extra full buffers and get the latest one
	if (end == WAVE_BUFFER_SIZE) {
	    if (const size_t numberOfFullBuffers = (currentSize - dataToCopy) / WAVE_BUFFER_SIZE;
		numberOfFullBuffers > 0) {
		// calculate the start of the last block (we need the end of the previous block, hence the - 1)
		const size_t startOfLastBuffer = std::max (
		    dataToCopy + (numberOfFullBuffers - 1) * WAVE_BUFFER_SIZE, currentSize - WAVE_BUFFER_SIZE
		);
		memcpy (recorder->audioBuffer, &data[startOfLastBuffer], WAVE_BUFFER_SIZE * sizeof (uint8_t));
		recorder->currentWritePointer = currentSize - startOfLastBuffer - WAVE_BUFFER_SIZE;
		memcpy (
		    recorder->audioBufferTmp, &data[startOfLastBuffer + WAVE_BUFFER_SIZE],
		    recorder->currentWritePointer * sizeof (uint8_t)
		);
	    } else {
		// okay, no full extra packets available, copy the rest of the data and flip the buffers
		memcpy (&recorder->audioBufferTmp[recorder->currentWritePointer], data, dataToCopy * sizeof (uint8_t));
		uint8_t* tmp = recorder->audioBuffer;
		recorder->audioBuffer = recorder->audioBufferTmp;
		recorder->audioBufferTmp = tmp;
		recorder->currentWritePointer = 0;
	    }

	    recorder->fullFrameReady = true;
	} else {
	    memcpy (&recorder->audioBufferTmp[recorder->currentWritePointer], data, dataToCopy * sizeof (uint8_t));
	    recorder->currentWritePointer += dataToCopy;
	}
    }

    if (pa_stream_drop (stream) != 0) {
	sLog.error ("Failed to drop data after peeking");
    }
}

void pa_server_info_cb (pa_context* ctx, const pa_server_info* info, void* userdata) {
    if (info == nullptr) {
	return;
    }

    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_U8;
    spec.rate = 44100;
    spec.channels = 1;

    if (recorder->captureStream) {
	pa_stream_unref (recorder->captureStream);
    }

    recorder->captureStream = pa_stream_new (ctx, "output monitor", &spec, nullptr);

    pa_stream_set_state_callback (recorder->captureStream, &pa_stream_notify_cb, userdata);
    pa_stream_set_read_callback (recorder->captureStream, &pa_stream_read_cb, userdata);

    std::string monitor_name (info->default_sink_name);
    monitor_name += ".monitor";

    pa_buffer_attr attr {};

    // 10 = latency msecs, 750 = max msecs to store
    size_t bytesPerSec = pa_bytes_per_second (&spec);
    attr.fragsize = bytesPerSec * 10 / 1000;
    attr.maxlength = attr.fragsize + bytesPerSec * 750 / 1000;

    sLog.debug ("Audio processing: capturing from monitor source '", monitor_name, "' (default sink)");

    if (pa_stream_connect_record (recorder->captureStream, monitor_name.c_str (), &attr, PA_STREAM_ADJUST_LATENCY)
	!= 0) {
	sLog.error ("Failed to connect to input for recording");
    }
}

void pa_context_subscribe_cb (pa_context* ctx, pa_subscription_event_type_t t, uint32_t idx, void* userdata) {
    // sink changes mean re-take the stream
    pa_operation* o = pa_context_get_server_info (ctx, &pa_server_info_cb, userdata);
    if (o) {
	pa_operation_unref (o);
    }
}

void pa_context_notify_cb (pa_context* ctx, void* userdata) {
    switch (pa_context_get_state (ctx)) {
	case PA_CONTEXT_READY:
	    {
		pa_context_set_subscribe_callback (ctx, pa_context_subscribe_cb, userdata);
		pa_operation* o = pa_context_subscribe (
		    ctx, static_cast<pa_subscription_mask_t> (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE),
		    nullptr, nullptr
		);

		if (o) {
		    pa_operation_unref (o);
		}

		// context being ready means to fetch the sink too
		pa_operation* o2 = pa_context_get_server_info (ctx, &pa_server_info_cb, userdata);

		if (o2) {
		    pa_operation_unref (o2);
		}

		break;
	    }
	case PA_CONTEXT_FAILED:
	    sLog.error ("PulseAudio context initialization failed. Audio processing is disabled");
	    break;
	default:
	    break;
    }
}

PulseAudioPlaybackRecorder::PulseAudioPlaybackRecorder () :
    m_captureData (
	{ .kisscfg = kiss_fftr_alloc (WAVE_BUFFER_SIZE, 0, nullptr, nullptr),
	  .audioBuffer = new uint8_t[WAVE_BUFFER_SIZE],
	  .audioBufferTmp = new uint8_t[WAVE_BUFFER_SIZE] }
    ) {
    this->m_dataMutex = SDL_CreateMutex ();
    this->m_mainloop = pa_mainloop_new ();
    this->m_mainloopApi = pa_mainloop_get_api (this->m_mainloop);
    this->m_context = pa_context_new (this->m_mainloopApi, "wallpaperengine-audioprocessing");

    pa_context_set_state_callback (this->m_context, &pa_context_notify_cb, &this->m_captureData);

    if (pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
	sLog.error ("PulseAudio connection failed! Audio processing is disabled");
	return;
    }

    // wait until the context is ready
    while (pa_context_get_state (this->m_context) != PA_CONTEXT_READY) {
	pa_mainloop_iterate (this->m_mainloop, 1, nullptr);
    }

    // Capture used to be pumped from the render loop (pa_mainloop_iterate() once per frame via
    // update()), so a slow frame - a GPU/compositor stall, a heavy shader pass - stalled capture
    // along with it. PulseAudio/PipeWire then force-drops the backlog once its buffer overflows,
    // so the wallpaper "catches up" all at once instead of reacting smoothly. Capture now runs on
    // its own thread so it keeps draining regardless of what rendering is doing.
    this->m_captureThread
	= SDL_CreateThread (&PulseAudioPlaybackRecorder::captureThreadEntry, "lwe-audiocapture", this);
}

PulseAudioPlaybackRecorder::~PulseAudioPlaybackRecorder () {
    this->m_running.store (false, std::memory_order_relaxed);
    if (this->m_mainloop) {
	// unblocks a pa_mainloop_iterate() the capture thread may be blocked in
	pa_mainloop_wakeup (this->m_mainloop);
    }
    if (this->m_captureThread) {
	SDL_WaitThread (this->m_captureThread, nullptr);
    }

    if (m_captureData.captureStream) {
	pa_stream_unref (m_captureData.captureStream);
    }

    delete[] this->m_captureData.audioBufferTmp;
    delete[] this->m_captureData.audioBuffer;
    free (this->m_captureData.kisscfg);

    pa_context_disconnect (this->m_context);
    pa_context_unref (this->m_context);
    pa_mainloop_free (this->m_mainloop);

    if (this->m_dataMutex) {
	SDL_DestroyMutex (this->m_dataMutex);
    }
}

void PulseAudioPlaybackRecorder::update () {
    // capture now runs on its own thread (see the constructor and captureLoop()) - nothing to do
    // here anymore, kept as a no-op override since AudioDriver still calls this once per frame.
}

void PulseAudioPlaybackRecorder::lock () const { SDL_LockMutex (this->m_dataMutex); }
void PulseAudioPlaybackRecorder::unlock () const { SDL_UnlockMutex (this->m_dataMutex); }

int PulseAudioPlaybackRecorder::captureThreadEntry (void* userdata) {
    static_cast<PulseAudioPlaybackRecorder*> (userdata)->captureLoop ();
    return 0;
}

void PulseAudioPlaybackRecorder::captureLoop () {
    while (this->m_running.load (std::memory_order_relaxed)) {
	// blocks until there's data, a state change, or pa_mainloop_wakeup() from the destructor -
	// this thread has nothing else to do, so there's no reason to poll instead of blocking
	pa_mainloop_iterate (this->m_mainloop, 1, nullptr);

	if (!this->m_captureData.fullFrameReady) {
	    continue;
	}

	this->m_captureData.fullFrameReady = false;
	this->processFrame ();
    }
}

void PulseAudioPlaybackRecorder::processFrame () {
    // convert audio data to deltas so the fft library can properly handle it
    for (int i = 0; i < WAVE_BUFFER_SIZE; i++) {
	this->m_audioFFTbuffer[i] = (this->m_captureData.audioBuffer[i] - 128) / 128.0f;
    }

    kiss_fftr (this->m_captureData.kisscfg, this->m_audioFFTbuffer, this->m_FFTinfo);

    // computed into locals first so the lock only needs to be held for the final copy, not the
    // whole FFT pass
    float bands64[64];
    float bands32[32];
    float bands16[16];

    // one loop produces all 3 band resolutions
    for (int band = 0; band < 64; band++) {
	int index = band * 2;
	float f1 = this->m_FFTinfo[index].r;
	float f2 = this->m_FFTinfo[index].i;
	f2 = f1 * f1 + f2 * f2; // magnitude
	f1 = 0.0f;

	if (f2 > 0.0f) {
	    // log10(magnitude) is unbounded and usually negative at ordinary listening volumes, but
	    // scripts/shaders consuming this expect roughly a 0 (quiet) - 1 (loud) range; empirically
	    // chosen from real capture logs, may need retuning for very quiet/loud setups.
	    constexpr float kLoudnessOffset = 1.0f;
	    f1 = 0.35f * log10 (f2) + kLoudnessOffset;
	}

	// Written directly (no smoothing here) - the wallpaper's own script already smooths this
	// via its "smoothing" scriptproperty; an extra pass here would just double up on that.
	bands64[band] = fmax (0.0f, f1 * static_cast<float> (2.0f - pow (M_E, (1.0f - band / 63.0f) * 1.0f - 0.5f)));
	bands32[band >> 1] = fmax (0.0f, f1 * static_cast<float> (2.0f - pow (M_E, (1.0f - band / 31.0f) * 1.0f - 0.5f)));
	bands16[band >> 2] = fmax (0.0f, f1 * static_cast<float> (2.0f - pow (M_E, (1.0f - band / 15.0f) * 1.0f - 0.5f)));
    }

    // The levels above are log scaled with no upper bound, and clamping them to 1 made every band of anything but
    // quiet music sit at the top. Fit them to the loudest recent band instead, the same way for all resolutions
    // so they stay consistent with each other.
    const auto now = std::chrono::steady_clock::now ();
    const float dt = std::chrono::duration<float> (now - this->m_lastFrame).count ();
    this->m_lastFrame = now;

    this->m_normalizer.update (bands64, 64, dt);

    for (float& band : bands64) {
	band = this->m_normalizer.apply (band);
    }
    for (float& band : bands32) {
	band = this->m_normalizer.apply (band);
    }
    for (float& band : bands16) {
	band = this->m_normalizer.apply (band);
    }

    this->lock ();
    memcpy (this->audio64, bands64, sizeof (bands64));
    memcpy (this->audio32, bands32, sizeof (bands32));
    memcpy (this->audio16, bands16, sizeof (bands16));
    this->unlock ();

    this->notifySpectrumListeners (bands64);

    static int diagnosticCounter = 0;
    if (++diagnosticCounter >= 100) {
	diagnosticCounter = 0;
	sLog.debug ("Audio processing: audio16[0..3] = ", bands16[0], ", ", bands16[1], ", ", bands16[2], ", ", bands16[3]);
    }

    // Edge-triggered marker for a loud transient (e.g. a clap) reaching the capture layer,
    // timestamped to isolate whether a future audio-to-visual delay regression is in capture or
    // downstream of it.
    static bool wasLoud = false;
    float peak = 0.0f;
    for (float band : bands16) {
	peak = fmax (peak, band);
    }
    if (peak > 0.5f && !wasLoud) {
	sLog.debug ("[", wallClockTimestamp (), "] Audio processing: TRANSIENT detected, peak=", peak);
    }
    wasLoud = peak > 0.5f;
}

} // namespace WallpaperEngine::Audio::Drivers::Recorders