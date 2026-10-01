#pragma once

#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Shaders {
class GLSLContext {
public:
    enum UnitType { UnitType_Vertex = 0, UnitType_Fragment = 1, UnitType_Geometry = 2 };

    struct Sources {
	std::string vertex;
	std::string fragment;
	/** empty when the shader has no geometry stage */
	std::string geometry;
    };

    GLSLContext ();
    ~GLSLContext ();

    /** Throws with the error and the offending source lines when glslang rejects a unit, `name` is only used for
     *  that report. An empty `geometry` means no geometry stage. */
    [[nodiscard]] Sources toGlsl (
	const std::string& vertex, const std::string& fragment, const std::string& name = "",
	const std::string& geometry = ""
    );

    [[nodiscard]] static GLSLContext& get ();

private:
    static std::unique_ptr<GLSLContext> sInstance;
};
} // namespace WallpaperEngine::Render::Shaders