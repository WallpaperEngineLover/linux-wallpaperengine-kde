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
	std::string geometry;
    };

    GLSLContext ();
    ~GLSLContext ();

    [[nodiscard]] Sources toGlsl (
	const std::string& vertex, const std::string& fragment, const std::string& name = "",
	const std::string& geometry = ""
    );

    [[nodiscard]] static GLSLContext& get ();

private:
    static std::unique_ptr<GLSLContext> sInstance;
};
}