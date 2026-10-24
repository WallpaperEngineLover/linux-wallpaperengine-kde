#include "PulseAudioPlayingDetector.h"
#include "WallpaperEngine/Logging/Log.h"

#include <unistd.h>

namespace WallpaperEngine::Audio::Drivers::Detectors {
void sinkInputInfoCallback (pa_context* context, const pa_sink_input_info* info, int eol, void* userdata) {
    auto* detector = static_cast<PulseAudioPlayingDetector*> (userdata);

    if (eol != 0 || info == nullptr) {
	detector->finishQuery ();
	return;
    }

    if (info->proplist == nullptr) {
	return;
    }

    const char* value = pa_proplist_gets (info->proplist, PA_PROP_APPLICATION_PROCESS_ID);

    if (value && strtol (value, nullptr, 10) != getpid () && !info->corked && !info->mute
	&& pa_cvolume_avg (&info->volume) != PA_VOLUME_MUTED) {
	detector->markPlaying ();
    }
}

static void subscriptionCallback (pa_context*, pa_subscription_event_type_t, uint32_t, void* userdata) {
    static_cast<PulseAudioPlayingDetector*> (userdata)->markDirty ();
}

PulseAudioPlayingDetector::PulseAudioPlayingDetector (
    Application::ApplicationContext& appContext,
    const Render::Drivers::Detectors::FullScreenDetector& fullscreenDetector
) : AudioPlayingDetector (appContext, fullscreenDetector) {
    this->m_mainloop = pa_mainloop_new ();
    this->m_mainloopApi = pa_mainloop_get_api (this->m_mainloop);
    this->m_context = pa_context_new (this->m_mainloopApi, "wallpaperengine");

    pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

    pa_context_state_t state;

    while ((state = pa_context_get_state (this->m_context)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD (state)) {
	pa_mainloop_iterate (this->m_mainloop, 1, nullptr);
    }

    if (state != PA_CONTEXT_READY) {
	sLog.error ("Automute: cannot reach the sound server, other apps' sound will not be detected");
	return;
    }

    pa_context_set_subscribe_callback (this->m_context, subscriptionCallback, this);

    if (pa_operation* op = pa_context_subscribe (this->m_context, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr)) {
	pa_operation_unref (op);
    }
}

PulseAudioPlayingDetector::~PulseAudioPlayingDetector () {
    if (this->m_context) {
	pa_context_disconnect (this->m_context);
	pa_context_unref (this->m_context);
    }

    if (this->m_mainloop) {
	pa_mainloop_free (this->m_mainloop);
    }
}

void PulseAudioPlayingDetector::update () {
    if (!this->getApplicationContext ().settings.audio.automute) {
	this->m_dirty = true;
	return this->setIsPlaying (false);
    }
    if (this->getFullscreenDetector ().anythingFullscreen ()) {
	this->m_dirty = true;
	return this->setIsPlaying (true);
    }

    if (pa_context_get_state (this->m_context) != PA_CONTEXT_READY) {
	return;
    }

    pa_mainloop_iterate (this->m_mainloop, 0, nullptr);

    if (!this->m_dirty || this->m_querying) {
	return;
    }

    this->m_found = false;
    this->m_dirty = false;

    pa_operation* op = pa_context_get_sink_input_info_list (this->m_context, sinkInputInfoCallback, this);
    if (op == nullptr) {
	this->m_dirty = true;
	return;
    }

    pa_operation_unref (op);
    this->m_querying = true;
}

void PulseAudioPlayingDetector::markPlaying () { this->m_found = true; }

void PulseAudioPlayingDetector::markDirty () { this->m_dirty = true; }

void PulseAudioPlayingDetector::finishQuery () {
    if (!this->m_querying) {
	return;
    }

    this->m_querying = false;
    this->setIsPlaying (this->m_found);
}
}
