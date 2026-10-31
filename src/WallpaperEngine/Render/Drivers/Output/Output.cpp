#include "Output.h"

#include "WallpaperEngine/Logging/Log.h"

#include <GL/glew.h>

using namespace WallpaperEngine::Render::Drivers::Output;

Output::Output (ApplicationContext& context, VideoDriver& driver) : m_context (context), m_driver (driver) { }

const std::map<std::string, OutputViewport*>& Output::getViewports () const { return this->m_viewports; }

int Output::getFullWidth () const { return this->m_fullWidth; }

int Output::getFullHeight () const { return this->m_fullHeight; }

void Output::readImageBuffer () const {
    // glReadnPixels requires GL 4.5; older drivers fall back to glReadPixels
    if (GLEW_VERSION_4_5) {
	glReadnPixels (
	    0, 0, this->getFullWidth (), this->getFullHeight (), GL_BGRA, GL_UNSIGNED_BYTE, this->getImageBufferSize (),
	    this->getImageBuffer ()
	);
    } else {
	glReadPixels (
	    0, 0, this->getFullWidth (), this->getFullHeight (), GL_BGRA, GL_UNSIGNED_BYTE, this->getImageBuffer ()
	);
    }

    GLenum error = glGetError ();

    if (error != GL_NO_ERROR) {
	sLog.exception ("OpenGL error when reading texture ", error);
    }
}
