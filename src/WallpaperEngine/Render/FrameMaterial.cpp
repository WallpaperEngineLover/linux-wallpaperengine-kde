#include "FrameMaterial.h"

#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Shaders/GLSLContext.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

namespace {
// x, y, z, u, v strip of WE's fullscreen quad, v up
constexpr float kQuad[] = {
    -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
    -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f, 1.0f,
};

GLuint compile (const std::string& source, const GLenum type, const std::string& name) {
    const GLuint shader = glCreateShader (type);
    const char* text = source.c_str ();
    glShaderSource (shader, 1, &text, nullptr);
    glCompileShader (shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);

    if (compiled == GL_FALSE) {
	char log[2048] = {};
	glGetShaderInfoLog (shader, sizeof (log) - 1, nullptr, log);
	sLog.error ("Frame material ", name, " failed to compile: ", log);
    }

    return shader;
}
} // namespace

FrameMaterial::FrameMaterial (
    const CWallpaper& owner, const Project& project, const std::string& file, const ComboMap& combos
) : m_owner (owner), m_file (file) {
    this->m_material = Data::Parsers::MaterialParser::load (project, file);

    if (this->m_material == nullptr || this->m_material->passes.empty ()) {
	sLog.error ("Frame material ", file, " could not be loaded");
	return;
    }

    this->m_pass = this->m_material->passes.front ().get ();
    this->m_combos = this->m_pass->combos;

    for (const auto& [name, value] : combos) {
	this->m_combos.insert_or_assign (name, value);
    }

    // scene wide defines (sub_1401A5C40)
    if (const auto* scene = owner.is<Wallpapers::CScene> () ? owner.as<Wallpapers::CScene> () : nullptr) {
	if (scene->getCamera ().isOrthogonal ()) {
	    this->m_combos.insert_or_assign ("SCENE_ORTHO", 1);
	}

	if (scene->isHDR ()) {
	    this->m_combos.insert_or_assign ("HDR", 1);
	}
    }

    static const ShaderConstantMap noConstants;

    try {
	this->m_shader = std::make_unique<Shaders::Shader> (
	    *project.assetLocator, this->m_pass->shader, this->m_combos, this->m_overrideCombos, this->m_pass->textures,
	    this->m_noTextures, noConstants
	);
	const auto sources = Shaders::GLSLContext::get ().toGlsl (
	    this->m_shader->vertex (), this->m_shader->fragment (), this->m_pass->shader
	);
	this->link (sources.vertex, sources.fragment, this->m_pass->shader);
    } catch (const std::exception& e) {
	sLog.error ("Frame material ", file, " has no usable shader: ", e.what ());
	this->m_shader = nullptr;
	return;
    }

    for (const auto* unit : { &this->m_shader->getVertex (), &this->m_shader->getFragment () }) {
	for (const auto* parameter : unit->getParameters ()) {
	    switch (parameter->getType ()) {
		case DynamicValue::Float:
		    this->setValue (parameter->getName (), glm::vec4 (parameter->getFloat ()), 1);
		    break;
		case DynamicValue::Vec2:
		    this->setValue (parameter->getName (), glm::vec4 (parameter->getVec2 (), 0.0f, 0.0f), 2);
		    break;
		case DynamicValue::Vec3:
		    this->setValue (parameter->getName (), glm::vec4 (parameter->getVec3 (), 0.0f), 3);
		    break;
		case DynamicValue::Vec4:
		    this->setValue (parameter->getName (), parameter->getVec4 (), 4);
		    break;
		default:
		    break;
	    }
	}
    }

    for (const auto& [name, setting] : this->m_pass->constants) {
	const auto& value = *setting->value;

	switch (value.getType ()) {
	    case DynamicValue::Float:
		this->setConstant (name, value.getFloat ());
		break;
	    case DynamicValue::Vec3:
		this->setConstant (name, value.getVec3 ());
		break;
	    case DynamicValue::Vec4:
		this->setConstant (name, value.getVec4 ());
		break;
	    default:
		break;
	}
    }

    GLint previousVao = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGenVertexArrays (1, &this->m_vao);
    glBindVertexArray (this->m_vao);
    glGenBuffers (1, &this->m_vertices);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_vertices);
    glBufferData (GL_ARRAY_BUFFER, sizeof (kQuad), kQuad, GL_STATIC_DRAW);

    if (this->a_Position != -1) {
	glEnableVertexAttribArray (this->a_Position);
	glVertexAttribPointer (this->a_Position, 3, GL_FLOAT, GL_FALSE, 5 * sizeof (float), nullptr);
    }

    if (this->a_TexCoord != -1) {
	glEnableVertexAttribArray (this->a_TexCoord);
	glVertexAttribPointer (
	    this->a_TexCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof (float), reinterpret_cast<void*> (3 * sizeof (float))
	);
    }

    glBindVertexArray (previousVao);
}

FrameMaterial::~FrameMaterial () {
    if (this->m_program != GL_NONE) {
	glDeleteProgram (this->m_program);
    }

    if (this->m_vertices != GL_NONE) {
	glDeleteBuffers (1, &this->m_vertices);
    }

    if (this->m_vao != GL_NONE) {
	glDeleteVertexArrays (1, &this->m_vao);
    }
}

void FrameMaterial::link (const std::string& vertex, const std::string& fragment, const std::string& name) {
    const GLuint vertexShader = compile (vertex, GL_VERTEX_SHADER, name);
    const GLuint fragmentShader = compile (fragment, GL_FRAGMENT_SHADER, name);
    this->m_program = glCreateProgram ();
    glAttachShader (this->m_program, vertexShader);
    glAttachShader (this->m_program, fragmentShader);
    glLinkProgram (this->m_program);
    glDeleteShader (vertexShader);
    glDeleteShader (fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv (this->m_program, GL_LINK_STATUS, &linked);

    if (linked == GL_FALSE) {
	char log[2048] = {};
	glGetProgramInfoLog (this->m_program, sizeof (log) - 1, nullptr, log);
	sLog.error ("Frame material ", name, " failed to link: ", log);
	glDeleteProgram (this->m_program);
	this->m_program = GL_NONE;
	return;
    }

#if !NDEBUG
    glObjectLabel (GL_PROGRAM, this->m_program, -1, this->m_file.c_str ());
#endif

    this->a_Position = glGetAttribLocation (this->m_program, "a_Position");
    this->a_TexCoord = glGetAttribLocation (this->m_program, "a_TexCoord");

    // translated GLSL collapses sampler bindings to 0
    glUseProgram (this->m_program);

    for (int index = 0; index <= 9; index++) {
	const std::string sampler = "g_Texture" + std::to_string (index);

	if (const GLint location = glGetUniformLocation (this->m_program, sampler.c_str ()); location != -1) {
	    glUniform1i (location, index);
	}
    }
}

std::string FrameMaterial::uniformOf (const std::string& constant) const {
    if (this->m_shader == nullptr) {
	return {};
    }

    for (const auto* unit : { &this->m_shader->getFragment (), &this->m_shader->getVertex () }) {
	for (const auto* parameter : unit->getParameters ()) {
	    if (parameter->getIdentifierName () == constant) {
		return parameter->getName ();
	    }
	}
    }

    return {};
}

void FrameMaterial::setValue (const std::string& uniform, const glm::vec4& value, const int components) {
    if (uniform.empty ()) {
	return;
    }

    this->m_values.insert_or_assign (uniform, Value { .value = value, .components = components });
}

void FrameMaterial::setTexture (const int index, const GLuint texture, const GLenum target) {
    this->m_textures.insert_or_assign (index, Texture { .texture = texture, .target = target });
}

void FrameMaterial::setConstant (const std::string& name, const float value) {
    this->setValue (this->uniformOf (name), glm::vec4 (value), 1);
}

void FrameMaterial::setConstant (const std::string& name, const glm::vec3& value) {
    this->setValue (this->uniformOf (name), glm::vec4 (value, 0.0f), 3);
}

void FrameMaterial::setConstant (const std::string& name, const glm::vec4& value) {
    this->setValue (this->uniformOf (name), value, 4);
}

void FrameMaterial::setUniform (const std::string& name, const float value) {
    this->setValue (name, glm::vec4 (value), 1);
}

void FrameMaterial::setUniform (const std::string& name, const glm::vec2& value) {
    this->setValue (name, glm::vec4 (value, 0.0f, 0.0f), 2);
}

void FrameMaterial::setUniform (const std::string& name, const glm::vec4& value) { this->setValue (name, value, 4); }

void FrameMaterial::draw (const CFBO& target) const {
    this->draw (target.getFramebuffer (), { target.getRealWidth (), target.getRealHeight () });
}

void FrameMaterial::draw (const GLuint framebuffer, const glm::ivec2& size) const {
    if (this->m_program == GL_NONE) {
	return;
    }

    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glViewport (0, 0, size.x, size.y);
    glUseProgram (this->m_program);

    // util materials: no cull, no depth; additive (bloom), translucent (fade), else replace
    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glDisable (GL_CULL_FACE);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (this->m_pass->blending == BlendingMode_Additive) {
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE);
    } else if (this->m_pass->blending == BlendingMode_Translucent) {
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
	glDisable (GL_BLEND);
    }

    for (const auto& [uniform, value] : this->m_values) {
	const GLint location = glGetUniformLocation (this->m_program, uniform.c_str ());

	if (location == -1) {
	    continue;
	}

	switch (value.components) {
	    case 1:
		glUniform1f (location, value.value.x);
		break;
	    case 2:
		glUniform2fv (location, 1, &value.value.x);
		break;
	    case 3:
		glUniform3fv (location, 1, &value.value.x);
		break;
	    default:
		glUniform4fv (location, 1, &value.value.x);
		break;
	}
    }

    // slot overrides first, then the render target the material names
    std::map<int, Texture> textures = this->m_textures;

    for (const auto& [index, name] : this->m_pass->textures) {
	if (textures.contains (index)) {
	    continue;
	}

	if (const auto fbo = this->m_owner.find (name); fbo != nullptr) {
	    textures.emplace (index, Texture { .texture = fbo->getTextureID (0) });
	}
    }

    for (const auto& [index, texture] : textures) {
	glActiveTexture (GL_TEXTURE0 + index);
	glBindTexture (texture.target, texture.texture);
    }

    glActiveTexture (GL_TEXTURE0);

    GLint previousVao = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVao);
    glBindVertexArray (this->m_vao);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray (previousVao);
    glDisable (GL_BLEND);
}
