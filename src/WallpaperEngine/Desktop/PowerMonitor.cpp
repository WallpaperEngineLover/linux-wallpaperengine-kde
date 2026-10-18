#include "PowerMonitor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dbus/dbus.h>
#include <dlfcn.h>
#include <fstream>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>

#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"

#ifdef ENABLE_WAYLAND
#include "WallpaperEngine/Desktop/HostWayland.h"
#include "ext-idle-notify-v1-protocol.h"
#include <wayland-client.h>
#endif /* ENABLE_WAYLAND */

using namespace WallpaperEngine::Desktop;

namespace {
constexpr auto BATTERY_INTERVAL = std::chrono::seconds (5);
/** poll fast while idle so the wallpaper resumes quickly */
constexpr auto IDLE_POLL_ACTIVE = std::chrono::milliseconds (250);
constexpr auto IDLE_POLL_MAX = std::chrono::milliseconds (5000);

std::string readLine (const std::filesystem::path& path) {
    std::ifstream file (path);
    std::string line;
    std::getline (file, line);
    return line;
}

// MIT-SCREEN-SAVER through dlopen, libXss isn't a build dependency
struct ScreenSaverInfo {
    unsigned long window;
    int state;
    int kind;
    unsigned long tilOrSince;
    unsigned long idle;
    unsigned long eventMask;
};

class X11Idle {
public:
    ~X11Idle () {
	if (this->m_info != nullptr) {
	    this->m_free (this->m_info);
	}

	if (this->m_display != nullptr) {
	    this->m_closeDisplay (this->m_display);
	}

	if (this->m_xss != nullptr) {
	    dlclose (this->m_xss);
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
	this->m_xss = dlopen ("libXss.so.1", RTLD_NOW | RTLD_LOCAL);

	if (this->m_x11 == nullptr || this->m_xss == nullptr) {
	    return false;
	}

	auto openDisplay = reinterpret_cast<void* (*)(const char*)> (dlsym (this->m_x11, "XOpenDisplay"));
	this->m_closeDisplay = reinterpret_cast<int (*) (void*)> (dlsym (this->m_x11, "XCloseDisplay"));
	this->m_free = reinterpret_cast<int (*) (void*)> (dlsym (this->m_x11, "XFree"));
	this->m_rootWindow = reinterpret_cast<unsigned long (*) (void*)> (dlsym (this->m_x11, "XDefaultRootWindow"));
	auto queryExtension
	    = reinterpret_cast<int (*) (void*, int*, int*)> (dlsym (this->m_xss, "XScreenSaverQueryExtension"));
	auto allocInfo = reinterpret_cast<ScreenSaverInfo* (*)()> (dlsym (this->m_xss, "XScreenSaverAllocInfo"));
	this->m_query = reinterpret_cast<int (*) (void*, unsigned long, ScreenSaverInfo*)> (
	    dlsym (this->m_xss, "XScreenSaverQueryInfo")
	);

	if (openDisplay == nullptr || this->m_closeDisplay == nullptr || this->m_free == nullptr
	    || this->m_rootWindow == nullptr || queryExtension == nullptr || allocInfo == nullptr
	    || this->m_query == nullptr) {
	    return false;
	}

	this->m_display = openDisplay (nullptr);

	if (this->m_display == nullptr) {
	    return false;
	}

	int eventBase = 0;
	int errorBase = 0;

	if (!queryExtension (this->m_display, &eventBase, &errorBase)) {
	    return false;
	}

	this->m_info = allocInfo ();
	return this->m_info != nullptr;
    }

    std::optional<uint64_t> idleMilliseconds () {
	if (!this->m_query (this->m_display, this->m_rootWindow (this->m_display), this->m_info)) {
	    return std::nullopt;
	}

	return this->m_info->idle;
    }

private:
    void* m_x11 = nullptr;
    void* m_xss = nullptr;
    void* m_display = nullptr;
    ScreenSaverInfo* m_info = nullptr;
    int (*m_closeDisplay) (void*) = nullptr;
    int (*m_free) (void*) = nullptr;
    unsigned long (*m_rootWindow) (void*) = nullptr;
    int (*m_query) (void*, unsigned long, ScreenSaverInfo*) = nullptr;
};

std::optional<bool> upowerOnBattery (DBusConnection* bus) {
    DBusMessage* call = dbus_message_new_method_call (
	"org.freedesktop.UPower", "/org/freedesktop/UPower", "org.freedesktop.DBus.Properties", "Get"
    );
    const char* interface = "org.freedesktop.UPower";
    const char* property = "OnBattery";
    dbus_message_append_args (call, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);

    std::string error;
    DBusMessage* reply = callBlocking (bus, call, error);

    if (reply == nullptr) {
	return std::nullopt;
    }

    std::optional<bool> result;
    DBusMessageIter iter;
    DBusMessageIter variant;

    if (dbus_message_iter_init (reply, &iter) && dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_VARIANT) {
	dbus_message_iter_recurse (&iter, &variant);

	if (dbus_message_iter_get_arg_type (&variant) == DBUS_TYPE_BOOLEAN) {
	    dbus_bool_t value = FALSE;
	    dbus_message_iter_get_basic (&variant, &value);
	    result = value != FALSE;
	}
    }

    dbus_message_unref (reply);
    return result;
}

std::optional<uint64_t> mutterIdleMilliseconds (DBusConnection* bus) {
    DBusMessage* call = dbus_message_new_method_call (
	"org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core", "org.gnome.Mutter.IdleMonitor",
	"GetIdletime"
    );

    std::string error;
    DBusMessage* reply = callBlocking (bus, call, error);

    if (reply == nullptr) {
	return std::nullopt;
    }

    dbus_uint64_t idle = 0;
    const bool ok = dbus_message_get_args (reply, nullptr, DBUS_TYPE_UINT64, &idle, DBUS_TYPE_INVALID);
    dbus_message_unref (reply);

    return ok ? std::optional<uint64_t> (idle) : std::nullopt;
}

DBusConnection* privateBus (DBusBusType type) {
    DBusError error;
    dbus_error_init (&error);

    DBusConnection* bus = dbus_bus_get_private (type, &error);

    if (dbus_error_is_set (&error)) {
	dbus_error_free (&error);
	return nullptr;
    }

    dbus_connection_set_exit_on_disconnect (bus, FALSE);
    return bus;
}

void closeBus (DBusConnection* bus) {
    if (bus != nullptr) {
	dbus_connection_close (bus);
	dbus_connection_unref (bus);
    }
}
} // namespace

std::optional<bool> WallpaperEngine::Desktop::sysfsOnBattery (const std::filesystem::path& powerSupplies) {
    std::error_code error;
    bool anyBattery = false;
    bool discharging = false;
    bool externalPower = false;

    for (const auto& entry : std::filesystem::directory_iterator (powerSupplies, error)) {
	const auto& dir = entry.path ();

	// mice, headsets and controllers have scope Device
	if (readLine (dir / "scope") == "Device") {
	    continue;
	}

	const std::string type = readLine (dir / "type");

	if (type == "Battery") {
	    anyBattery = true;
	    discharging = discharging || readLine (dir / "status") == "Discharging";
	} else if (readLine (dir / "online") == "1") {
	    externalPower = true;
	}
    }

    if (!anyBattery) {
	return std::nullopt;
    }

    return discharging && !externalPower;
}

struct PowerMonitor::Sources {
    DBusConnection* systemBus = nullptr;
    DBusConnection* sessionBus = nullptr;
    bool upowerWorks = true;
    bool mutterWorks = true;
    std::unique_ptr<X11Idle> x11;
    bool x11Tried = false;
    bool loggedUnavailable = false;

#ifdef ENABLE_WAYLAND
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    ext_idle_notifier_v1* notifier = nullptr;
    ext_idle_notification_v1* notification = nullptr;
#endif /* ENABLE_WAYLAND */

#ifdef ENABLE_WAYLAND
    void disconnectWayland () {
	if (this->notification != nullptr) {
	    ext_idle_notification_v1_destroy (this->notification);
	}

	if (this->notifier != nullptr) {
	    ext_idle_notifier_v1_destroy (this->notifier);
	}

	if (this->seat != nullptr) {
	    wl_seat_destroy (this->seat);
	}

	if (this->registry != nullptr) {
	    wl_registry_destroy (this->registry);
	}

	if (this->display != nullptr) {
	    wl_display_disconnect (this->display);
	}

	this->notification = nullptr;
	this->notifier = nullptr;
	this->seat = nullptr;
	this->registry = nullptr;
	this->display = nullptr;
    }
#endif /* ENABLE_WAYLAND */

    ~Sources () {
#ifdef ENABLE_WAYLAND
	this->disconnectWayland ();
#endif /* ENABLE_WAYLAND */

	closeBus (this->systemBus);
	closeBus (this->sessionBus);
    }
};

#ifdef ENABLE_WAYLAND
namespace {
void registryGlobal (void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* sources = static_cast<PowerMonitor::Sources*> (data);

    if (std::strcmp (interface, wl_seat_interface.name) == 0 && sources->seat == nullptr) {
	sources->seat = static_cast<wl_seat*> (wl_registry_bind (registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp (interface, ext_idle_notifier_v1_interface.name) == 0) {
	sources->notifier = static_cast<ext_idle_notifier_v1*> (
	    wl_registry_bind (registry, name, &ext_idle_notifier_v1_interface, 1)
	);
    }
}

void registryGlobalRemove (void*, wl_registry*, uint32_t) { }

const wl_registry_listener registryListener = {
    .global = registryGlobal,
    .global_remove = registryGlobalRemove,
};

void notificationIdled (void* data, ext_idle_notification_v1*) { static_cast<std::atomic<bool>*> (data)->store (true); }

void notificationResumed (void* data, ext_idle_notification_v1*) {
    static_cast<std::atomic<bool>*> (data)->store (false);
}

const ext_idle_notification_v1_listener notificationListener = {
    .idled = notificationIdled,
    .resumed = notificationResumed,
};
} // namespace
#endif /* ENABLE_WAYLAND */

PowerMonitor::PowerMonitor () : m_wakeFd (eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK)), m_sources (new Sources) {
    // UPower and the compositor are queried on the thread, sysfs answers until then
    this->m_onBattery = sysfsOnBattery ("/sys/class/power_supply").value_or (false);

#ifdef ENABLE_WAYLAND
    // main thread: connectWayland picks the route to the compositor once (Flatpak helper)
    if (const char* session = std::getenv ("XDG_SESSION_TYPE");
	session != nullptr && std::string_view (session) == "wayland" && std::getenv ("WAYLAND_DISPLAY") != nullptr) {
	this->m_sources->display = connectWayland ();
    }
#endif /* ENABLE_WAYLAND */

    this->m_thread
	= std::thread ([this] { WallpaperEngine::Logging::guardThread ("Power monitor", [this] { this->run (); }); });
}

PowerMonitor::~PowerMonitor () {
    this->m_stop = true;
    this->wake ();

    if (this->m_thread.joinable ()) {
	this->m_thread.join ();
    }

    delete this->m_sources;

    if (this->m_wakeFd >= 0) {
	close (this->m_wakeFd);
    }
}

void PowerMonitor::setIdleTimeout (int minutes) {
    this->m_idleMinutes = std::max (0, minutes);
    this->wake ();
}

void PowerMonitor::wake () const {
    if (this->m_wakeFd >= 0) {
	const uint64_t one = 1;
	std::ignore = write (this->m_wakeFd, &one, sizeof (one));
    }
}

void PowerMonitor::run () {
    using Clock = std::chrono::steady_clock;

    auto& sources = *this->m_sources;
    bool lastBattery = this->m_onBattery;

    sources.systemBus = privateBus (DBUS_BUS_SYSTEM);

    if (const auto onBattery = sources.systemBus != nullptr ? upowerOnBattery (sources.systemBus) : std::nullopt;
	onBattery.has_value ()) {
	this->m_onBattery = *onBattery;
    } else {
	sources.upowerWorks = false;
    }

#ifdef ENABLE_WAYLAND
    if (sources.display != nullptr) {
	sources.registry = wl_display_get_registry (sources.display);
	wl_registry_add_listener (sources.registry, &registryListener, &sources);
	wl_display_roundtrip (sources.display);

	if (sources.seat == nullptr || sources.notifier == nullptr) {
	    sLog.debug ("Power: compositor has no ext-idle-notify-v1");
	}
    }
#endif /* ENABLE_WAYLAND */

    auto nextBatteryCheck = Clock::now () + BATTERY_INTERVAL;
    int appliedMinutes = 0;

    while (!this->m_stop) {
	const auto now = Clock::now ();

	if (now >= nextBatteryCheck) {
	    std::optional<bool> onBattery;

	    if (sources.upowerWorks && sources.systemBus != nullptr) {
		onBattery = upowerOnBattery (sources.systemBus);
	    }

	    if (!onBattery.has_value ()) {
		onBattery = sysfsOnBattery ("/sys/class/power_supply");
	    }

	    this->m_onBattery = onBattery.value_or (false);
	    nextBatteryCheck = now + BATTERY_INTERVAL;
	}

	if (this->m_onBattery != lastBattery) {
	    lastBattery = this->m_onBattery;
	    sLog.out ("Power: ", lastBattery ? "running on battery" : "on external power");
	}

	const int minutes = this->m_idleMinutes;
	bool waylandIdle = false;

#ifdef ENABLE_WAYLAND
	waylandIdle = sources.display != nullptr && sources.seat != nullptr && sources.notifier != nullptr;

	if (waylandIdle && minutes != appliedMinutes) {
	    if (sources.notification != nullptr) {
		ext_idle_notification_v1_destroy (sources.notification);
		sources.notification = nullptr;
	    }

	    this->m_idle = false;

	    if (minutes > 0) {
		const uint64_t timeout = static_cast<uint64_t> (minutes) * 60000u;

		sources.notification = ext_idle_notifier_v1_get_idle_notification (
		    sources.notifier, static_cast<uint32_t> (std::min<uint64_t> (timeout, UINT32_MAX)), sources.seat
		);
		ext_idle_notification_v1_add_listener (sources.notification, &notificationListener, &this->m_idle);
	    }

	    wl_display_flush (sources.display);
	}
#endif /* ENABLE_WAYLAND */

	auto wait = std::chrono::duration_cast<std::chrono::milliseconds> (nextBatteryCheck - Clock::now ());

	if (!waylandIdle && minutes > 0) {
	    std::optional<uint64_t> idleMs;

	    if (!sources.x11Tried) {
		sources.x11Tried = true;
		sources.x11 = std::make_unique<X11Idle> ();

		if (const char* session = std::getenv ("XDG_SESSION_TYPE");
		    session == nullptr || std::string_view (session) != "x11" || !sources.x11->open ()) {
		    sources.x11.reset ();
		}
	    }

	    if (sources.x11 != nullptr) {
		idleMs = sources.x11->idleMilliseconds ();
	    }

	    if (!idleMs.has_value () && sources.mutterWorks) {
		if (sources.sessionBus == nullptr) {
		    sources.sessionBus = privateBus (DBUS_BUS_SESSION);
		}

		idleMs = sources.sessionBus != nullptr ? mutterIdleMilliseconds (sources.sessionBus) : std::nullopt;
		sources.mutterWorks = idleMs.has_value ();
	    }

	    if (idleMs.has_value ()) {
		const uint64_t timeout = static_cast<uint64_t> (minutes) * 60000u;
		this->m_idle = *idleMs >= timeout;
		const auto untilIdle = std::chrono::milliseconds (*idleMs >= timeout ? 0 : timeout - *idleMs);
		wait = std::min (wait, this->m_idle ? IDLE_POLL_ACTIVE : std::min (untilIdle, IDLE_POLL_MAX));
	    } else if (!sources.loggedUnavailable) {
		sources.loggedUnavailable = true;
		sLog.error (
		    "Power: --idle-pause has no idle source here (ext-idle-notify-v1, X11 screen saver or Mutter)"
		);
	    }
	} else if (!waylandIdle) {
	    this->m_idle = false;
	}

	appliedMinutes = minutes;

	pollfd fds[2] = {
	    { .fd = this->m_wakeFd, .events = POLLIN, .revents = 0 },
	    { .fd = -1, .events = POLLIN, .revents = 0 },
	};
	nfds_t count = 1;

#ifdef ENABLE_WAYLAND
	if (sources.display != nullptr) {
	    while (wl_display_prepare_read (sources.display) != 0) {
		wl_display_dispatch_pending (sources.display);
	    }

	    wl_display_flush (sources.display);
	    fds[1].fd = wl_display_get_fd (sources.display);
	    count = 2;
	}
#endif /* ENABLE_WAYLAND */

	poll (fds, count, static_cast<int> (std::max<int64_t> (0, wait.count ())));

#ifdef ENABLE_WAYLAND
	if (sources.display != nullptr) {
	    if (fds[1].revents & POLLIN) {
		wl_display_read_events (sources.display);
	    } else {
		wl_display_cancel_read (sources.display);
	    }

	    if (fds[1].revents & (POLLERR | POLLHUP) || wl_display_dispatch_pending (sources.display) < 0) {
		sLog.error ("Power: lost the Wayland connection, idle detection stops");
		this->m_idle = false;
		sources.disconnectWayland ();
	    }
	}
#endif /* ENABLE_WAYLAND */

	if (fds[0].revents & POLLIN) {
	    uint64_t value;
	    std::ignore = read (this->m_wakeFd, &value, sizeof (value));
	}
    }
}
