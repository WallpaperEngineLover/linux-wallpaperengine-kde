#include "SessionMonitor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>

#include "WallpaperEngine/Desktop/HyprlandIPC.h"
#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"
#include "nlohmann/json.hpp"

#if defined(ENABLE_WAYLAND) && defined(ENABLE_KDE_FEATURES)
#define LWE_KWIN_DPMS 1
#include <vector>
#include <wayland-client.h>

#include "WallpaperEngine/Desktop/HostWayland.h"
#include "WallpaperEngine/Desktop/KDESession.h"
#include "kwin-dpms-protocol.h"
#endif

using namespace WallpaperEngine::Desktop;

namespace {
// signals can be missed when a screen locker restarts, so the state is asked again now and then
constexpr auto RECHECK_INTERVAL = std::chrono::seconds (30);
constexpr int DPMS_POLL_MS = 1000;

struct ScreenSaverName {
    const char* service;
    const char* path;
    const char* interface;
};

constexpr std::array SCREEN_SAVERS = {
    ScreenSaverName { "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver" },
    ScreenSaverName { "org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver" },
    ScreenSaverName { "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver" },
    ScreenSaverName { "org.cinnamon.ScreenSaver", "/org/cinnamon/ScreenSaver", "org.cinnamon.ScreenSaver" },
    ScreenSaverName { "org.mate.ScreenSaver", "/org/mate/ScreenSaver", "org.mate.ScreenSaver" },
};

// DPMS through dlopen, libXext isn't a build dependency
class X11Dpms {
public:
    ~X11Dpms () {
	if (this->m_display != nullptr) {
	    this->m_closeDisplay (this->m_display);
	}

	if (this->m_xext != nullptr) {
	    dlclose (this->m_xext);
	}

	if (this->m_x11 != nullptr) {
	    dlclose (this->m_x11);
	}
    }

    bool open () {
	if (std::getenv ("DISPLAY") == nullptr) {
	    return false;
	}

	this->m_x11 = dlopen ("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
	this->m_xext = dlopen ("libXext.so.6", RTLD_NOW | RTLD_LOCAL);

	if (this->m_x11 == nullptr || this->m_xext == nullptr) {
	    return false;
	}

	auto openDisplay = reinterpret_cast<void* (*)(const char*)> (dlsym (this->m_x11, "XOpenDisplay"));
	this->m_closeDisplay = reinterpret_cast<int (*) (void*)> (dlsym (this->m_x11, "XCloseDisplay"));
	auto queryExtension
	    = reinterpret_cast<int (*) (void*, int*, int*)> (dlsym (this->m_xext, "DPMSQueryExtension"));
	auto capable = reinterpret_cast<int (*) (void*)> (dlsym (this->m_xext, "DPMSCapable"));
	this->m_info
	    = reinterpret_cast<int (*) (void*, unsigned short*, unsigned char*)> (dlsym (this->m_xext, "DPMSInfo"));

	if (openDisplay == nullptr || this->m_closeDisplay == nullptr || queryExtension == nullptr || capable == nullptr
	    || this->m_info == nullptr) {
	    return false;
	}

	this->m_display = openDisplay (nullptr);

	int eventBase = 0;
	int errorBase = 0;

	return this->m_display != nullptr && queryExtension (this->m_display, &eventBase, &errorBase)
	    && capable (this->m_display);
    }

    /** DPMS off, standby and suspend all mean a blank screen. Nothing when DPMS is not enabled on the server */
    std::optional<bool> blanked () {
	unsigned short level = 0;
	unsigned char enabled = 0;

	if (!this->m_info (this->m_display, &level, &enabled)) {
	    return std::nullopt;
	}

	return enabled != 0 && level != 0;
    }

private:
    void* m_x11 = nullptr;
    void* m_xext = nullptr;
    void* m_display = nullptr;
    int (*m_closeDisplay) (void*) = nullptr;
    int (*m_info) (void*, unsigned short*, unsigned char*) = nullptr;
};

#ifdef LWE_KWIN_DPMS
// KWin's org_kde_kwin_dpms: the compositor tells the state of each output, nothing is ever requested (the protocol
// has a set request). Other compositors don't offer the manager and the connection is dropped again
class KWinDpms {
public:
    explicit KWinDpms (wl_display* display) : m_display (display) { }

    ~KWinDpms () {
	this->m_outputs.clear ();

	if (this->m_manager != nullptr) {
	    org_kde_kwin_dpms_manager_destroy (this->m_manager);
	}

	if (this->m_registry != nullptr) {
	    wl_registry_destroy (this->m_registry);
	}

	wl_display_disconnect (this->m_display);
    }

    KWinDpms (const KWinDpms&) = delete;
    KWinDpms& operator= (const KWinDpms&) = delete;

    bool open () {
	this->m_registry = wl_display_get_registry (this->m_display);
	wl_registry_add_listener (this->m_registry, &registryListener, this);

	if (!this->roundtrip () || this->m_manager == nullptr) {
	    return false;
	}

	for (const auto& output : this->m_outputs) {
	    this->attach (*output);
	}

	return this->roundtrip ();
    }

    [[nodiscard]] int fd () const { return wl_display_get_fd (this->m_display); }
    [[nodiscard]] bool failed () const { return wl_display_get_error (this->m_display) != 0; }

    void beforePoll () {
	while (wl_display_prepare_read (this->m_display) != 0) {
	    wl_display_dispatch_pending (this->m_display);
	}

	wl_display_flush (this->m_display);
    }

    void afterPoll (const bool readable) {
	if (readable) {
	    wl_display_read_events (this->m_display);
	} else {
	    wl_display_cancel_read (this->m_display);
	}

	wl_display_dispatch_pending (this->m_display);
    }

    [[nodiscard]] bool allOff () const {
	return !this->m_outputs.empty () && std::ranges::all_of (this->m_outputs, [] (const auto& output) {
	    return output->mode != ORG_KDE_KWIN_DPMS_MODE_ON;
	});
    }

private:
    static constexpr int ROUNDTRIP_MS = 2000;

    bool roundtrip () {
	bool done = false;
	wl_callback* callback = wl_display_sync (this->m_display);

	wl_callback_add_listener (callback, &syncListener, &done);

	const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (ROUNDTRIP_MS);

	while (!done && !this->failed ()) {
	    const auto left
		= std::chrono::duration_cast<std::chrono::milliseconds> (deadline - std::chrono::steady_clock::now ());

	    if (left.count () <= 0) {
		break;
	    }

	    this->beforePoll ();

	    pollfd pending = { .fd = this->fd (), .events = POLLIN, .revents = 0 };
	    const int ready = poll (&pending, 1, static_cast<int> (left.count ()));

	    this->afterPoll (ready > 0);
	}

	if (!done) {
	    wl_callback_destroy (callback);
	}

	return done;
    }

    static void syncDone (void* data, wl_callback*, uint32_t) { *static_cast<bool*> (data) = true; }

    static constexpr wl_callback_listener syncListener = { .done = syncDone };

    struct Output {
	uint32_t name = 0;
	wl_output* output = nullptr;
	org_kde_kwin_dpms* dpms = nullptr;
	uint32_t mode = ORG_KDE_KWIN_DPMS_MODE_ON;

	~Output () {
	    if (this->dpms != nullptr) {
		org_kde_kwin_dpms_release (this->dpms);
	    }

	    if (this->output != nullptr) {
		wl_output_destroy (this->output);
	    }
	}
    };

    void attach (Output& output) const {
	if (output.dpms == nullptr && this->m_manager != nullptr) {
	    output.dpms = org_kde_kwin_dpms_manager_get (this->m_manager, output.output);
	    org_kde_kwin_dpms_add_listener (output.dpms, &dpmsListener, &output);
	}
    }

    static void registryGlobal (void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
	auto* self = static_cast<KWinDpms*> (data);

	if (std::strcmp (interface, org_kde_kwin_dpms_manager_interface.name) == 0) {
	    self->m_manager = static_cast<org_kde_kwin_dpms_manager*> (
		wl_registry_bind (registry, name, &org_kde_kwin_dpms_manager_interface, 1)
	    );
	} else if (std::strcmp (interface, wl_output_interface.name) == 0) {
	    auto& output = self->m_outputs.emplace_back (std::make_unique<Output> ());
	    output->name = name;
	    output->output = static_cast<wl_output*> (wl_registry_bind (registry, name, &wl_output_interface, 1));
	    self->attach (*output);
	}
    }

    static void registryGlobalRemove (void* data, wl_registry*, uint32_t name) {
	std::erase_if (static_cast<KWinDpms*> (data)->m_outputs, [name] (const auto& output) {
	    return output->name == name;
	});
    }

    static void dpmsSupported (void*, org_kde_kwin_dpms*, uint32_t) { }

    static void dpmsMode (void* data, org_kde_kwin_dpms*, uint32_t mode) { static_cast<Output*> (data)->mode = mode; }

    static void dpmsDone (void*, org_kde_kwin_dpms*) { }

    static constexpr wl_registry_listener registryListener = {
	.global = registryGlobal,
	.global_remove = registryGlobalRemove,
    };

    static constexpr org_kde_kwin_dpms_listener dpmsListener = {
	.supported = dpmsSupported,
	.mode = dpmsMode,
	.done = dpmsDone,
    };

    wl_display* m_display;
    wl_registry* m_registry = nullptr;
    org_kde_kwin_dpms_manager* m_manager = nullptr;
    std::vector<std::unique_ptr<Output>> m_outputs;
};
#endif

// sway and Hyprland blank an output without telling its clients (wlr-output-management still says enabled, and sway
// keeps sending frame callbacks), but both list the power state of their outputs on the IPC socket. Only read, asked
// once a second like an X server's DPMS
class OutputPower {
public:
    static std::unique_ptr<OutputPower> detect () {
	const char* runtime = std::getenv ("XDG_RUNTIME_DIR");

	if (const char* sway = std::getenv ("SWAYSOCK"); sway != nullptr && *sway != '\0') {
	    return std::unique_ptr<OutputPower> (new OutputPower (Kind::Sway, sway));
	}

	if (auto hypr = findHyprlandSocket (
		runtime != nullptr ? runtime : "", std::getenv ("HYPRLAND_INSTANCE_SIGNATURE"),
		std::getenv ("WAYLAND_DISPLAY"), std::getenv ("XDG_CURRENT_DESKTOP")
	    );
	    !hypr.empty ()) {
	    return std::unique_ptr<OutputPower> (new OutputPower (Kind::Hyprland, hypr));
	}

	return nullptr;
    }

    std::optional<bool> blanked () const {
	const auto reply
	    = this->m_kind == Kind::Sway ? this->swayOutputs () : hyprlandRequest (this->m_path, "j/monitors");

	if (!reply.has_value ()) {
	    return std::nullopt;
	}

	const auto outputs = nlohmann::json::parse (*reply, nullptr, false);

	if (!outputs.is_array ()) {
	    return std::nullopt;
	}

	const char* key = this->m_kind == Kind::Sway ? "power" : "dpmsStatus";
	bool any = false;

	for (const auto& output : outputs) {
	    if (!output.is_object ()) {
		return std::nullopt;
	    }

	    if (this->m_kind == Kind::Sway) {
		const auto active = output.find ("active");

		if (active != output.end () && !active->is_boolean ()) {
		    return std::nullopt;
		}

		if (active == output.end () || !active->get<bool> ()) {
		    continue;
		}
	    }

	    const auto state = output.find (key);

	    if (state == output.end () || !state->is_boolean ()) {
		return std::nullopt;
	    }

	    if (state->get<bool> ()) {
		return false;
	    }

	    any = true;
	}

	return any;
    }

private:
    enum class Kind { Sway, Hyprland };

    OutputPower (const Kind kind, std::string path) : m_kind (kind), m_path (std::move (path)) { }

    std::optional<std::string> swayOutputs () const {
	const int fd = connectUnixSocket (this->m_path, 100000);

	if (fd < 0) {
	    return std::nullopt;
	}

	std::optional<std::string> result;

	constexpr std::string_view MAGIC = "i3-ipc";
	char request[MAGIC.size () + 8] = {};
	const uint32_t type = 3;
	std::copy (MAGIC.begin (), MAGIC.end (), request);
	std::memcpy (request + MAGIC.size () + 4, &type, sizeof (type));

	char header[sizeof (request)];

	if (send (fd, request, sizeof (request), MSG_NOSIGNAL) == static_cast<ssize_t> (sizeof (request))
	    && recvAll (fd, header, sizeof (header))) {
	    uint32_t length = 0;
	    std::memcpy (&length, header + MAGIC.size (), sizeof (length));

	    // a monitor list is a few KB, anything near a megabyte is not one
	    if (length < (1u << 20)) {
		std::string payload (length, '\0');

		if (recvAll (fd, payload.data (), length)) {
		    result = std::move (payload);
		}
	    }
	}

	close (fd);
	return result;
    }

    static bool recvAll (const int fd, char* data, const size_t size) {
	size_t done = 0;

	while (done < size) {
	    const ssize_t got = recv (fd, data + done, size - done, 0);

	    if (got <= 0) {
		return false;
	    }

	    done += static_cast<size_t> (got);
	}

	return true;
    }

    Kind m_kind;
    std::string m_path;
};

DBusMessage* methodCall (const char* service, const char* path, const char* interface, const char* method) {
    DBusMessage* call = dbus_message_new_method_call (service, path, interface, method);

    // asking must never start a screen saver the session doesn't run
    dbus_message_set_auto_start (call, FALSE);
    return call;
}

std::optional<bool> readBoolean (DBusMessage* reply) {
    DBusMessageIter iter;
    dbus_bool_t value = FALSE;

    if (!dbus_message_iter_init (reply, &iter)) {
	return std::nullopt;
    }

    if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_VARIANT) {
	DBusMessageIter variant;
	dbus_message_iter_recurse (&iter, &variant);

	if (dbus_message_iter_get_arg_type (&variant) != DBUS_TYPE_BOOLEAN) {
	    return std::nullopt;
	}

	dbus_message_iter_get_basic (&variant, &value);
	return value != FALSE;
    }

    if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_BOOLEAN) {
	return std::nullopt;
    }

    dbus_message_iter_get_basic (&iter, &value);
    return value != FALSE;
}

std::optional<bool> screenSaverActive (DBusConnection* bus, const ScreenSaverName& name) {
    std::string error;
    DBusMessage* reply = callBlocking (bus, methodCall (name.service, name.path, name.interface, "GetActive"), error);

    if (reply == nullptr) {
	return std::nullopt;
    }

    const auto active = readBoolean (reply);
    dbus_message_unref (reply);
    return active;
}

std::string logindSessionPath (DBusConnection* bus) {
    const auto objectPath = [] (DBusMessage* reply) -> std::string {
	const char* path = nullptr;
	std::string result;

	if (reply != nullptr) {
	    if (dbus_message_get_args (reply, nullptr, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
		result = path;
	    }

	    dbus_message_unref (reply);
	}

	return result;
    };
    std::string error;

    DBusMessage* byPid = methodCall (
	"org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "GetSessionByPID"
    );
    const dbus_uint32_t pid = static_cast<dbus_uint32_t> (getpid ());
    dbus_message_append_args (byPid, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);

    if (auto path = objectPath (callBlocking (bus, byPid, error)); !path.empty ()) {
	return path;
    }

    // a Flatpak's pid is not the one logind knows
    if (const char* id = std::getenv ("XDG_SESSION_ID"); id != nullptr && *id != '\0') {
	DBusMessage* byId = methodCall (
	    "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "GetSession"
	);
	dbus_message_append_args (byId, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID);
	return objectPath (callBlocking (bus, byId, error));
    }

    return {};
}

std::optional<bool> logindLockedHint (DBusConnection* bus, const std::string& session) {
    DBusMessage* reply = getProperty (
	bus, "org.freedesktop.login1", session.c_str (), "org.freedesktop.login1.Session", "LockedHint", false
    );

    if (reply == nullptr) {
	return std::nullopt;
    }

    const auto hint = readBoolean (reply);
    dbus_message_unref (reply);
    return hint;
}

// 0 is on, 1 standby, 2 suspend, 3 off
std::optional<bool> mutterDisplayOff (DBusConnection* bus) {
    DBusMessage* reply = getProperty (
	bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig", "org.gnome.Mutter.DisplayConfig",
	"PowerSaveMode", false
    );

    if (reply == nullptr) {
	return std::nullopt;
    }

    std::optional<bool> off;
    DBusMessageIter variant;

    if (propertyValue (reply, variant) && dbus_message_iter_get_arg_type (&variant) == DBUS_TYPE_INT32) {
	dbus_int32_t mode = 0;
	dbus_message_iter_get_basic (&variant, &mode);
	off = mode > 0;
    }

    dbus_message_unref (reply);
    return off;
}

void addMatch (DBusConnection* bus, const std::string& rule) {
    DBusError error;
    dbus_error_init (&error);
    dbus_bus_add_match (bus, rule.c_str (), &error);

    if (dbus_error_is_set (&error)) {
	dbus_error_free (&error);
    }
}

}

struct SessionMonitor::Sources {
    DBusConnection* systemBus = nullptr;
    DBusConnection* sessionBus = nullptr;
    std::string session;
    std::atomic<bool> changed = true;
    // the argument of the last ActiveChanged, -1 before the first one; a locker that doesn't answer GetActive still
    // sends it
    std::atomic<int> screenSaverSignal = -1;
    bool hint = false;
    bool screenSaver = false;
    std::unique_ptr<X11Dpms> dpms;
    std::unique_ptr<OutputPower> outputPower;
    bool outputsOff = false;
    std::chrono::steady_clock::time_point outputsChecked {};
    bool mutterOff = false;
#ifdef LWE_KWIN_DPMS
    wl_display* kwinDisplay = nullptr;
    std::unique_ptr<KWinDpms> kwin;
#endif
};

namespace {
DBusHandlerResult noteSignal (DBusConnection*, DBusMessage* message, void* data) {
    auto& sources = *static_cast<SessionMonitor::Sources*> (data);

    if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_SIGNAL) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    dbus_bool_t active = FALSE;

    if (dbus_message_is_signal (message, "org.freedesktop.ScreenSaver", "ActiveChanged")
	|| dbus_message_is_signal (message, "org.gnome.ScreenSaver", "ActiveChanged")
	|| dbus_message_is_signal (message, "org.cinnamon.ScreenSaver", "ActiveChanged")
	|| dbus_message_is_signal (message, "org.mate.ScreenSaver", "ActiveChanged")) {
	if (dbus_message_get_args (message, nullptr, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID)) {
	    sources.screenSaverSignal = active != FALSE;
	}
    }

    sources.changed = true;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
}

SessionMonitor::SessionMonitor () :
    m_wakeFd (eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK)), m_sources (std::make_unique<Sources> ()) {
#ifdef LWE_KWIN_DPMS
    // main thread: connectWayland picks the route to the compositor once (Flatpak helper)
    if (const char* type = std::getenv ("XDG_SESSION_TYPE"); type != nullptr && std::string_view (type) == "wayland"
	&& std::getenv ("WAYLAND_DISPLAY") != nullptr && isKDESession ()) {
	this->m_sources->kwinDisplay = connectWayland ();
    }
#endif

    try {
	this->m_thread = std::thread ([this] {
	    WallpaperEngine::Logging::guardThread ("Session monitor", [this] { this->run (); });
	});
    } catch (...) {
	if (this->m_wakeFd >= 0) {
	    close (this->m_wakeFd);
	}

#ifdef LWE_KWIN_DPMS
	if (this->m_sources->kwinDisplay != nullptr) {
	    wl_display_disconnect (this->m_sources->kwinDisplay);
	}
#endif

	throw;
    }
}

SessionMonitor::~SessionMonitor () {
    this->m_stop = true;
    this->wake ();

    if (this->m_thread.joinable ()) {
	this->m_thread.join ();
    }

    closeBus (this->m_sources->systemBus);
    closeBus (this->m_sources->sessionBus);

    if (this->m_wakeFd >= 0) {
	close (this->m_wakeFd);
    }
}

void SessionMonitor::wake () const {
    if (this->m_wakeFd >= 0) {
	const uint64_t one = 1;
	std::ignore = write (this->m_wakeFd, &one, sizeof (one));
    }
}

void SessionMonitor::run () {
    auto& sources = *this->m_sources;

    sources.systemBus = privateBus (DBUS_BUS_SYSTEM);
    sources.sessionBus = privateBus (DBUS_BUS_SESSION);

    if (sources.systemBus != nullptr) {
	sources.session = logindSessionPath (sources.systemBus);

	if (!sources.session.empty ()) {
	    addMatch (
		sources.systemBus,
		"type='signal',sender='org.freedesktop.login1',path='" + sources.session
		    + "',interface='org.freedesktop.DBus.Properties'"
	    );
	    addMatch (
		sources.systemBus,
		"type='signal',sender='org.freedesktop.login1',path='" + sources.session
		    + "',interface='org.freedesktop.login1.Session'"
	    );
	    dbus_connection_add_filter (sources.systemBus, noteSignal, &sources, nullptr);
	} else {
	    sLog.debug ("Lock: no logind session, relying on the screen saver");
	}
    }

    if (sources.sessionBus != nullptr) {
	for (const auto& name : SCREEN_SAVERS) {
	    addMatch (
		sources.sessionBus,
		std::string ("type='signal',interface='") + name.interface + "',member='ActiveChanged'"
	    );
	}

	addMatch (
	    sources.sessionBus,
	    "type='signal',sender='org.gnome.Mutter.DisplayConfig',interface='org.freedesktop.DBus.Properties'"
	);
	dbus_connection_add_filter (sources.sessionBus, noteSignal, &sources, nullptr);
    }

    // an X server tells nothing by itself, it is asked once a second
    if (const char* type = std::getenv ("XDG_SESSION_TYPE"); type == nullptr || std::string_view (type) == "x11") {
	sources.dpms = std::make_unique<X11Dpms> ();

	if (!sources.dpms->open ()) {
	    sources.dpms.reset ();
	}
    }

    if (const char* type = std::getenv ("XDG_SESSION_TYPE"); type != nullptr && std::string_view (type) == "wayland") {
	sources.outputPower = OutputPower::detect ();
    }

#ifdef LWE_KWIN_DPMS
    if (sources.kwinDisplay != nullptr) {
	sources.kwin = std::make_unique<KWinDpms> (sources.kwinDisplay);
	sources.kwinDisplay = nullptr;

	if (!sources.kwin->open ()) {
	    sLog.debug ("Display: no org_kde_kwin_dpms, relying on frame callbacks");
	    sources.kwin.reset ();
	}
    }
#endif

    auto nextRecheck = std::chrono::steady_clock::now ();

    while (!this->m_stop) {
	if (sources.changed.exchange (false) || std::chrono::steady_clock::now () >= nextRecheck) {
	    nextRecheck = std::chrono::steady_clock::now () + RECHECK_INTERVAL;

	    // a call that timed out says nothing, the state from before stays
	    if (!sources.session.empty ()) {
		if (const auto hint = logindLockedHint (sources.systemBus, sources.session); hint.has_value ()) {
		    sources.hint = *hint;
		}
	    }

	    std::optional<bool> active;
	    bool saverOnBus = false;

	    if (sources.sessionBus != nullptr) {
		for (const auto& name : SCREEN_SAVERS) {
		    if (this->m_stop) {
			break;
		    }

		    saverOnBus = saverOnBus || dbus_bus_name_has_owner (sources.sessionBus, name.service, nullptr);

		    if (const auto answer = screenSaverActive (sources.sessionBus, name); answer.has_value ()) {
			active = active.value_or (false) || *answer;
		    }
		}
	    }

	    if (this->m_stop) {
		break;
	    }

	    // a locker that went away while locked never sends ActiveChanged(false)
	    sources.screenSaver = active.value_or (saverOnBus && sources.screenSaverSignal == 1);

	    if (sources.sessionBus != nullptr) {
		if (const auto off = mutterDisplayOff (sources.sessionBus); off.has_value ()) {
		    sources.mutterOff = *off;
		}
	    }

	    const bool locked = sources.hint || sources.screenSaver;

	    if (locked != this->m_locked) {
		this->m_locked = locked;
		sLog.out (
		    "Lock: session ", locked ? "locked" : "unlocked", " (logind hint ", sources.hint ? "set" : "clear",
		    ", screen saver ", sources.screenSaver ? "active" : "inactive", ")"
		);
	    }
	}

	pollfd fds[4] = {
	    { .fd = this->m_wakeFd, .events = POLLIN, .revents = 0 },
	    { .fd = -1, .events = POLLIN, .revents = 0 },
	    { .fd = -1, .events = POLLIN, .revents = 0 },
	    { .fd = -1, .events = POLLIN, .revents = 0 },
	};

	// the blocking calls above read the socket and queue whatever else arrived, poll never sees that again
	bool queued = false;

	for (int i = 0; i < 2; i++) {
	    DBusConnection* bus = i == 0 ? sources.systemBus : sources.sessionBus;

	    if (bus != nullptr && dbus_connection_get_is_connected (bus)) {
		int fd = -1;
		dbus_connection_get_unix_fd (bus, &fd);
		fds[1 + i].fd = fd;
		queued = queued || dbus_connection_get_dispatch_status (bus) == DBUS_DISPATCH_DATA_REMAINS;
	    }
	}

	bool displayOff = sources.mutterOff;

	if (sources.dpms != nullptr) {
	    displayOff = displayOff || sources.dpms->blanked ().value_or (false);
	}

	if (sources.outputPower != nullptr) {
	    const auto now = std::chrono::steady_clock::now ();

	    if (now - sources.outputsChecked >= std::chrono::milliseconds (DPMS_POLL_MS)) {
		sources.outputsOff = sources.outputPower->blanked ().value_or (false);
		sources.outputsChecked = now;
	    }

	    displayOff = displayOff || sources.outputsOff;
	}

#ifdef LWE_KWIN_DPMS
	if (sources.kwin != nullptr) {
	    if (sources.kwin->failed ()) {
		sLog.debug ("Display: lost the Wayland connection used for KWin's DPMS state");
		sources.kwin.reset ();
	    } else {
		displayOff = displayOff || sources.kwin->allOff ();
		sources.kwin->beforePoll ();
		fds[3].fd = sources.kwin->fd ();
	    }
	}
#endif

	if (displayOff != this->m_displayOff) {
	    this->m_displayOff = displayOff;
	    sLog.out ("Display: ", displayOff ? "blanked (DPMS)" : "on again");
	}

	auto untilRecheck
	    = std::chrono::duration_cast<std::chrono::milliseconds> (nextRecheck - std::chrono::steady_clock::now ());

	if (sources.dpms != nullptr || sources.outputPower != nullptr) {
	    untilRecheck = std::min (untilRecheck, std::chrono::milliseconds (DPMS_POLL_MS));
	}

	poll (fds, 4, queued ? 0 : static_cast<int> (std::max<int64_t> (1, untilRecheck.count ())));

#ifdef LWE_KWIN_DPMS
	if (sources.kwin != nullptr) {
	    sources.kwin->afterPoll ((fds[3].revents & POLLIN) != 0);
	}
#endif

	if (fds[0].revents & POLLIN) {
	    uint64_t value;
	    std::ignore = read (this->m_wakeFd, &value, sizeof (value));
	}

	// unread messages count against the session bus quota, read them even when only the other bus spoke
	for (DBusConnection* bus : { sources.systemBus, sources.sessionBus }) {
	    if (bus != nullptr) {
		drainBus (bus);
	    }
	}
    }
}
