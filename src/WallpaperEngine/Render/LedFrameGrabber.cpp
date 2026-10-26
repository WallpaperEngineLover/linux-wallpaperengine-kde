#include "LedFrameGrabber.h"

#include <cstring>

#include "WallpaperEngine/Led/LedLayout.h"
#include "WallpaperEngine/Logging/Log.h"

namespace WallpaperEngine::Render {
namespace {
    const char* kVertex = R"(#version 330
out vec2 v_TexCoord;

void main () {
    vec2 corner = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_TexCoord = corner;
    gl_Position = vec4 (corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

    const char* kFragment = R"(#version 330
uniform sampler2D g_Texture0;
uniform vec2 u_Texel;
in vec2 v_TexCoord;
out vec4 fragColor;

void main () {
    vec2 step = u_Texel * 2.0;
    vec4 albedo = texture (g_Texture0, v_TexCoord - step) + texture (g_Texture0, v_TexCoord + step)
	+ texture (g_Texture0, v_TexCoord + vec2 (-step.x, step.y)) + texture (g_Texture0, v_TexCoord + vec2 (step.x, -step.y));
    fragColor = vec4 (albedo.rgb * 0.25, 1.0);
}
)";

    GLuint compile (const GLenum type, const char* source) {
	const GLuint shader = glCreateShader (type);
	glShaderSource (shader, 1, &source, nullptr);
	glCompileShader (shader);

	GLint compiled = GL_FALSE;
	glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);

	if (compiled == GL_FALSE) {
	    char log[1024] = {};
	    glGetShaderInfoLog (shader, sizeof (log) - 1, nullptr, log);
	    sLog.error ("LED frame shader failed to compile: ", log);
	}

	return shader;
    }

    struct SavedState {
	GLint drawFramebuffer = 0, readFramebuffer = 0, program = 0, vao = 0, activeTexture = 0, texture = 0,
	      sampler = 0;
	GLint packBuffer = 0;
	GLint viewport[4] = {};
	GLboolean colorMask[4] = {};
	GLboolean blend = GL_FALSE, depth = GL_FALSE, cull = GL_FALSE, scissor = GL_FALSE, stencil = GL_FALSE;
	GLboolean framebufferSrgb = GL_FALSE;

	SavedState () {
	    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
	    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
	    glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &vao);
	    glGetIntegerv (GL_ACTIVE_TEXTURE, &activeTexture);
	    glActiveTexture (GL_TEXTURE0);
	    glGetIntegerv (GL_TEXTURE_BINDING_2D, &texture);
	    glGetIntegerv (GL_SAMPLER_BINDING, &sampler);
	    glGetIntegerv (GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
	    glGetIntegerv (GL_VIEWPORT, viewport);
	    glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);
	    blend = glIsEnabled (GL_BLEND);
	    depth = glIsEnabled (GL_DEPTH_TEST);
	    cull = glIsEnabled (GL_CULL_FACE);
	    scissor = glIsEnabled (GL_SCISSOR_TEST);
	    stencil = glIsEnabled (GL_STENCIL_TEST);
	    framebufferSrgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
	}

	~SavedState () {
	    const auto set = [] (const GLenum capability, const GLboolean on) {
		if (on) {
		    glEnable (capability);
		} else {
		    glDisable (capability);
		}
	    };

	    glBindBuffer (GL_PIXEL_PACK_BUFFER, packBuffer);
	    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, drawFramebuffer);
	    glBindFramebuffer (GL_READ_FRAMEBUFFER, readFramebuffer);
	    glUseProgram (program);
	    glBindVertexArray (vao);
	    glActiveTexture (GL_TEXTURE0);
	    glBindTexture (GL_TEXTURE_2D, texture);
	    glBindSampler (0, sampler);
	    glActiveTexture (activeTexture);
	    glViewport (viewport[0], viewport[1], viewport[2], viewport[3]);
	    glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
	    set (GL_BLEND, blend);
	    set (GL_DEPTH_TEST, depth);
	    set (GL_CULL_FACE, cull);
	    set (GL_SCISSOR_TEST, scissor);
	    set (GL_STENCIL_TEST, stencil);
	    set (GL_FRAMEBUFFER_SRGB, framebufferSrgb);
	}
    };
}

LedFrameGrabber::~LedFrameGrabber () {
    if (!this->m_created) {
	return;
    }

    glDeleteProgram (this->m_program);
    glDeleteTextures (1, &this->m_target);
    glDeleteFramebuffers (1, &this->m_framebuffer);
    glDeleteSamplers (1, &this->m_sampler);
    glDeleteVertexArrays (1, &this->m_vao);
    glDeleteBuffers (2, this->m_buffers);
}

void LedFrameGrabber::create () {
    this->m_created = true;

    const GLuint vertex = compile (GL_VERTEX_SHADER, kVertex);
    const GLuint fragment = compile (GL_FRAGMENT_SHADER, kFragment);

    this->m_program = glCreateProgram ();
    glAttachShader (this->m_program, vertex);
    glAttachShader (this->m_program, fragment);
    glLinkProgram (this->m_program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);
    glUseProgram (this->m_program);
    glUniform1i (glGetUniformLocation (this->m_program, "g_Texture0"), 0);
    this->m_texelLocation = glGetUniformLocation (this->m_program, "u_Texel");

    glGenTextures (1, &this->m_target);
    glBindTexture (GL_TEXTURE_2D, this->m_target);
    glTexImage2D (
	GL_TEXTURE_2D, 0, GL_RGBA8, Led::kFrameWidth, Led::kFrameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr
    );
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers (1, &this->m_framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_target, 0);

    glGenSamplers (1, &this->m_sampler);
    glSamplerParameteri (this->m_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri (this->m_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri (this->m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri (this->m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenVertexArrays (1, &this->m_vao);
    glGenBuffers (2, this->m_buffers);

    for (const GLuint buffer : this->m_buffers) {
	glBindBuffer (GL_PIXEL_PACK_BUFFER, buffer);
	glBufferData (GL_PIXEL_PACK_BUFFER, Led::kFrameWidth * Led::kFrameHeight * 4, nullptr, GL_STREAM_READ);
    }
}

bool LedFrameGrabber::grab (const GLuint texture, const int width, const int height, std::vector<uint8_t>& out) {
    if (texture == 0 || width <= 0 || height <= 0) {
	return false;
    }

    const SavedState saved;

    if (!this->m_created) {
	this->create ();
    }

    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_STENCIL_TEST);
    glDisable (GL_FRAMEBUFFER_SRGB);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    glViewport (0, 0, Led::kFrameWidth, Led::kFrameHeight);
    glUseProgram (this->m_program);
    glUniform2f (this->m_texelLocation, 1.0f / static_cast<float> (width), 1.0f / static_cast<float> (height));
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, texture);
    glBindSampler (0, this->m_sampler);
    glBindVertexArray (this->m_vao);
    glDrawArrays (GL_TRIANGLES, 0, 3);

    const int current = this->m_slot;
    const int previous = 1 - current;

    glReadBuffer (GL_COLOR_ATTACHMENT0);
    glBindBuffer (GL_PIXEL_PACK_BUFFER, this->m_buffers[current]);
    glReadPixels (0, 0, Led::kFrameWidth, Led::kFrameHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    bool produced = false;

    if (this->m_pending) {
	glBindBuffer (GL_PIXEL_PACK_BUFFER, this->m_buffers[previous]);

	if (const auto* rgba = static_cast<const uint8_t*> (glMapBuffer (GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
	    rgba != nullptr) {
	    out.resize (Led::kFrameBytes);

	    for (int row = 0; row < Led::kFrameHeight; row++) {
		const uint8_t* from = rgba + static_cast<size_t> (row) * Led::kFrameWidth * 4;
		uint8_t* to = out.data () + static_cast<size_t> (row) * Led::kFrameWidth * 3;

		for (int column = 0; column < Led::kFrameWidth; column++) {
		    to[column * 3] = from[column * 4];
		    to[column * 3 + 1] = from[column * 4 + 1];
		    to[column * 3 + 2] = from[column * 4 + 2];
		}
	    }

	    glUnmapBuffer (GL_PIXEL_PACK_BUFFER);
	    produced = true;
	}
    }

    this->m_pending = true;
    this->m_slot = previous;
    return produced;
}
}
