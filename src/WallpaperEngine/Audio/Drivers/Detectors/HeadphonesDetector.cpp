#include "HeadphonesDetector.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cstring>

using namespace WallpaperEngine::Audio::Drivers::Detectors;

namespace {
constexpr pa_usec_t STARTUP_TIMEOUT_USEC = 2 * PA_USEC_PER_SEC;

bool isHeadphonesFormFactor (const char* value) {
    return value != nullptr && (std::strcmp (value, "headphone") == 0 || std::strcmp (value, "headset") == 0);
}

/** WASAPI's Headphones/Headset form factors on a stereo device, in PulseAudio terms */
bool isHeadphones (const pa_sink_info& info) {
    if (info.sample_spec.channels != 2) {
	return false;
    }

    // PulseAudio's name for it, and PipeWire's own spelling
    if (isHeadphonesFormFactor (pa_proplist_gets (info.proplist, PA_PROP_DEVICE_FORM_FACTOR))
	|| isHeadphonesFormFactor (pa_proplist_gets (info.proplist, "device.form-factor"))) {
	return true;
    }

    if (info.active_port == nullptr) {
	return false;
    }

#if PA_CHECK_VERSION(14, 0, 0)
    if (info.active_port->type == PA_DEVICE_PORT_TYPE_HEADPHONES
	|| info.active_port->type == PA_DEVICE_PORT_TYPE_HEADSET) {
	return true;
    }
#endif

    // what OpenAL Soft's own PulseAudio backend checks
    return std::strcmp (info.active_port->name, "analog-output-headphones") == 0;
}
} // namespace

HeadphonesDetector::HeadphonesDetector () {
    this->m_mainloop = pa_threaded_mainloop_new ();

    // the first connection doesn't wait for a server, the sounds start right after this
    this->connect (PA_CONTEXT_NOFLAGS);

    if (pa_threaded_mainloop_start (this->m_mainloop) < 0) {
	sLog.error ("Cannot start the sound server connection, headphones detection is disabled");
	return;
    }

    // the first sounds start right after this, so wait for the first answer, but not on a server that never gives one
    pa_threaded_mainloop_lock (this->m_mainloop);

    pa_mainloop_api* api = pa_threaded_mainloop_get_api (this->m_mainloop);
    timeval deadline {};

    pa_timeval_add (pa_gettimeofday (&deadline), STARTUP_TIMEOUT_USEC);
    pa_time_event* timeout = api->time_new (api, &deadline, &HeadphonesDetector::startupTimeout, this);

    while (!this->m_startupExpired) {
	const pa_context_state_t state = pa_context_get_state (this->m_context);

	if (!PA_CONTEXT_IS_GOOD (state) || (state == PA_CONTEXT_READY && this->m_pending == 0)) {
	    break;
	}

	pa_threaded_mainloop_wait (this->m_mainloop);
    }

    api->time_free (timeout);
    pa_threaded_mainloop_unlock (this->m_mainloop);

    if (this->m_startupExpired) {
	sLog.debug ("The sound server didn't answer in time, headphones detection goes on in the background");
    }
}

HeadphonesDetector::~HeadphonesDetector () {
    if (this->m_mainloop != nullptr) {
	pa_threaded_mainloop_stop (this->m_mainloop);
    }

    if (this->m_reconnect != nullptr) {
	pa_threaded_mainloop_get_api (this->m_mainloop)->time_free (this->m_reconnect);
    }

    if (this->m_context != nullptr) {
	pa_context_disconnect (this->m_context);
	pa_context_unref (this->m_context);
    }

    if (this->m_mainloop != nullptr) {
	pa_threaded_mainloop_free (this->m_mainloop);
    }
}

void HeadphonesDetector::connect (pa_context_flags_t flags) {
    if (this->m_context != nullptr) {
	pa_context_set_state_callback (this->m_context, nullptr, nullptr);
	pa_context_set_subscribe_callback (this->m_context, nullptr, nullptr);
	pa_context_disconnect (this->m_context);
	pa_context_unref (this->m_context);
    }

    this->m_pending = 0;
    this->m_wasReady = false;
    this->m_context = pa_context_new (pa_threaded_mainloop_get_api (this->m_mainloop), "wallpaperengine-output");

    pa_context_set_state_callback (this->m_context, &HeadphonesDetector::contextState, this);

    if (pa_context_connect (this->m_context, nullptr, flags, nullptr) < 0) {
	// the state callback has seen the failure already when the mainloop runs, before that it's the caller's
	sLog.debug ("Cannot connect to the sound server: ", pa_strerror (pa_context_errno (this->m_context)));
    }
}

void HeadphonesDetector::scheduleReconnect (pa_usec_t delay) {
    pa_mainloop_api* api = pa_threaded_mainloop_get_api (this->m_mainloop);
    timeval when {};

    pa_timeval_add (pa_gettimeofday (&when), delay);

    // the context can't be replaced from inside its own state callback
    if (this->m_reconnect != nullptr) {
	api->time_restart (this->m_reconnect, &when);
    } else {
	this->m_reconnect = api->time_new (api, &when, &HeadphonesDetector::reconnectTimer, this);
    }
}

void HeadphonesDetector::startupTimeout (pa_mainloop_api*, pa_time_event*, const timeval*, void* userdata) {
    auto* detector = static_cast<HeadphonesDetector*> (userdata);

    detector->m_startupExpired = true;
    pa_threaded_mainloop_signal (detector->m_mainloop, 0);
}

void HeadphonesDetector::reconnectTimer (pa_mainloop_api* api, pa_time_event* event, const timeval*, void* userdata) {
    auto* detector = static_cast<HeadphonesDetector*> (userdata);

    api->time_free (event);
    detector->m_reconnect = nullptr;
    detector->connect (static_cast<pa_context_flags_t> (PA_CONTEXT_NOFAIL));
}

void HeadphonesDetector::contextState (pa_context* context, void* userdata) {
    auto* detector = static_cast<HeadphonesDetector*> (userdata);

    switch (pa_context_get_state (context)) {
	case PA_CONTEXT_READY:
	    {
		pa_context_set_subscribe_callback (context, &HeadphonesDetector::subscription, detector);

		if (pa_operation* operation = pa_context_subscribe (
			context,
			static_cast<pa_subscription_mask_t> (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER),
			nullptr, nullptr
		    )) {
		    pa_operation_unref (operation);
		}

		detector->m_wasReady = true;
		detector->query ();
		break;
	    }
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
	    detector->m_headphones.store (false, std::memory_order_relaxed);
	    pa_threaded_mainloop_signal (detector->m_mainloop, 0);
	    // a server that went away: reconnect at once, NOFAIL then waits for it on D-Bus. A connection that never
	    // got anywhere had no way to wait (no D-Bus, PULSE_SERVER), try again later
	    detector->scheduleReconnect (detector->m_wasReady ? 0 : 2 * PA_USEC_PER_SEC);
	    break;
	default:
	    break;
    }
}

void HeadphonesDetector::subscription (pa_context*, pa_subscription_event_type_t, uint32_t, void* userdata) {
    // a port switch (headphones plugged in) is a sink change, a new default sink a server change
    static_cast<HeadphonesDetector*> (userdata)->query ();
}

void HeadphonesDetector::query () {
    if (pa_operation* operation = pa_context_get_server_info (this->m_context, &HeadphonesDetector::serverInfo, this)) {
	this->m_pending++;
	pa_operation_unref (operation);
    }
}

void HeadphonesDetector::serverInfo (pa_context* context, const pa_server_info* info, void* userdata) {
    auto* detector = static_cast<HeadphonesDetector*> (userdata);

    if (info != nullptr && info->default_sink_name != nullptr) {
	if (pa_operation* operation = pa_context_get_sink_info_by_name (
		context, info->default_sink_name, &HeadphonesDetector::sinkInfo, detector
	    )) {
	    detector->m_pending++;
	    pa_operation_unref (operation);
	}
    } else {
	detector->m_headphones.store (false, std::memory_order_relaxed);
    }

    detector->m_pending--;
    pa_threaded_mainloop_signal (detector->m_mainloop, 0);
}

void HeadphonesDetector::sinkInfo (pa_context*, const pa_sink_info* info, int eol, void* userdata) {
    auto* detector = static_cast<HeadphonesDetector*> (userdata);

    if (eol != 0) {
	detector->m_pending--;
	pa_threaded_mainloop_signal (detector->m_mainloop, 0);
	return;
    }

    if (info == nullptr) {
	return;
    }

    const bool headphones = isHeadphones (*info);
    const std::string report = std::string (info->name) + (headphones ? " (headphones)" : "");

    detector->m_headphones.store (headphones, std::memory_order_relaxed);

    if (report != detector->m_lastReport) {
	detector->m_lastReport = report;
	sLog.debug ("Default audio output: ", report);
    }
}
