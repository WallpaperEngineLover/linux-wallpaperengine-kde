#include "PulseProcessVolume.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace WallpaperEngine::Audio::Drivers;

namespace {
constexpr auto RETRY_DELAY = std::chrono::seconds (2);

// ppid follows the state after the command name, which can contain spaces and brackets
pid_t parentOf (pid_t pid) {
    std::ifstream stat ("/proc/" + std::to_string (pid) + "/stat");
    std::string line;

    if (!std::getline (stat, line)) {
	return 0;
    }

    const auto end = line.rfind (')');

    if (end == std::string::npos) {
	return 0;
    }

    char state = 0;
    long parent = 0;
    std::istringstream fields (line.substr (end + 1));

    return fields >> state >> parent ? static_cast<pid_t> (parent) : 0;
}
} // namespace

PulseProcessVolume::PulseProcessVolume (pid_t root) : m_root (root) {
    this->m_loop = pa_mainloop_new ();
    this->connect ();
}

PulseProcessVolume::~PulseProcessVolume () {
    this->disconnect ();

    if (this->m_loop) {
	pa_mainloop_free (this->m_loop);
    }
}

void PulseProcessVolume::connect () {
    this->m_context = pa_context_new (pa_mainloop_get_api (this->m_loop), "wallpaperengine-web-volume");

    pa_context_set_state_callback (this->m_context, onState, this);
    pa_context_set_subscribe_callback (this->m_context, onEvent, this);

    pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);
}

void PulseProcessVolume::disconnect () {
    if (this->m_context == nullptr) {
	return;
    }

    pa_context_set_state_callback (this->m_context, nullptr, nullptr);
    pa_context_set_subscribe_callback (this->m_context, nullptr, nullptr);
    pa_context_disconnect (this->m_context);
    pa_context_unref (this->m_context);

    this->m_context = nullptr;
    this->m_known.clear ();
}

void PulseProcessVolume::setVolume (float factor) {
    if (factor == this->m_factor) {
	return;
    }

    this->m_factor = factor;

    if (this->m_context == nullptr || pa_context_get_state (this->m_context) != PA_CONTEXT_READY) {
	// onState() applies it once connected
	return;
    }

    pa_operation* operation = pa_context_get_sink_input_info_list (this->m_context, onStream, this);

    if (operation != nullptr) {
	pa_operation_unref (operation);
    }
}

void PulseProcessVolume::update () {
    for (int i = 0; i < 32 && pa_mainloop_iterate (this->m_loop, 0, nullptr) > 0; i++) { }

    if (this->m_context == nullptr) {
	return;
    }

    const auto state = pa_context_get_state (this->m_context);
    const auto now = std::chrono::steady_clock::now ();

    if ((state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) && now >= this->m_retryAt) {
	this->disconnect ();
	this->connect ();
	this->m_retryAt = now + RETRY_DELAY;
    }
}

bool PulseProcessVolume::descendsFromRoot (pid_t pid) const {
    for (int depth = 0; depth < 64 && pid > 1; depth++) {
	if (pid == this->m_root) {
	    return true;
	}

	pid = parentOf (pid);
    }

    return false;
}

void PulseProcessVolume::adjust (const pa_sink_input_info& info) {
    auto known = this->m_known.find (info.index);

    if (known == this->m_known.end ()) {
	bool ours = false;

	if (info.proplist != nullptr) {
	    if (const char* value = pa_proplist_gets (info.proplist, PA_PROP_APPLICATION_PROCESS_ID)) {
		char* end = nullptr;
		const long pid = std::strtol (value, &end, 10);

		ours = *end == '\0' && pid > 0 && this->descendsFromRoot (static_cast<pid_t> (pid));
	    }
	}

	known = this->m_known.emplace (info.index, ours).first;
    }

    if (!known->second) {
	return;
    }

    pa_cvolume wanted;
    pa_cvolume_set (&wanted, info.volume.channels, pa_sw_volume_from_linear (this->m_factor));

    // our own change event comes back here
    if (pa_cvolume_equal (&wanted, &info.volume)) {
	return;
    }

    pa_operation* operation = pa_context_set_sink_input_volume (this->m_context, info.index, &wanted, nullptr, nullptr);

    if (operation != nullptr) {
	pa_operation_unref (operation);
    }
}

void PulseProcessVolume::onState (pa_context* context, void* userdata) {
    auto* self = static_cast<PulseProcessVolume*> (userdata);

    if (pa_context_get_state (context) != PA_CONTEXT_READY) {
	return;
    }

    pa_operation* subscribe = pa_context_subscribe (context, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr);
    pa_operation* list = pa_context_get_sink_input_info_list (context, onStream, self);

    if (subscribe != nullptr) {
	pa_operation_unref (subscribe);
    }

    if (list != nullptr) {
	pa_operation_unref (list);
    }
}

void PulseProcessVolume::onEvent (
    pa_context* context, pa_subscription_event_type_t type, uint32_t index, void* userdata
) {
    auto* self = static_cast<PulseProcessVolume*> (userdata);

    if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
	return;
    }

    if ((type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
	self->m_known.erase (index);
	return;
    }

    // new streams start corked, so this lands before the first sample
    pa_operation* operation = pa_context_get_sink_input_info (context, index, onStream, self);

    if (operation != nullptr) {
	pa_operation_unref (operation);
    }
}

void PulseProcessVolume::onStream (pa_context*, const pa_sink_input_info* info, int last, void* userdata) {
    if (last != 0 || info == nullptr) {
	return;
    }

    static_cast<PulseProcessVolume*> (userdata)->adjust (*info);
}
