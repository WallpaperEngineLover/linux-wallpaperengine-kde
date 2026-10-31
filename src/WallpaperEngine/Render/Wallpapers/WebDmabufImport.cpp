#include "WebDmabufImport.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdlib>
#include <string_view>
#include <sys/stat.h>

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render::Wallpapers;
using WallpaperEngine::WebBrowser::IPC::DmabufFrame;

namespace {
using ImageTargetTexture = void (*) (GLenum target, void* image);

constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffull;
// the browser cycles through a handful of buffers, and every imported one pins a frame of GPU memory
constexpr size_t MAX_ENTRIES = 6;

bool hasExtension (const char* list, std::string_view name) {
    if (list == nullptr) {
	return false;
    }

    const std::string_view all (list);

    for (size_t at = all.find (name); at != std::string_view::npos; at = all.find (name, at + 1)) {
	const bool startsWord = at == 0 || all[at - 1] == ' ';
	const bool endsWord = at + name.size () == all.size () || all[at + name.size ()] == ' ';

	if (startsWord && endsWord) {
	    return true;
	}
    }

    return false;
}

struct Functions {
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    ImageTargetTexture targetTexture = nullptr;
    bool modifiers = false;
};

const Functions& functions () {
    static const Functions loaded = [] {
	Functions result;
	const EGLDisplay display = eglGetCurrentDisplay ();

	if (display == EGL_NO_DISPLAY) {
	    return result;
	}

	const char* eglExtensions = eglQueryString (display, EGL_EXTENSIONS);

	if (!hasExtension (eglExtensions, "EGL_EXT_image_dma_buf_import")
	    || !hasExtension (eglExtensions, "EGL_KHR_image_base")) {
	    return result;
	}

	result.createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC> (eglGetProcAddress ("eglCreateImageKHR"));
	result.destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC> (eglGetProcAddress ("eglDestroyImageKHR"));
	result.targetTexture
	    = reinterpret_cast<ImageTargetTexture> (eglGetProcAddress ("glEGLImageTargetTexture2DOES"));
	result.modifiers = hasExtension (eglExtensions, "EGL_EXT_image_dma_buf_import_modifiers");

	bool glImport = false;
	GLint extensionCount = 0;
	glGetIntegerv (GL_NUM_EXTENSIONS, &extensionCount);

	for (GLint i = 0; i < extensionCount && !glImport; i++) {
	    const auto* name = reinterpret_cast<const char*> (glGetStringi (GL_EXTENSIONS, static_cast<GLuint> (i)));
	    glImport = name != nullptr && std::string_view (name) == "GL_OES_EGL_image";
	}

	if (result.createImage == nullptr || result.destroyImage == nullptr || result.targetTexture == nullptr
	    || !glImport) {
	    result = {};
	}

	return result;
    }();

    return loaded;
}
}

struct WebDmabufImport::Entry {
    dev_t device = 0;
    ino_t inode = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    uint32_t stride = 0;
    uint64_t offset = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};

bool WebDmabufImport::available () {
    const auto& loaded = functions ();

    return loaded.createImage != nullptr && eglGetCurrentDisplay () != EGL_NO_DISPLAY;
}

WebDmabufImport::WebDmabufImport () : m_display (eglGetCurrentDisplay ()) {
    glGenFramebuffers (1, &this->m_readFramebuffer);
}

WebDmabufImport::~WebDmabufImport () {
    for (auto& entry : this->m_entries) {
	glDeleteTextures (1, &entry->texture);
	functions ().destroyImage (this->m_display, entry->image);
    }

    glDeleteFramebuffers (1, &this->m_readFramebuffer);

    if (this->m_gridFramebuffer != 0) {
	glDeleteFramebuffers (1, &this->m_gridFramebuffer);
	glDeleteTextures (1, &this->m_gridTexture);
    }
}

bool WebDmabufImport::framebufferHasContent (const GLuint framebuffer, const uint32_t width, const uint32_t height) {
    constexpr int columns = 48;
    constexpr int rows = 27;
    constexpr int tolerance = 2;

    if (this->m_gridFramebuffer == 0) {
	GLint previous = 0;
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &previous);
	glGenTextures (1, &this->m_gridTexture);
	glBindTexture (GL_TEXTURE_2D, this->m_gridTexture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, columns, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture (GL_TEXTURE_2D, previous);
	glGenFramebuffers (1, &this->m_gridFramebuffer);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_gridFramebuffer);
	glFramebufferTexture2D (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_gridTexture, 0);
    }

    GLint previousRead = 0;
    GLint previousDraw = 0;
    GLboolean colorMask[4];
    const GLboolean scissor = glIsEnabled (GL_SCISSOR_TEST);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);

    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, framebuffer);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_gridFramebuffer);
    glBlitFramebuffer (
	0, 0, static_cast<GLint> (width), static_cast<GLint> (height), 0, 0, columns, rows, GL_COLOR_BUFFER_BIT,
	GL_NEAREST
    );

    uint8_t pixels[columns * rows * 4];
    glBindFramebuffer (GL_READ_FRAMEBUFFER, this->m_gridFramebuffer);
    glReadPixels (0, 0, columns, rows, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);

    if (scissor) {
	glEnable (GL_SCISSOR_TEST);
    }

    glBindFramebuffer (GL_READ_FRAMEBUFFER, static_cast<GLuint> (previousRead));
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, static_cast<GLuint> (previousDraw));

    for (int i = 1; i < columns * rows; i++) {
	for (int channel = 0; channel < 4; channel++) {
	    if (std::abs (pixels[i * 4 + channel] - pixels[channel]) > tolerance) {
		return true;
	    }
	}
    }

    return false;
}

WebDmabufImport::Entry* WebDmabufImport::find (const DmabufFrame& frame) {
    struct stat info {};

    if (fstat (frame.fd[0], &info) != 0) {
	return nullptr;
    }

    for (auto it = this->m_entries.begin (); it != this->m_entries.end (); ++it) {
	const Entry& entry = **it;

	if (entry.device == info.st_dev && entry.inode == info.st_ino && entry.width == frame.width
	    && entry.height == frame.height && entry.fourcc == frame.fourcc && entry.modifier == frame.modifier
	    && entry.stride == frame.stride[0] && entry.offset == frame.offset[0]) {
	    // most recently used first
	    this->m_entries.splice (this->m_entries.begin (), this->m_entries, it);
	    return this->m_entries.front ().get ();
	}
    }

    return nullptr;
}

WebDmabufImport::Entry* WebDmabufImport::import (const DmabufFrame& frame) {
    const auto& loaded = functions ();
    struct stat info {};

    if (loaded.createImage == nullptr || fstat (frame.fd[0], &info) != 0) {
	return nullptr;
    }

    constexpr EGLint planeAttributes[4][5] = {
	{ EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
	  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
	{ EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
	  EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
	{ EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
	  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
	{ EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
	  EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
    };

    EGLint attributes[7 + 4 * 10 + 1];
    int count = 0;
    const auto add = [&] (EGLint name, EGLint value) {
	attributes[count++] = name;
	attributes[count++] = value;
    };

    add (EGL_WIDTH, static_cast<EGLint> (frame.width));
    add (EGL_HEIGHT, static_cast<EGLint> (frame.height));
    add (EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint> (frame.fourcc));

    for (int i = 0; i < frame.planeCount; i++) {
	add (planeAttributes[i][0], frame.fd[i]);
	add (planeAttributes[i][1], static_cast<EGLint> (frame.offset[i]));
	add (planeAttributes[i][2], static_cast<EGLint> (frame.stride[i]));

	if (loaded.modifiers && frame.modifier != DRM_FORMAT_MOD_INVALID) {
	    add (planeAttributes[i][3], static_cast<EGLint> (frame.modifier & 0xffffffffu));
	    add (planeAttributes[i][4], static_cast<EGLint> (frame.modifier >> 32));
	}
    }

    attributes[count] = EGL_NONE;

    const EGLImageKHR image
	= loaded.createImage (this->m_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);

    if (image == EGL_NO_IMAGE_KHR) {
	if (!this->m_errorLogged) {
	    this->m_errorLogged = true;
	    sLog.error (
		"WebDmabufImport: eglCreateImage failed (EGL error 0x", std::hex, eglGetError (), std::dec, ") for a ",
		frame.width, "x", frame.height, " buffer, fourcc 0x", std::hex, frame.fourcc, ", modifier 0x",
		frame.modifier, std::dec
	    );
	}

	return nullptr;
    }

    // buffers of another size are a resize behind us, nothing sends those again
    for (auto it = this->m_entries.begin (); it != this->m_entries.end ();) {
	if ((*it)->width != frame.width || (*it)->height != frame.height) {
	    glDeleteTextures (1, &(*it)->texture);
	    loaded.destroyImage (this->m_display, (*it)->image);
	    it = this->m_entries.erase (it);
	} else {
	    ++it;
	}
    }

    auto entry = std::make_unique<Entry> ();
    entry->device = info.st_dev;
    entry->inode = info.st_ino;
    entry->width = frame.width;
    entry->height = frame.height;
    entry->fourcc = frame.fourcc;
    entry->modifier = frame.modifier;
    entry->stride = frame.stride[0];
    entry->offset = frame.offset[0];
    entry->image = image;

    GLint previous = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &previous);
    glGenTextures (1, &entry->texture);
    glBindTexture (GL_TEXTURE_2D, entry->texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    while (glGetError () != GL_NO_ERROR) { }

    loaded.targetTexture (GL_TEXTURE_2D, image);
    const GLenum error = glGetError ();
    glBindTexture (GL_TEXTURE_2D, previous);

    if (error != GL_NO_ERROR) {
	if (!this->m_errorLogged) {
	    this->m_errorLogged = true;
	    sLog.error (
		"WebDmabufImport: the GL driver refused the imported buffer (GL error 0x", std::hex, error, ")"
	    );
	}

	glDeleteTextures (1, &entry->texture);
	loaded.destroyImage (this->m_display, image);
	return nullptr;
    }

    this->m_imports++;
    this->m_entries.push_front (std::move (entry));

    if (this->m_entries.size () > MAX_ENTRIES) {
	const Entry& oldest = *this->m_entries.back ();
	glDeleteTextures (1, &oldest.texture);
	loaded.destroyImage (this->m_display, oldest.image);
	this->m_entries.pop_back ();
    }

    return this->m_entries.front ().get ();
}

bool WebDmabufImport::copyTo (const DmabufFrame& frame, const GLuint framebuffer) {
    if (frame.planeCount < 1 || frame.fd[0] < 0) {
	return false;
    }

    Entry* entry = this->find (frame);

    if (entry == nullptr) {
	entry = this->import (frame);
    }

    if (entry == nullptr) {
	return false;
    }

    GLint previousRead = 0;
    GLint previousDraw = 0;
    GLboolean colorMask[4];
    const GLboolean scissor = glIsEnabled (GL_SCISSOR_TEST);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);

    glBindFramebuffer (GL_READ_FRAMEBUFFER, this->m_readFramebuffer);
    glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, entry->texture, 0);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, framebuffer);
    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    while (glGetError () != GL_NO_ERROR) { }

    const bool complete = glCheckFramebufferStatus (GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (complete) {
	const GLint width = static_cast<GLint> (frame.width);
	const GLint height = static_cast<GLint> (frame.height);

	glBlitFramebuffer (0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    const bool failed = !complete || glGetError () != GL_NO_ERROR;

    glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);

    if (scissor) {
	glEnable (GL_SCISSOR_TEST);
    }

    glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, static_cast<GLuint> (previousRead));
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, static_cast<GLuint> (previousDraw));

    if (failed && !this->m_errorLogged) {
	this->m_errorLogged = true;
	sLog.error ("WebDmabufImport: copying the imported buffer into the frame buffer failed");
    }

    return !failed;
}
