#include "GnomeFullScreenDetector.h"

#include "WallpaperEngine/Render/Drivers/WaylandOpenGLDriver.h"

using namespace WallpaperEngine::Render::Drivers::Detectors;

GnomeFullScreenDetector::GnomeFullScreenDetector (
    Application::ApplicationContext& appContext, WaylandOpenGLDriver& driver
) : FullScreenDetector (appContext), m_driver (driver) { }

bool GnomeFullScreenDetector::anythingFullscreen () const {
    auto* gnomeShell = this->m_driver.getGnomeShell ();

    if (gnomeShell == nullptr) {
	return false;
    }

    // while paused the driver doesn't dispatch
    gnomeShell->dispatch ();

    return Desktop::pausesAnyOf (
	gnomeShell->windows (), this->m_driver.drawnOutputs (), this->getApplicationContext ()
    );
}
