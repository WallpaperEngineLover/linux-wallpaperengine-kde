#include "WaylandOutput.h"
#include "../WaylandOpenGLDriver.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"

using namespace WallpaperEngine::Render::Drivers::Output;

WaylandOutput::WaylandOutput (ApplicationContext& context, WaylandOpenGLDriver& driver) : Output (context, driver) {
    updateViewports ();
}

void WaylandOutput::updateViewports () {
    m_viewports.clear ();
    const auto PDRIVER = dynamic_cast<WaylandOpenGLDriver*> (&m_driver);
    glm::ivec2 fullw = { 0, 0 };
    for (const auto& o : PDRIVER->m_screens) {
	if (!o->hasShellSurface ()) {
	    continue;
	}

	m_viewports[o->name] = o;

	const glm::ivec2 buffer = o->bufferSize ();

	fullw = fullw + glm::ivec2 { buffer.x, 0 };
	if (buffer.y > fullw.y) {
	    fullw.y = buffer.y;
	}
    }

    m_fullWidth = fullw.x;
    m_fullHeight = fullw.y;
}

void WaylandOutput::reset () { updateViewports (); }

bool WaylandOutput::renderVFlip () const { return true; }

bool WaylandOutput::renderMultiple () const {
    return false; // todo
}

bool WaylandOutput::haveImageBuffer () const { return false; }

void* WaylandOutput::getImageBuffer () const { return nullptr; }

uint32_t WaylandOutput::getImageBufferSize () const { return 0; }

void WaylandOutput::updateRender () const { }