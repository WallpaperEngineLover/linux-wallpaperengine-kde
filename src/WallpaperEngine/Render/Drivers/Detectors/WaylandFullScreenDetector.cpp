#include "WaylandFullScreenDetector.h"
#include "GnomeFullScreenDetector.h"

#ifdef ENABLE_KDE_FEATURES
#include "KDEWaylandFullScreenDetector.h"
#include "WallpaperEngine/Desktop/KDESession.h"
#endif

#include "WallpaperEngine/Desktop/HostWayland.h"
#include "WallpaperEngine/Desktop/WindowState.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/VideoFactories.h"
#include "WallpaperEngine/Render/Drivers/WaylandOpenGLDriver.h"
#include "cosmic-toplevel-info-unstable-v1-protocol.h"
#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"
#include <algorithm>
#include <cstring>
#include <wayland-client.h>

namespace WallpaperEngine::Render::Drivers::Detectors {

namespace {

    using Output = WaylandFullScreenDetector::Output;
    using Toplevel = WaylandFullScreenDetector::Toplevel;

    void
    outputGeometry (void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {
    }

    void outputMode (void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) { }

    void outputDone (void*, wl_output*) { }

    void outputScale (void*, wl_output*, int32_t) { }

    void outputName (void* data, wl_output*, const char* name) {
	if (name) {
	    static_cast<Output*> (data)->name = name;
	}
    }

    void outputDescription (void*, wl_output*, const char*) { }

    constexpr wl_output_listener outputListener = {
	.geometry = outputGeometry,
	.mode = outputMode,
	.done = outputDone,
	.scale = outputScale,
	.name = outputName,
	.description = outputDescription,
    };

    void toplevelTitle (void*, zwlr_foreign_toplevel_handle_v1*, const char*) { }

    void toplevelAppId (void* data, zwlr_foreign_toplevel_handle_v1*, const char* appId) {
	if (appId) {
	    static_cast<Toplevel*> (data)->appId = appId;
	}
    }

    void toplevelOutputEnter (void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* output) {
	static_cast<Toplevel*> (data)->outputs.insert (output);
    }

    void toplevelOutputLeave (void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* output) {
	static_cast<Toplevel*> (data)->outputs.erase (output);
    }

    void toplevelState (void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* state) {
	const auto toplevel = static_cast<Toplevel*> (data);
	const auto begin = static_cast<uint32_t*> (state->data);

	toplevel->fullscreen = false;
	toplevel->maximized = false;
	toplevel->activated = false;

	for (auto it = begin; it < begin + state->size / sizeof (uint32_t); ++it) {
	    switch (*it) {
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
		    toplevel->fullscreen = true;
		    break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
		    toplevel->maximized = true;
		    break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
		    toplevel->activated = true;
		    break;
		default:
		    break;
	    }
	}
    }

    void toplevelDone (void*, zwlr_foreign_toplevel_handle_v1*) { }

    void toplevelClosed (void* data, zwlr_foreign_toplevel_handle_v1*) {
	const auto toplevel = static_cast<Toplevel*> (data);
	toplevel->detector->removeToplevel (toplevel);
    }

    void toplevelParent (void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) { }

    constexpr zwlr_foreign_toplevel_handle_v1_listener toplevelListener = {
	.title = toplevelTitle,
	.app_id = toplevelAppId,
	.output_enter = toplevelOutputEnter,
	.output_leave = toplevelOutputLeave,
	.state = toplevelState,
	.done = toplevelDone,
	.closed = toplevelClosed,
	.parent = toplevelParent,
    };

    void managerToplevel (void* data, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* handle) {
	static_cast<WaylandFullScreenDetector*> (data)->addToplevel (handle);
    }

    void managerFinished (void*, zwlr_foreign_toplevel_manager_v1* manager) {
	zwlr_foreign_toplevel_manager_v1_destroy (manager);
    }

    constexpr zwlr_foreign_toplevel_manager_v1_listener managerListener = {
	.toplevel = managerToplevel,
	.finished = managerFinished,
    };

    // COSMIC: the ext handle names the window (app id, closed), the cosmic extension object made from it has the outputs
    // and the state
    void foreignClosed (void* data, ext_foreign_toplevel_handle_v1*) {
	const auto toplevel = static_cast<Toplevel*> (data);
	toplevel->detector->removeToplevel (toplevel);
    }

    void foreignDone (void*, ext_foreign_toplevel_handle_v1*) { }

    void foreignTitle (void*, ext_foreign_toplevel_handle_v1*, const char*) { }

    void foreignAppId (void* data, ext_foreign_toplevel_handle_v1*, const char* appId) {
	if (appId) {
	    static_cast<Toplevel*> (data)->appId = appId;
	}
    }

    void foreignIdentifier (void*, ext_foreign_toplevel_handle_v1*, const char*) { }

    constexpr ext_foreign_toplevel_handle_v1_listener foreignHandleListener = {
	.closed = foreignClosed,
	.done = foreignDone,
	.title = foreignTitle,
	.app_id = foreignAppId,
	.identifier = foreignIdentifier,
    };

    void cosmicClosed (void*, zcosmic_toplevel_handle_v1*) { }

    void cosmicDone (void*, zcosmic_toplevel_handle_v1*) { }

    void cosmicTitle (void*, zcosmic_toplevel_handle_v1*, const char*) { }

    void cosmicAppId (void*, zcosmic_toplevel_handle_v1*, const char*) { }

    void cosmicOutputEnter (void* data, zcosmic_toplevel_handle_v1*, wl_output* output) {
	static_cast<Toplevel*> (data)->outputs.insert (output);
    }

    void cosmicOutputLeave (void* data, zcosmic_toplevel_handle_v1*, wl_output* output) {
	static_cast<Toplevel*> (data)->outputs.erase (output);
    }

    void cosmicWorkspace (void*, zcosmic_toplevel_handle_v1*, zcosmic_workspace_handle_v1*) { }

    void cosmicState (void* data, zcosmic_toplevel_handle_v1*, wl_array* state) {
	const auto toplevel = static_cast<Toplevel*> (data);
	const auto begin = static_cast<uint32_t*> (state->data);

	toplevel->fullscreen = false;
	toplevel->maximized = false;
	toplevel->activated = false;
	toplevel->minimized = false;

	for (auto it = begin; it < begin + state->size / sizeof (uint32_t); ++it) {
	    switch (*it) {
		case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
		    toplevel->fullscreen = true;
		    break;
		case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
		    toplevel->maximized = true;
		    break;
		case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
		    toplevel->activated = true;
		    break;
		case ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
		    toplevel->minimized = true;
		    break;
		default:
		    break;
	    }
	}
    }

    void cosmicGeometry (void*, zcosmic_toplevel_handle_v1*, wl_output*, int32_t, int32_t, int32_t, int32_t) { }

    void cosmicExtWorkspace (void*, zcosmic_toplevel_handle_v1*, ext_workspace_handle_v1*) { }

    constexpr zcosmic_toplevel_handle_v1_listener cosmicHandleListener = {
	.closed = cosmicClosed,
	.done = cosmicDone,
	.title = cosmicTitle,
	.app_id = cosmicAppId,
	.output_enter = cosmicOutputEnter,
	.output_leave = cosmicOutputLeave,
	.workspace_enter = cosmicWorkspace,
	.workspace_leave = cosmicWorkspace,
	.state = cosmicState,
	.geometry = cosmicGeometry,
	.ext_workspace_enter = cosmicExtWorkspace,
	.ext_workspace_leave = cosmicExtWorkspace,
    };

    void foreignListToplevel (void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
	static_cast<WaylandFullScreenDetector*> (data)->addForeignToplevel (handle);
    }

    void foreignListFinished (void*, ext_foreign_toplevel_list_v1*) { }

    constexpr ext_foreign_toplevel_list_v1_listener foreignListListener = {
	.toplevel = foreignListToplevel,
	.finished = foreignListFinished,
    };

    void cosmicInfoToplevel (void*, zcosmic_toplevel_info_v1*, zcosmic_toplevel_handle_v1*) { }

    void cosmicInfoFinished (void*, zcosmic_toplevel_info_v1*) { }

    void cosmicInfoDone (void*, zcosmic_toplevel_info_v1*) { }

    constexpr zcosmic_toplevel_info_v1_listener cosmicInfoListener = {
	.toplevel = cosmicInfoToplevel,
	.finished = cosmicInfoFinished,
	.done = cosmicInfoDone,
    };

    void registryGlobal (void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
	const auto detector = static_cast<WaylandFullScreenDetector*> (data);

	if (strcmp (interface, wl_output_interface.name) == 0) {
	    detector->addOutput (registry, name, version);
	} else if (strcmp (interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
	    detector->m_managerGlobal = name;
	    detector->m_managerVersion = version;
	} else if (strcmp (interface, zcosmic_toplevel_info_v1_interface.name) == 0) {
	    detector->m_cosmicInfoGlobal = name;
	    detector->m_cosmicInfoVersion = version;
	} else if (strcmp (interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
	    detector->m_foreignListGlobal = name;
	}
    }

    void registryGlobalRemove (void* data, wl_registry*, uint32_t name) {
	static_cast<WaylandFullScreenDetector*> (data)->removeGlobal (name);
    }

    constexpr wl_registry_listener registryListener = {
	.global = registryGlobal,
	.global_remove = registryGlobalRemove,
    };

}

WaylandFullScreenDetector::WaylandFullScreenDetector (
    Application::ApplicationContext& appContext, WaylandOpenGLDriver* driver
) : FullScreenDetector (appContext), m_driver (driver) {
    m_display = Desktop::connectWayland ();
    if (!m_display) {
	sLog.exception ("Failed to query wayland display");
    }

    m_registry = wl_display_get_registry (m_display);
    wl_registry_add_listener (m_registry, &registryListener, this);
    wl_display_roundtrip (m_display);

    // COSMIC (cosmic-comp): version 2 of its toplevel info works on ext-foreign-toplevel-list handles
    if (m_managerGlobal == 0 && m_cosmicInfoGlobal != 0 && m_cosmicInfoVersion >= 2 && m_foreignListGlobal != 0) {
	m_cosmicInfo = static_cast<zcosmic_toplevel_info_v1*> (wl_registry_bind (
	    m_registry, m_cosmicInfoGlobal, &zcosmic_toplevel_info_v1_interface, std::min<uint32_t> (m_cosmicInfoVersion, 3)
	));
	zcosmic_toplevel_info_v1_add_listener (m_cosmicInfo, &cosmicInfoListener, this);
	m_foreignList = static_cast<ext_foreign_toplevel_list_v1*> (
	    wl_registry_bind (m_registry, m_foreignListGlobal, &ext_foreign_toplevel_list_v1_interface, 1)
	);
	ext_foreign_toplevel_list_v1_add_listener (m_foreignList, &foreignListListener, this);
	wl_display_roundtrip (m_display);
	wl_display_roundtrip (m_display);
	return;
    }

    if (m_managerGlobal == 0) {
	sLog.out ("Fullscreen detection not supported by your Wayland compositor");
	return;
    }

    // bound after the outputs, so the first output_enter events have a wl_output of ours to name
    m_toplevelManager = static_cast<zwlr_foreign_toplevel_manager_v1*> (wl_registry_bind (
	m_registry, m_managerGlobal, &zwlr_foreign_toplevel_manager_v1_interface,
	std::min<uint32_t> (m_managerVersion, 3)
    ));
    zwlr_foreign_toplevel_manager_v1_add_listener (m_toplevelManager, &managerListener, this);
    wl_display_roundtrip (m_display);
    wl_display_roundtrip (m_display);
}

WaylandFullScreenDetector::~WaylandFullScreenDetector () {
    for (const auto* toplevel : m_toplevels) {
	delete toplevel;
    }

    for (const auto* output : m_outputs) {
	delete output;
    }

    if (m_display) {
	wl_display_disconnect (m_display);
    }
}

void WaylandFullScreenDetector::addOutput (wl_registry* registry, uint32_t globalName, uint32_t version) {
    // wl_output.name is v4, without it the toplevels on this output count everywhere
    const auto output = new Output {
	.output = static_cast<wl_output*> (
	    wl_registry_bind (registry, globalName, &wl_output_interface, std::min<uint32_t> (version, 4))
	),
	.globalName = globalName,
	.name = {},
    };
    wl_output_add_listener (output->output, &outputListener, output);
    m_outputs.push_back (output);
}

void WaylandFullScreenDetector::removeGlobal (uint32_t globalName) {
    const auto it = std::ranges::find (m_outputs, globalName, &Output::globalName);

    if (it == m_outputs.end ()) {
	return;
    }

    for (auto* toplevel : m_toplevels) {
	toplevel->outputs.erase ((*it)->output);
    }

    if (wl_proxy_get_version (reinterpret_cast<wl_proxy*> ((*it)->output)) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
	wl_output_release ((*it)->output);
    } else {
	wl_output_destroy ((*it)->output);
    }

    delete *it;
    m_outputs.erase (it);
}

void WaylandFullScreenDetector::addToplevel (zwlr_foreign_toplevel_handle_v1* handle) {
    const auto toplevel = new Toplevel { .detector = this, .handle = handle };
    zwlr_foreign_toplevel_handle_v1_add_listener (handle, &toplevelListener, toplevel);
    m_toplevels.push_back (toplevel);
}

void WaylandFullScreenDetector::addForeignToplevel (ext_foreign_toplevel_handle_v1* handle) {
    const auto toplevel = new Toplevel { .detector = this, .foreign = handle };
    ext_foreign_toplevel_handle_v1_add_listener (handle, &foreignHandleListener, toplevel);
    toplevel->cosmic = zcosmic_toplevel_info_v1_get_cosmic_toplevel (m_cosmicInfo, handle);
    zcosmic_toplevel_handle_v1_add_listener (toplevel->cosmic, &cosmicHandleListener, toplevel);
    m_toplevels.push_back (toplevel);
}

void WaylandFullScreenDetector::removeToplevel (Toplevel* toplevel) {
    if (toplevel->handle) {
	zwlr_foreign_toplevel_handle_v1_destroy (toplevel->handle);
    }

    if (toplevel->cosmic) {
	zcosmic_toplevel_handle_v1_destroy (toplevel->cosmic);
    }

    if (toplevel->foreign) {
	ext_foreign_toplevel_handle_v1_destroy (toplevel->foreign);
    }

    std::erase (m_toplevels, toplevel);
    delete toplevel;
}

const std::string* WaylandFullScreenDetector::outputName (wl_output* output) const {
    const auto it = std::ranges::find (m_outputs, output, &Output::output);

    if (it == m_outputs.end () || (*it)->name.empty ()) {
	return nullptr;
    }

    return &(*it)->name;
}

std::vector<Desktop::WindowState> WaylandFullScreenDetector::windows (const std::vector<std::string>& drawn) const {
    const bool maximizedPauses = getApplicationContext ().settings.render.pauseOnMaximized;
    std::vector<Desktop::WindowState> windows;

    for (const auto* toplevel : m_toplevels) {
	const uint32_t flags = (toplevel->activated ? Desktop::WindowState::Focused : 0)
	    | (toplevel->maximized && maximizedPauses ? Desktop::WindowState::Maximized : 0)
	    | (toplevel->fullscreen ? Desktop::WindowState::Fullscreen : 0);

	if (flags == 0 || toplevel->minimized) {
	    continue;
	}

	bool placed = false;

	for (auto* output : toplevel->outputs) {
	    if (const auto* name = outputName (output)) {
		windows.push_back ({ toplevel->appId, *name, flags });
		placed = true;
	    }
	}

	if (!placed) {
	    for (const auto& name : drawn) {
		windows.push_back ({ toplevel->appId, name, flags });
	    }
	}
    }

    return windows;
}

bool WaylandFullScreenDetector::anythingFullscreen () const {
    if (!m_toplevelManager && !m_cosmicInfo) {
	return false;
    }

    wl_display_roundtrip (m_display);

    // a driver that isn't ours has no outputs to tell apart, every window counts there: each named output plus "",
    // where windows on no named output land
    std::vector<std::string> drawn { "" };

    if (m_driver) {
	drawn = m_driver->drawnOutputs ();
    } else {
	for (const auto* output : m_outputs) {
	    if (!output->name.empty ()) {
		drawn.push_back (output->name);
	    }
	}
    }

    return Desktop::pausesAnyOf (windows (drawn), drawn, getApplicationContext ());
}

void WaylandFullScreenDetector::reset () { }

__attribute__ ((constructor)) void registerWaylandFullscreenDetector () {
    sVideoFactories.registerFullscreenDetector (
	"wayland", [] (ApplicationContext& context, VideoDriver& driver) -> std::unique_ptr<FullScreenDetector> {
	    auto* wayland = dynamic_cast<WaylandOpenGLDriver*> (&driver);

	    if (wayland && wayland->getGnomeShell ()) {
		return std::make_unique<GnomeFullScreenDetector> (context, *wayland);
	    }
#ifdef ENABLE_KDE_FEATURES
	    if (wayland && WallpaperEngine::Desktop::isKDESession ()) {
		auto detector = std::make_unique<KDEWaylandFullScreenDetector> (context, *wayland);
		if (!detector->isInitialized ()) {
		    return std::make_unique<WaylandFullScreenDetector> (context, wayland);
		}
		return detector;
	    }
#endif
	    return std::make_unique<WaylandFullScreenDetector> (context, wayland);
	}
    );
}

}
