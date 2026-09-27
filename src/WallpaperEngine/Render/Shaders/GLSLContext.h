#pragma once

#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Shaders {
class GLSLContext {
public:
    enum UnitType { UnitType_Vertex = 0, UnitType_Fragment = 1 };

    GLSLContext ();
    ~GLSLContext ();

    /** Throws with the error and the offending source lines when glslang rejects either unit, `name` is only
     *  used for that report. */
    [[nodiscard]] std::pair<std::string, std::string> toGlsl (
	const std::string& vertex, const std::string& fragment, const std::string& name = ""
    );

    [[nodiscard]] static GLSLContext& get ();

private:
    static std::unique_ptr<GLSLContext> sInstance;
};
} // namespace WallpaperEngine::Render::Shaders