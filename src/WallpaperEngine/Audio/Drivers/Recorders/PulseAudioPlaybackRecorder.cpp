#include "PulseAudioPlaybackRecorder.h"
#include "WallpaperEngine/Data/Utils/FloatCompare.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <pulse/rtclock.h>
#include <sstream>

namespace WallpaperEngine::Audio::Drivers::Recorders {
namespace {
    constexpr int CAPTURE_RATE = 44100;
    constexpr int CAPTURE_CHANNELS = 2;
    constexpr auto CAPTURE_TIMEOUT = std::chrono::milliseconds (1000);
    constexpr auto SILENCE_HOLD = std::chrono::milliseconds (50);

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
}

void pa_server_info_cb (pa_context* ctx, const pa_server_info* info, void* userdata);

static void pa_retry_capture_cb (pa_mainloop_api* api, pa_time_event* event, const struct timeval*, void* userdata) {
    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);

    api->time_free (event);

    if (recorder->captureStream == nullptr) {
	return;
    }

    // a failed stream has let go of its context by now
    pa_context* ctx = recorder->context;

    if (ctx == nullptr || pa_context_get_state (ctx) != PA_CONTEXT_READY) {
	return;
    }

    if (pa_operation* o = pa_context_get_server_info (ctx, &pa_server_info_cb, userdata)) {
	pa_operation_unref (o);
    }
}

static void pa_stream_notify_cb (pa_stream* stream, void* userdata) {
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

	    if (recorder->context != nullptr) {
		pa_context_rttime_new (
		    recorder->context, pa_rtclock_now () + PA_USEC_PER_SEC, &pa_retry_capture_cb, userdata
		);
	    }
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

static void pa_stream_read_cb (pa_stream* stream, const size_t, void* userdata) {
    auto* recorder = static_cast<PulseAudioPlaybackRecorder::PulseAudioData*> (userdata);

    const void* data = nullptr;
    size_t currentSize;
    if (pa_stream_peek (stream, &data, &currentSize) != 0) {
	sLog.error ("Failed to peek at stream data...");
	return;
    }

    if (data == nullptr && currentSize == 0) {
	return;
    }

    if (data == nullptr && currentSize > 0) {
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

    recorder->context = ctx;

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

    size_t bytesPerSec = pa_bytes_per_second (&spec);
    attr.fragsize = bytesPerSec * 10 / 1000;
    attr.maxlength = attr.fragsize + bytesPerSec * 750 / 1000;

    sLog.debug ("Audio processing: capturing from monitor source '", monitor_name, "' (default sink)");

    if (pa_stream_connect_record (recorder->captureStream, monitor_name.c_str (), &attr, PA_STREAM_ADJUST_LATENCY)
	!= 0) {
	sLog.error ("Failed to connect to input for recording");
    }
}

static void pa_context_subscribe_cb (pa_context* ctx, pa_subscription_event_type_t t, uint32_t idx, void* userdata) {
    pa_operation* o = pa_context_get_server_info (ctx, &pa_server_info_cb, userdata);
    if (o) {
	pa_operation_unref (o);
    }
}

static void pa_context_notify_cb (pa_context* ctx, void* userdata) {
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
    m_captureData ({ .owner = this, .captureStream = nullptr, .captureLost = false }), m_analyzer (CAPTURE_RATE) {
    this->m_stats.enabled = std::getenv ("LWE_AUDIO_STATS") != nullptr;
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

    // capture on its own thread: pumped from the render loop a slow frame stalled it and the server force-dropped the
    // backlog
    this->m_captureThread = std::thread ([this] {
	WallpaperEngine::Logging::guardThread ("Audio capture", [this] { this->captureLoop (); });
    });
}

PulseAudioPlaybackRecorder::~PulseAudioPlaybackRecorder () {
    this->m_running.store (false, std::memory_order_relaxed);
    if (this->m_mainloop) {
	pa_mainloop_wakeup (this->m_mainloop);
    }
    if (this->m_captureThread.joinable ()) {
	this->m_captureThread.join ();
    }

    if (m_captureData.captureStream) {
	pa_stream_set_state_callback (m_captureData.captureStream, nullptr, nullptr);
	pa_stream_disconnect (m_captureData.captureStream);
	pa_stream_unref (m_captureData.captureStream);
    }

    pa_context_disconnect (this->m_context);
    pa_context_unref (this->m_context);
    pa_mainloop_free (this->m_mainloop);

    if (this->m_dataMutex) {
	SDL_DestroyMutex (this->m_dataMutex);
    }
}

void PulseAudioPlaybackRecorder::lock () const { SDL_LockMutex (this->m_dataMutex); }
void PulseAudioPlaybackRecorder::unlock () const { SDL_UnlockMutex (this->m_dataMutex); }

void PulseAudioPlaybackRecorder::captureLoop () {
    bool cleared = false;

    while (this->m_running.load (std::memory_order_relaxed)) {
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
	this->logStats ();
    }
}

void PulseAudioPlaybackRecorder::logStats () {
    auto& stats = this->m_stats;
    const auto now = std::chrono::steady_clock::now ();

    if (!stats.enabled || now - stats.since < std::chrono::seconds (1)) {
	return;
    }

    sLog.out (
	"Audio capture: ", stats.packets, " packets, ", stats.holes, " holes, ", stats.blocks, " blocks, peak ",
	stats.peak, ", dc ", stats.count > 0.0 ? stats.sum / stats.count : 0.0, ", bands low ", stats.lowMax, " high ",
	stats.highMax, ", stream ", this->m_captureData.captureLost ? "lost" : "up"
    );

    stats = { .enabled = true, .since = now };
}

void PulseAudioPlaybackRecorder::clearCaptured () {
    constexpr float silence[128] = {};

    this->lock ();
    std::fill_n (this->m_captured, 128, 0.0f);
    this->unlock ();
    this->notifySpectrumListeners (silence);
}

void PulseAudioPlaybackRecorder::dropBlock () {
    this->m_stats.holes++;
    this->m_analyzer.reset ();
}

void PulseAudioPlaybackRecorder::consumeSamples (const float* samples, std::size_t frames) {
    if (this->m_stats.enabled) {
	this->m_stats.packets++;

	for (std::size_t i = 0; i < frames * CAPTURE_CHANNELS; i++) {
	    this->m_stats.sum += samples[i];
	    this->m_stats.peak = std::max (this->m_stats.peak, std::fabs (samples[i]));
	}

	this->m_stats.count += static_cast<double> (frames * CAPTURE_CHANNELS);
    }

    // WASAPI's loopback sends nothing while nothing plays and WE zeroes the spectrum after a second without data, while
    // PipeWire and PulseAudio keep sending zeros: their FFT leaves a 1e-7 residue that pages with their own gain
    // (CWAV's monstercat) stretch to full height once the peak they follow has decayed
    if (std::all_of (samples, samples + frames * CAPTURE_CHANNELS, [] (float sample) {
	    return Data::Utils::isZero (sample);
	})) {
	const auto now = std::chrono::steady_clock::now ();

	if (this->m_silentSince == std::chrono::steady_clock::time_point {}) {
	    this->m_silentSince = now;
	}

	this->m_analyzer.reset ();

	if (!this->m_silenceCleared && now - this->m_silentSince >= SILENCE_HOLD) {
	    this->m_silenceCleared = true;
	    this->clearCaptured ();
	}

	return;
    }

    this->m_silentSince = {};
    this->m_silenceCleared = false;
    this->m_lastSamples = std::chrono::steady_clock::now ();

    float bands[SpectrumAnalyzer::BANDS * 2];

    if (this->m_analyzer.feed (samples, frames, CAPTURE_CHANNELS, bands)) {
	if (this->m_stats.enabled) {
	    this->m_stats.blocks++;
	    this->m_stats.lowMax = std::max (this->m_stats.lowMax, *std::max_element (bands, bands + 16));
	    this->m_stats.highMax = std::max (this->m_stats.highMax, *std::max_element (bands + 16, bands + 64));
	}

	this->lock ();
	std::copy_n (bands, 128, this->m_captured);
	this->unlock ();
	this->notifySpectrumListeners (bands);
    }
}

}