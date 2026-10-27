#include "SessionMonitor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <tuple>
#include <unistd.h>

#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"

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
    bool mutterOff = false;
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
    try {
	this->m_thread = std::thread ([this] {
	    WallpaperEngine::Logging::guardThread ("Session monitor", [this] { this->run (); });
	});
    } catch (...) {
	if (this->m_wakeFd >= 0) {
	    close (this->m_wakeFd);
	}

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

	pollfd fds[3] = {
	    { .fd = this->m_wakeFd, .events = POLLIN, .revents = 0 },
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

	if (displayOff != this->m_displayOff) {
	    this->m_displayOff = displayOff;
	    sLog.out ("Display: ", displayOff ? "blanked (DPMS)" : "on again");
	}

	auto untilRecheck
	    = std::chrono::duration_cast<std::chrono::milliseconds> (nextRecheck - std::chrono::steady_clock::now ());

	if (sources.dpms != nullptr) {
	    untilRecheck = std::min (untilRecheck, std::chrono::milliseconds (DPMS_POLL_MS));
	}

	poll (fds, 3, queued ? 0 : static_cast<int> (std::max<int64_t> (1, untilRecheck.count ())));

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
