#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Types.h"

namespace WallpaperEngine::Data::Model {

enum BlendingMode {
    BlendingMode_Unknown = 0,
    BlendingMode_Normal = 1,
    BlendingMode_Translucent = 2,
    BlendingMode_Additive = 3,
};

enum CullingMode { CullingMode_Unknown = 0, CullingMode_Normal = 1, CullingMode_Disable = 2 };

enum DepthtestMode {
    DepthtestMode_Unknown = 0,
    DepthtestMode_Disabled = 1,
    DepthtestMode_Enabled = 2,
};

enum DepthwriteMode {
    DepthwriteMode_Unknown = 0,
    DepthwriteMode_Disabled = 1,
    DepthwriteMode_Enabled = 2,
};

struct MaterialPass {
    BlendingMode blending;
    CullingMode cullmode;
    DepthtestMode depthtest;
    DepthwriteMode depthwrite;
    std::string shader;
    TextureMap textures;
    TextureMap usertextures;
    ComboMap combos;
    /** e.g. overbright, bloom settings */
    ShaderConstantMap constants;
};

struct Material {
    std::string filename;
    std::vector<MaterialPassUniquePtr> passes;
};

} // namespace WallpaperEngine::Data::Model