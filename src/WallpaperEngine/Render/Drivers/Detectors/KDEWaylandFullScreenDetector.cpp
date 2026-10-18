#include "KDEWaylandFullScreenDetector.h"

#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/WaylandOpenGLDriver.h"

#include <nlohmann/json.hpp>
#include <unistd.h>

namespace WallpaperEngine::Render::Drivers::Detectors {

KDEWaylandFullScreenDetector::KDEWaylandFullScreenDetector (
    Application::ApplicationContext& appContext, WaylandOpenGLDriver& driver
) : FullScreenDetector (appContext), m_driver (driver) {
    if (!initializeDBus ()) {
	sLog.out ("KDE window detection could not set up its D-Bus service, falling back to the Wayland detector");
	return;
    }

    this->m_script = std::make_unique<Desktop::KWinScript> (
	this->m_connection, "lwe-window-states-" + Desktop::processTag (), this->scriptSource ()
    );

    if (!this->m_script->isLoaded ()) {
	sLog.out ("KDE window detection could not load its KWin script, falling back to the Wayland detector");
	this->m_script.reset ();
    }
}

KDEWaylandFullScreenDetector::~KDEWaylandFullScreenDetector () {
    this->m_script.reset ();

    if (this->m_connection != nullptr) {
	dbus_connection_unregister_object_path (this->m_connection, kObjectPath);
	dbus_connection_close (this->m_connection);
	dbus_connection_unref (this->m_connection);
    }
}

bool KDEWaylandFullScreenDetector::initializeDBus () {
    DBusError error;
    dbus_error_init (&error);

    // private: we_manager runs one engine per screen, each with its own service
    this->m_connection = dbus_bus_get_private (DBUS_BUS_SESSION, &error);

    if (dbus_error_is_set (&error)) {
	sLog.error ("Failed to connect to the session D-Bus: ", error.message);
	dbus_error_free (&error);
	this->m_connection = nullptr;
	return false;
    }

    dbus_connection_set_exit_on_disconnect (this->m_connection, false);
    this->m_serviceName = std::string (kServiceName) + "_" + Desktop::processTag ();

    dbus_error_init (&error);
    const int requested
	= dbus_bus_request_name (this->m_connection, this->m_serviceName.c_str (), DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);

    if (dbus_error_is_set (&error) || requested != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
	sLog.error (
	    "Could not own the D-Bus name ", this->m_serviceName, dbus_error_is_set (&error) ? ": " : "",
	    dbus_error_is_set (&error) ? error.message : ""
	);
	dbus_error_free (&error);
	dbus_connection_close (this->m_connection);
	dbus_connection_unref (this->m_connection);
	this->m_connection = nullptr;
	return false;
    }

    static constexpr DBusObjectPathVTable vtable = {
	.unregister_function = nullptr,
	.message_function = &KDEWaylandFullScreenDetector::handleMessage,
    };

    return dbus_connection_register_object_path (this->m_connection, kObjectPath, &vtable, this);
}

// WE 2.8 rules (sub_14003D050 / sub_14003BBA0), same as the GNOME extension: visible, unminimized windows on the
// current desktop; maximized marks its output, the active root window marks focused, and fullscreen if not
// maximized and at least the output's size. Show desktop and other activities hide windows. KWin 5: UNSUPPORTED
std::string KDEWaylandFullScreenDetector::scriptSource () const {
    const std::string service = this->m_serviceName;

    return "const service = '" + service + "';\n" + "const objectPath = '" + kObjectPath + "';\n" + "const method = '"
	+ kMethodName + "';\n" + "const enginePid = " + std::to_string (getpid ()) + ";\n" + "const unsupported = '"
	+ kUnsupported + "';\n" + R"(
const MAXIMIZE_FULL = 3;
let last = '';
// Plasma 6.7 gives scripts hiddenByShowDesktopChanged but not the property, so it is kept from the signal, which
// fires on every change (show desktop on, then off again)
const hiddenByShowDesktop = new Set();

function isHiddenByShowDesktop(window) {
    if (typeof window.hiddenByShowDesktop === 'boolean')
        return window.hiddenByShowDesktop;
    return hiddenByShowDesktop.has(String(window.internalId));
}

function rootOf(window) {
    while (window && window.transient && window.transientFor)
        window = window.transientFor;
    return window;
}

function counts(window) {
    if (!window || window.deleted || !window.managed || window.minimized || isHiddenByShowDesktop(window) ||
        window.pid === enginePid)
        return false;
    if (window.specialWindow || window.popupWindow || window.desktopWindow || window.dock)
        return false;
    if (!window.normalWindow && !window.dialog)
        return false;
    if (window.activities.length > 0 && window.activities.indexOf(workspace.currentActivity) < 0)
        return false;
    return window.onAllDesktops || window.desktops.indexOf(workspace.currentDesktop) >= 0;
}

function report() {
    const focus = rootOf(workspace.activeWindow);
    const windows = [];

    for (const window of workspace.windowList()) {
        if (!counts(window))
            continue;

        const maximized = window.maximizeMode === MAXIMIZE_FULL;
        let flags = maximized ? 2 : 0;

        if (window === focus) {
            flags |= 1;
            const area = window.output ? window.output.geometry : null;
            const frame = window.frameGeometry;
            if (!maximized && (window.fullScreen || (area && frame.width >= area.width && frame.height >= area.height)))
                flags |= 4;
        }

        if (flags !== 0)
            windows.push([window.desktopFileName || window.resourceClass || '', window.output ? window.output.name : '', flags]);
    }

    const json = JSON.stringify(windows);
    if (json === last)
        return;
    last = json;
    callDBus(service, objectPath, service, method, json);
}

function watch(window) {
    window.fullScreenChanged.connect(report);
    window.maximizedChanged.connect(report);
    window.minimizedChanged.connect(report);
    window.outputChanged.connect(report);
    window.desktopsChanged.connect(report);
    window.activitiesChanged.connect(report);
    // connecting a missing signal kills the script
    if (window.hiddenByShowDesktopChanged) {
        window.hiddenByShowDesktopChanged.connect(() => {
            const id = String(window.internalId);
            if (hiddenByShowDesktop.has(id))
                hiddenByShowDesktop.delete(id);
            else
                hiddenByShowDesktop.add(id);
            report();
        });
    }
    // fires every frame while moving, only the active window's size matters
    window.frameGeometryChanged.connect(() => {
        if (window === rootOf(workspace.activeWindow))
            report();
    });
}

if (typeof workspace.windowList === 'function') {
    workspace.windowList().forEach(watch);
    workspace.windowAdded.connect(window => {
        watch(window);
        report();
    });
    workspace.windowRemoved.connect(window => {
        hiddenByShowDesktop.delete(String(window.internalId));
        report();
    });
    // activating a window ends show desktop, which also mends a state that started inverted (shown at load)
    workspace.windowActivated.connect(window => {
        if (window)
            hiddenByShowDesktop.delete(String(window.internalId));
        report();
    });
    workspace.currentDesktopChanged.connect(report);
    workspace.currentActivityChanged.connect(report);
    report();
} else {
    callDBus(service, objectPath, service, method, unsupported);
}
)";
}

DBusHandlerResult
KDEWaylandFullScreenDetector::handleMessage (DBusConnection* connection, DBusMessage* message, void* userData) {
    return static_cast<KDEWaylandFullScreenDetector*> (userData)->handleMessage (message);
}

DBusHandlerResult KDEWaylandFullScreenDetector::handleMessage (DBusMessage* message) {
    if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_METHOD_CALL
	|| !dbus_message_has_member (message, kMethodName)) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* json = nullptr;

    if (dbus_message_get_args (message, nullptr, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID)) {
	this->applyWindows (json);
    }

    // callDBus doesn't wait, this only silences the missing-reply log
    if (DBusMessage* reply = dbus_message_new_method_return (message); reply != nullptr) {
	dbus_connection_send (this->m_connection, reply, nullptr);
	dbus_message_unref (reply);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

bool KDEWaylandFullScreenDetector::applyWindows (const std::string& json) {
    if (json == kUnsupported) {
	sLog.error (
	    "KDE window detection needs Plasma 6, wallpapers won't pause for maximized, fullscreen or focused windows"
	);
	this->m_windows.clear ();
	return false;
    }

    const auto parsed = nlohmann::json::parse (json, nullptr, false);

    if (!parsed.is_array ()) {
	sLog.error ("KDE window detection: the KWin script sent ", json);
	return false;
    }

    std::vector<Desktop::WindowState> windows;

    for (const auto& entry : parsed) {
	if (!entry.is_array () || entry.size () != 3 || !entry[0].is_string () || !entry[1].is_string ()
	    || !entry[2].is_number_unsigned ()) {
	    continue;
	}

	windows.push_back ({ entry[0].get<std::string> (), entry[1].get<std::string> (), entry[2].get<uint32_t> () });
    }

    this->m_windows = std::move (windows);
    return true;
}

void KDEWaylandFullScreenDetector::dispatch () const {
    if (this->m_connection == nullptr || this->m_disconnected) {
	return;
    }

    // a stale state could pause forever
    if (!Desktop::drainBus (this->m_connection)) {
	sLog.error ("KDE window detection: the session bus connection dropped, pausing for windows stops");
	this->m_disconnected = true;
	this->m_windows.clear ();
    }
}

bool KDEWaylandFullScreenDetector::anythingFullscreen () const {
    this->dispatch ();
    return Desktop::pausesAnyOf (this->m_windows, this->m_driver.drawnOutputs (), this->getApplicationContext ());
}

bool KDEWaylandFullScreenDetector::isInitialized () const { return this->m_script != nullptr; }

void KDEWaylandFullScreenDetector::reset () { }

}
