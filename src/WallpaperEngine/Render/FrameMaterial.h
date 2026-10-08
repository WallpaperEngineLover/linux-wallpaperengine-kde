#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>

#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"

namespace WallpaperEngine::Data::Model {
struct Project;
}

namespace WallpaperEngine::Render {
class CFBO;
class CWallpaper;

/** A util material drawn over a whole target (bloom, combine; sub_14017FA70, sub_140183610), first pass only */
class FrameMaterial {
public:
    /** on top of the material's own (sub_140150110) */
    FrameMaterial (
	const CWallpaper& owner, const Data::Model::Project& project, const std::string& file,
	const Data::Model::ComboMap& combos = {}
    );
    ~FrameMaterial ();

    FrameMaterial (const FrameMaterial&) = delete;
    FrameMaterial& operator= (const FrameMaterial&) = delete;

    /** material +208/+216 overrides */
    void setTexture (int index, GLuint texture, GLenum target = GL_TEXTURE_2D);
    /** by material name (sub_14017E920) */
    void setConstant (const std::string& name, float value);
    void setConstant (const std::string& name, const glm::vec3& value);
    void setConstant (const std::string& name, const glm::vec4& value);
    /** by shader name */
    void setUniform (const std::string& name, float value);
    void setUniform (const std::string& name, const glm::vec2& value);
    void setUniform (const std::string& name, const glm::vec4& value);

    void draw (GLuint framebuffer, const glm::ivec2& size) const;
    void draw (const CFBO& target) const;

private:
    struct Value {
	glm::vec4 value { 0.0f };
	int components = 1;
	bool integer = false;
    };

    void link (const std::string& vertex, const std::string& fragment, const std::string& name);
    void setValue (const std::string& uniform, const glm::vec4& value, int components);
    [[nodiscard]] std::string uniformOf (const std::string& constant) const;

    const CWallpaper& m_owner;
    std::string m_file;
    Data::Model::MaterialUniquePtr m_material;
    const Data::Model::MaterialPass* m_pass = nullptr;
    Data::Model::ComboMap m_combos;
    Data::Model::ComboMap m_overrideCombos;
    Data::Model::TextureMap m_noTextures;
    std::unique_ptr<Shaders::Shader> m_shader;
    GLuint m_program = GL_NONE;
    GLuint m_vertices = GL_NONE;
    /** the scene's VAO carries the output quad's attributes */
    GLuint m_vao = GL_NONE;
    GLint a_Position = -1;
    GLint a_TexCoord = -1;
    struct Texture {
	GLuint texture = GL_NONE;
	GLenum target = GL_TEXTURE_2D;
    };
    std::map<int, Texture> m_textures;
    std::map<std::string, Value> m_values;
};
} // namespace WallpaperEngine::Render
