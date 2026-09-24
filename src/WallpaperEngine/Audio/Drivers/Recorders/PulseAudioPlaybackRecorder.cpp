#include "PulseAudioPlaybackRecorder.h"
#include "WallpaperEngine/Logging/Log.h"
#include <pulse/rtclock.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace WallpaperEngine::Audio::Drivers::Recorders {
namespace {
constexpr int CAPTURE_RATE = 44100;
constexpr int CAPTURE_CHANNELS = 2;
constexpr auto CAPTURE_TIMEOUT = std::chrono::milliseconds (1000);

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

void pa_server_info_cb (pa_context* ctx, const pa_server_info* info, void* userdata);

void pa_retry_capture_cb (pa_mainloop_api* api, pa_time_event* event, const struct timeval* /*tv*/, void* userdata) {
    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);

    api->time_free (event);

    if (recorder->captureStream == nullptr) {
	return;
    }

    pa_context* ctx = pa_stream_get_context (recorder->captureStream);

    if (pa_context_get_state (ctx) != PA_CONTEXT_READY) {
	return;
    }

    if (pa_operation* o = pa_context_get_server_info (ctx, &pa_server_info_cb, userdata)) {
	pa_operation_unref (o);
    }
}

void pa_stream_notify_cb (pa_stream* stream, void* userdata) {
    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);

    switch (pa_stream_get_state (stream)) {
	case PA_STREAM_FAILED:
	case PA_STREAM_TERMINATED:
	    if (stream != recorder->captureStream) {
		break;
	    }

	    // the server can drop the monitor stream while sinks change state (a hotswap starting or stopping
	    // sounds) and nothing else may follow to re-take it, so retry on our own
	    if (!recorder->captureLost) {
		recorder->captureLost = true;
		sLog.error ("Audio capture stream lost, retrying every second");
	    }

	    pa_context_rttime_new (
		pa_stream_get_context (stream), pa_rtclock_now () + PA_USEC_PER_SEC, &pa_retry_capture_cb, userdata
	    );
	    break;
	case PA_STREAM_READY:
	    if (recorder->captureLost) {
		recorder->captureLost = false;
		sLog.out ("Audio capture stream restored");
	    }

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
    const void* data = nullptr;
    size_t currentSize;
    if (pa_stream_peek (stream, &data, &currentSize) != 0) {
	sLog.error ("Failed to peek at stream data...");
	return;
    }

    if (data == nullptr && currentSize == 0) {
	// No data in the buffer, ignore.
	return;
    }

    if (data == nullptr && currentSize > 0) {
	// Hole in the buffer. We must drop it.
	recorder->owner->dropBlock ();

	if (pa_stream_drop (stream) != 0) {
	    sLog.error ("Failed to drop a hole while capturing!");
	    return;
	}

	return;
    }

    if (currentSize > 0 && data) {
	recorder->owner->consumeSamples (
	    reinterpret_cast<const float*> (data), currentSize / (sizeof (float) * CAPTURE_CHANNELS)
	);
    }

    if (pa_stream_drop (stream) != 0) {
	sLog.error ("Failed to drop data after peeking");
    }
}

void pa_server_info_cb (pa_context* ctx, const pa_server_info* info, void* userdata) {
    if (info == nullptr || info->default_sink_name == nullptr) {
	return;
    }

    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);
    const std::string monitor_name = std::string (info->default_sink_name) + ".monitor";

    if (recorder->captureStream) {
	// sink/source events also fire for volume and state changes (another wallpaper starting its sounds),
	// only re-take the stream when the default sink moved or the old one died
	const bool alive = PA_STREAM_IS_GOOD (pa_stream_get_state (recorder->captureStream));

	if (alive && monitor_name == recorder->monitorName) {
	    return;
	}

	// the context keeps a connected stream alive, unref alone left it feeding the same buffer
	pa_stream_set_state_callback (recorder->captureStream, nullptr, nullptr);
	pa_stream_set_read_callback (recorder->captureStream, nullptr, nullptr);
	pa_stream_disconnect (recorder->captureStream);
	pa_stream_unref (recorder->captureStream);
	recorder->captureStream = nullptr;
    }

    recorder->monitorName = monitor_name;

    pa_sample_spec spec;
    // WE's loopback capture is float stereo, 44.1kHz gives exactly its FFT size (it scales with the rate)
    spec.format = PA_SAMPLE_FLOAT32NE;
    spec.rate = CAPTURE_RATE;
    spec.channels = CAPTURE_CHANNELS;

    recorder->captureStream = pa_stream_new (ctx, "output monitor", &spec, nullptr);

    pa_stream_set_state_callback (recorder->captureStream, &pa_stream_notify_cb, userdata);
    pa_stream_set_read_callback (recorder->captureStream, &pa_stream_read_cb, userdata);

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
		    ctx,
		    static_cast<pa_subscription_mask_t> (
			PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SERVER
		    ),
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
    m_captureData ({ .owner = this, .captureStream = nullptr, .captureLost = false }),
    m_analyzer (CAPTURE_RATE),
    m_webFFT (kiss_fftr_alloc (WAVE_BUFFER_SIZE, 0, nullptr, nullptr)) {
    this->m_dataMutex = SDL_CreateMutex ();
    this->m_mainloop = pa_mainloop_new ();
    this->m_mainloopApi = pa_mainloop_get_api (this->m_mainloop);
    this->m_context = pa_context_new (this->m_mainloopApi, "wallpaperengine-audioprocessing");

    pa_context_set_state_callback (this->m_context, &pa_context_notify_cb, &this->m_captureData);

    if (pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
	sLog.error ("PulseAudio connection failed! Audio processing is disabled");
	return;
    }

    // wait until the context is ready, a server that goes away mid-handshake would otherwise spin here forever
    while (pa_context_get_state (this->m_context) != PA_CONTEXT_READY) {
	if (!PA_CONTEXT_IS_GOOD (pa_context_get_state (this->m_context))) {
	    sLog.error ("PulseAudio connection failed! Audio processing is disabled");
	    return;
	}

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
	// unblocks the pa_mainloop_poll() the capture thread may be waiting in
	pa_mainloop_wakeup (this->m_mainloop);
    }
    if (this->m_captureThread) {
	SDL_WaitThread (this->m_captureThread, nullptr);
    }

    if (m_captureData.captureStream) {
	pa_stream_set_state_callback (m_captureData.captureStream, nullptr, nullptr);
	pa_stream_disconnect (m_captureData.captureStream);
	pa_stream_unref (m_captureData.captureStream);
    }

    free (this->m_webFFT);

    pa_context_disconnect (this->m_context);
    pa_context_unref (this->m_context);
    pa_mainloop_free (this->m_mainloop);

    if (this->m_dataMutex) {
	SDL_DestroyMutex (this->m_dataMutex);
    }
}

void PulseAudioPlaybackRecorder::lock () const { SDL_LockMutex (this->m_dataMutex); }
void PulseAudioPlaybackRecorder::unlock () const { SDL_UnlockMutex (this->m_dataMutex); }

int PulseAudioPlaybackRecorder::captureThreadEntry (void* userdata) {
    static_cast<PulseAudioPlaybackRecorder*> (userdata)->captureLoop ();
    return 0;
}

void PulseAudioPlaybackRecorder::captureLoop () {
    bool cleared = false;

    while (this->m_running.load (std::memory_order_relaxed)) {
	// time out so a stream that stopped delivering (suspended sink) is noticed
	if (pa_mainloop_prepare (this->m_mainloop, 100 * 1000) < 0 || pa_mainloop_poll (this->m_mainloop) < 0
	    || pa_mainloop_dispatch (this->m_mainloop) < 0) {
	    break;
	}

	const bool stale = std::chrono::steady_clock::now () - this->m_lastSamples > CAPTURE_TIMEOUT;

	if (stale && !cleared) {
	    this->m_analyzer.reset ();
	    this->clearCaptured ();
	}

	cleared = stale;
    }
}

void PulseAudioPlaybackRecorder::clearCaptured () {
    this->lock ();
    std::fill_n (this->m_captured, 128, 0.0f);
    this->unlock ();
}

void PulseAudioPlaybackRecorder::dropBlock () { this->m_analyzer.reset (); }

void PulseAudioPlaybackRecorder::consumeSamples (const float* samples, std::size_t frames) {
    this->m_lastSamples = std::chrono::steady_clock::now ();

    float bands[SpectrumAnalyzer::BANDS * 2];

    if (this->m_analyzer.feed (samples, frames, CAPTURE_CHANNELS, bands)) {
	this->lock ();
	std::copy_n (bands, 128, this->m_captured);
	this->unlock ();
    }

    for (std::size_t i = 0; i < frames; i++) {
	this->m_webSamples[this->m_webSampleCount++]
	    = (samples[i * CAPTURE_CHANNELS] + samples[i * CAPTURE_CHANNELS + 1]) * 0.5f;

	if (this->m_webSampleCount == WAVE_BUFFER_SIZE) {
	    this->m_webSampleCount = 0;
	    this->processWebFrame ();
	}
    }
}

void PulseAudioPlaybackRecorder::processWebFrame () {
    kiss_fftr (this->m_webFFT, this->m_webSamples, this->m_FFTinfo);

    float bands64[64];

    for (int band = 0; band < 64; band++) {
	const int index = band * 2;
	const float power = this->m_FFTinfo[index].r * this->m_FFTinfo[index].r
	    + this->m_FFTinfo[index].i * this->m_FFTinfo[index].i;
	float level = 0.0f;

	if (power > 0.0f) {
	    level = 0.35f * log10 (power) + 1.0f;
	}

	bands64[band] = fmax (0.0f, level * static_cast<float> (2.0f - pow (M_E, (1.0f - band / 63.0f) * 1.0f - 0.5f)));
    }

    const auto now = std::chrono::steady_clock::now ();
    const float dt = std::chrono::duration<float> (now - this->m_lastWebFrame).count ();
    this->m_lastWebFrame = now;

    this->m_normalizer.update (bands64, 64, dt);

    for (float& band : bands64) {
	band = this->m_normalizer.apply (band);
    }

    this->notifySpectrumListeners (bands64);
}

} // namespace WallpaperEngine::Audio::Drivers::Recorders