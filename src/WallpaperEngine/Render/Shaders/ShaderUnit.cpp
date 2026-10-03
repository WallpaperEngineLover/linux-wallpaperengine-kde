#include "ShaderUnit.h"

#include "WallpaperEngine/Logging/Log.h"
#include <cctype>
#include <charconv>
#include <cmath>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <utility>

#include "GLSLContext.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableFloat.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableInteger.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector2.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector3.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector4.h"

#include "WallpaperEngine/Data/Builders/VectorBuilder.h"
#include "WallpaperEngine/FileSystem/Container.h"

// the wpe_ defines rename words GLSL reserves that are plain identifiers in HLSL (3681571511's fxaa has a
// "float common")
#define SHADER_HEADER(filename)                                                                                        \
    "#version 330\n"                                                                                                   \
    "// ======================================================\n"                                                      \
    "// Processed shader "                                                                                             \
	+ filename                                                                                                     \
	+ "\n"                                                                                                         \
	  "// ======================================================\n"                                                \
	  "precision highp float;\n"                                                                                   \
	  "#define mul(x, y) ((y) * (x))\n"                                                                            \
	  "#define max(x, y) max (y, x)\n"                                                                             \
	  "#define lerp mix\n"                                                                                         \
	  "#define frac fract\n"                                                                                       \
	  "#define CASTI(x) (int(x))\n"                                                                                \
	  "#define CASTU(x) (uint(x))\n"                                                                               \
	  "#define CASTF(x) (float(x))\n"                                                                              \
	  "#define CAST4U(x) (uvec4(x))\n"                                                                             \
	  "#define CAST2(x) (vec2(x))\n"                                                                               \
	  "#define CAST3(x) (vec3(x))\n"                                                                               \
	  "#define CAST4(x) (vec4(x))\n"                                                                               \
	  "#define CAST3X3(x) (mat3(x))\n"                                                                             \
	  "#define float2 vec2\n"                                                                                      \
	  "#define float3 vec3\n"                                                                                      \
	  "#define float4 vec4\n"                                                                                      \
	  "#define int2 ivec2\n"                                                                                       \
	  "#define int3 ivec3\n"                                                                                       \
	  "#define int4 ivec4\n"                                                                                       \
	  "#define saturate(x) (clamp(x, 0.0, 1.0))\n"                                                                 \
	  "#define texSample2D texture\n"                                                                              \
	  "#define texSample2DLod textureLod\n"                                                                        \
	  "#define sampler2DComparison sampler2DShadow\n"                                                              \
	  "#define texSample2DCompare(s, u, d) vec4 (texture (s, vec3 (u, d)))\n"                                      \
	  "#define atan2 atan\n"                                                                                       \
	  "#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))\n"                                                              \
	  "#define ddx dFdx\n"                                                                                         \
	  "#define ddy(x) dFdy(-(x))\n"                                                                                \
	  "#define active wpe_active\n"                                                                                \
	  "#define asm wpe_asm\n"                                                                                      \
	  "#define cast wpe_cast\n"                                                                                    \
	  "#define common wpe_common\n"                                                                                \
	  "#define enum wpe_enum\n"                                                                                    \
	  "#define external wpe_external\n"                                                                            \
	  "#define filter wpe_filter\n"                                                                                \
	  "#define fixed wpe_fixed\n"                                                                                  \
	  "#define goto wpe_goto\n"                                                                                    \
	  "#define input wpe_input\n"                                                                                  \
	  "#define long wpe_long\n"                                                                                    \
	  "#define noinline wpe_noinline\n"                                                                            \
	  "#define output wpe_output\n"                                                                                \
	  "#define partition wpe_partition\n"                                                                          \
	  "#define public wpe_public\n"                                                                                \
	  "#define resource wpe_resource\n"                                                                            \
	  "#define short wpe_short\n"                                                                                  \
	  "#define sizeof wpe_sizeof\n"                                                                                \
	  "#define superp wpe_superp\n"                                                                                \
	  "#define this wpe_this\n"                                                                                    \
	  "#define union wpe_union\n"                                                                                  \
	  "#define using wpe_using\n"                                                                                  \
	  "#define GLSL 1\n\n";
#define FRAGMENT_SHADER_DEFINES                                                                                        \
    "out vec4 out_FragColor;\n"                                                                                        \
    "#define varying in\n"
#define GEOMETRY_SHADER_DEFINES "#define GEOMETRY 1\n"
#define GEOMETRY_INPUT_PREFIX "wpeGs_"
#define VERTEX_SHADER_DEFINES                                                                                          \
    "#define attribute in\n"                                                                                           \
    "#define varying out\n"
#define DEFINE_COMBO(name, value) "#define " + name + " " + std::to_string (value) + "\n";

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Render::Shaders;

namespace {
// "name|name|..." of every uniform, varying and attribute the source declares, for regex alternations
std::string declaredInputNames (const std::string& source) {
    static const std::regex inputDecl (R"(\b(?:uniform|varying|attribute)\s+\w+\s+([A-Za-z_]\w*))");

    std::string names;
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), inputDecl); it != std::sregex_iterator ();
	 ++it) {
	names += (names.empty () ? "" : "|") + (*it)[1].str ();
    }

    return names;
}

// the #if/#elif/#else lines leading to the branch that contains pos, one list per open level, so code
// moved somewhere else can be put back under the same condition
std::vector<std::vector<std::string>> conditionalBranch (const std::string& source, const size_t pos) {
    static const std::regex directive (R"(^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b)");

    std::vector<std::vector<std::string>> levels;
    size_t start = 0;
    while (start < pos) {
	const size_t end = std::min (source.find ('\n', start), source.size ());
	const std::string line = source.substr (start, end - start);

	if (std::smatch match; std::regex_search (line, match, directive)) {
	    const std::string kind = match[1].str ();
	    if (kind == "endif") {
		if (!levels.empty ()) {
		    levels.pop_back ();
		}
	    } else if (kind == "elif" || kind == "else") {
		if (!levels.empty ()) {
		    levels.back ().push_back (line);
		}
	    } else {
		levels.push_back ({ line });
	    }
	}

	start = end + 1;
    }

    return levels;
}

// the quoted filename of the #include at start, or nothing if the quotes aren't on the same line
std::optional<std::string> includeFilename (const std::string& source, const size_t start) {
    const size_t lineEnd = std::min (source.find ('\n', start), source.size ());
    const size_t quoteStart = source.find ('"', start);

    if (quoteStart >= lineEnd) {
	return std::nullopt;
    }

    const size_t quoteEnd = source.find ('"', quoteStart + 1);

    if (quoteEnd >= lineEnd) {
	return std::nullopt;
    }

    return source.substr (quoteStart + 1, quoteEnd - quoteStart - 1);
}
} // namespace

ShaderUnit::ShaderUnit (
    const GLSLContext::UnitType type, std::string file, std::string content, const AssetLocator& assetLocator,
    const ShaderConstantMap& constants, const TextureMap& passTextures, const TextureMap& overrideTextures,
    const ComboMap& combos, const ComboMap& overrideCombos
) :
    m_type (type), m_file (std::move (file)), m_content (std::move (content)), m_combos (combos),
    m_overrideCombos (overrideCombos), m_constants (constants), m_passTextures (passTextures),
    m_overrideTextures (overrideTextures), m_link (nullptr), m_assetLocator (assetLocator) {
    this->preprocess ();
}

void ShaderUnit::preprocess () {
    this->m_preprocessed = this->m_content;

    this->preprocessIncludes ();
    this->preprocessRequires ();
    this->preprocessVariables ();
    this->preprocessBalanceConditionals ();
    this->preprocessSwizzledDeclarations ();
    this->preprocessScalarSwizzles ();

    const std::string from = "gl_FragColor";
    const std::string to = "out_FragColor";

    size_t start_pos = 0;
    while ((start_pos = this->m_preprocessed.find (from, start_pos)) != std::string::npos) {
	this->m_preprocessed.replace (start_pos, from.length (), to);
	start_pos += to.length (); // avoids re-matching if 'to' is a substring of 'from'
    }
}

void ShaderUnit::preprocessVariables () {
    size_t start = 0, end = 0;
    while ((end = this->m_preprocessed.find ('\n', start)) != std::string::npos) {
	std::string line = this->m_preprocessed.substr (start, end - start);
	const size_t combo = line.find ("// [COMBO] ");
	const size_t uniform = line.find ("uniform ");
	const size_t comment = line.find ("// ");
	const size_t semicolon = line.find (';');

	if (combo != std::string::npos) {
	    this->parseComboConfiguration (line.substr (combo + strlen ("// [COMBO] ")), 0);
	} else if (
	    uniform != std::string::npos && comment != std::string::npos && semicolon != std::string::npos &&
	    // semicolon before comment means it's a trailing comment, not a commented-out line
	    // (doesn't account for block comments)
	    semicolon < comment
	) {
	    // uniforms with trailing comments never have a value assigned, which is what lets this find type/name
	    const size_t last_space = line.find_last_of (' ', semicolon);

	    if (last_space != std::string::npos) {
		const size_t previous_space = line.find_last_of (' ', last_space - 1);

		if (previous_space != std::string::npos) {
		    std::string type = line.substr (previous_space + 1, last_space - previous_space - 1);
		    std::string name = line.substr (last_space + 1, semicolon - last_space - 1);
		    std::string json = line.substr (comment + 2);

		    this->parseParameterConfiguration (type, name, json);
		}
	    }
	}

	start = end + 1;
    }
}

void ShaderUnit::preprocessIncludes () {
    // wallpaper64.exe sub_140162100: line by line, a line starting with #include is replaced by the file between its
    // quotes (shaders/<name>, expanded the same way) where it stands, every file only once per unit. A file that was
    // already included, or a line without the quotes, leaves an empty line. WE's translator (sub_1400F5CB0) then
    // takes every attribute/varying/uniform line out of the code and declares it up front, so included functions see
    // uniforms the file only declares after the #include. Here the file's own declarations outside any #if move up to
    // where its first include starts, conditional ones stay where they are
    std::set<std::string> included;
    std::string out;
    std::string hoisted;
    size_t firstInclude = std::string::npos;
    int depth = 0;
    const std::string& source = this->m_preprocessed;
    size_t start = 0;

    while (start < source.size ()) {
	size_t end = source.find ('\n', start);
	if (end == std::string::npos) {
	    end = source.size ();
	}

	const std::string line = source.substr (start, end - start);
	const size_t first = line.find_first_not_of (" \t");
	const std::string trimmed = first == std::string::npos ? "" : line.substr (first);

	if (trimmed.starts_with ("#if")) {
	    depth++;
	} else if (trimmed.starts_with ("#endif")) {
	    depth = std::max (depth - 1, 0);
	}

	if (line.starts_with ("#include")) {
	    if (firstInclude == std::string::npos) {
		firstInclude = out.size ();
	    }

	    out += this->expandIncludes (line, included);
	} else if (
	    firstInclude != std::string::npos && depth == 0
	    && (trimmed.starts_with ("uniform ") || trimmed.starts_with ("varying ")
		|| trimmed.starts_with ("attribute "))
	) {
	    hoisted += line + '\n';
	    out += '\n';
	} else {
	    out += line + '\n';
	}

	start = end + 1;
    }

    if (!hoisted.empty ()) {
	out.insert (firstInclude, hoisted);
    }

    this->m_preprocessed = out;
}

std::string ShaderUnit::expandIncludes (const std::string& source, std::set<std::string>& included) const {
    std::string out;
    size_t start = 0;

    while (start < source.size ()) {
	size_t end = source.find ('\n', start);
	if (end == std::string::npos) {
	    end = source.size ();
	}

	if (source.compare (start, 8, "#include") != 0) {
	    out.append (source, start, end - start + (end < source.size () ? 1 : 0));
	    start = end + 1;
	    continue;
	}

	const auto filename = includeFilename (source, start);

	if (!filename.has_value ()) {
	    sLog.error ("Malformed #include directive in shader ", this->m_file);
	} else if (included.insert (*filename).second) {
	    try {
		out += this->expandIncludes (this->m_assetLocator.includeShader (*filename), included);
	    } catch (AssetLoadException&) {
		sLog.error ("Shader include ", *filename, " of ", this->m_file, " was not found");
	    }
	}

	out += '\n';
	start = end + 1;
    }

    return out;
}

void ShaderUnit::preprocessRequires () {
    size_t start = 0, end = 0;

    while ((start = this->m_preprocessed.find ("#require", end)) != std::string::npos) {
	const size_t lineEnd = this->m_preprocessed.find_first_of ('\n', start);

	const size_t nameStart = start + std::string ("#require ").length ();

	if (nameStart >= lineEnd) {
	    sLog.error ("Malformed #require directive (no module name) in shader ", this->m_file);
	    end = lineEnd;
	    continue;
	}

	std::string moduleName = this->m_preprocessed.substr (nameStart, lineEnd - nameStart);

	while (!moduleName.empty () && (moduleName.back () == ' ' || moduleName.back () == '\r')) {
	    moduleName.pop_back ();
	}

	if (moduleName.empty ()) {
	    sLog.error ("Malformed #require directive (empty module name) in shader ", this->m_file);
	    end = lineEnd;
	    continue;
	}

	sLog.debug ("Resolving require module: ", moduleName, " in shader ", this->m_file);

	std::string moduleCode = this->resolveRequireModule (moduleName);

	this->m_preprocessed = this->m_preprocessed.replace (start, 2, "//");

	if (!moduleCode.empty ()) {
	    this->m_preprocessed.insert (start, moduleCode);
	    end = start + moduleCode.length ();
	} else {
	    end = lineEnd;
	}
    }
}

std::string ShaderUnit::resolveRequireModule (const std::string& moduleName) const {
    if (moduleName == "LightingV1") {
	return this->generateLightingV1 ();
    }

    sLog.error ("Unknown #require module: ", moduleName, " in shader ", this->m_file);
    return "";
}

std::string ShaderUnit::generateLightingV1 () const {
    // wallpaper64.exe 2.8.42 sub_140169140, from the pass's LIGHTS_* defines (sub_1401A5C40). WE emits nothing when the
    // pass's LIGHTING combo is 0; its default is only parsed after the requires, so the preprocessor decides that here
    const auto count = [this] (const std::string& name) {
	for (const ComboMap* combos : { &this->m_overrideCombos, &this->m_combos }) {
	    if (const auto it = combos->find (name); it != combos->end ()) {
		return std::max (it->second, 0);
	    }
	}
	return 0;
    };
    const int points = count ("LIGHTS_POINT");
    const int spots = count ("LIGHTS_SPOT");
    const int tubes = count ("LIGHTS_TUBE");
    const int directionals = count ("LIGHTS_DIRECTIONAL");
    const int spotShadowCookies = count ("LIGHTS_SPOT_SHADOW_COOKIE");
    const int spotShadows = count ("LIGHTS_SPOT_SHADOW");
    const int spotCookies = count ("LIGHTS_SPOT_COOKIE");
    const int directionalShadows = count ("LIGHTS_DIRECTIONAL_SHADOW");
    const int pointShadows = count ("LIGHTS_POINT_SHADOW");
    const int features = spotShadowCookies + spotShadows + spotCookies + 3 * directionalShadows;
    std::string out = "// begin of generated module LightingV1\n#if LIGHTING\n";

    const auto declare = [&out] (const char* type, const char* name, const int size) {
	out += std::string ("uniform ") + type + " " + name + "[" + std::to_string (size) + "];\n";
    };
    const auto index = [] (const char* name, const int value) {
	return std::string ("\tconst uint ") + name + " = " + std::to_string (value) + "u;\n";
    };

    if (points) {
	declare ("vec4", "g_LPoint_Color", points);
	declare ("vec4", "g_LPoint_Origin", points);
    }
    if (spots) {
	declare ("vec4", "g_LSpot_Color", spots);
	declare ("vec4", "g_LSpot_Origin", spots);
	declare ("vec4", "g_LSpot_Direction", spots);
	declare ("vec4", "g_LSpot_Exponent", spots);
    }
    if (tubes) {
	declare ("vec4", "g_LTube_Color", tubes);
	declare ("vec4", "g_LTube_OriginA", tubes);
	declare ("vec4", "g_LTube_OriginB", tubes);
    }
    if (directionals) {
	declare ("vec4", "g_LDirectional_Color", directionals);
	declare ("vec4", "g_LDirectional_Direction", directionals);
    }
    if (features) {
	declare ("mat4", "g_LFeature_ShadowProjection", features);
	declare ("vec4", "g_LFeature_ShadowProjectionTransform", features);
    }
    if (pointShadows) {
	declare ("vec4", "g_LFeature_ShadowPointProjection", pointShadows);
	declare ("vec4", "g_LFeature_ShadowPointProjectionTransform", pointShadows);
    }

    out += "vec3 PerformLighting_V1(vec3 worldPos, vec3 color, vec3 normal, vec3 viewVector, vec3 specularTint, vec3 "
	   "ambient, float roughness, float metallic)\n{\n\tvec3 light = CAST3(0.0);\n";

    int light = 0;

    for (; light < pointShadows; light++) {
	out += "{\n" + index ("i", light);
	out += "\tvec3 lightDelta = g_LPoint_Origin[i].xyz - worldPos;\n"
	       "\tvec4 projectedCoords = CalculateProjectedCoordsPoint(worldPos, g_LPoint_Origin[i].xyz, "
	       "g_LFeature_ShadowPointProjection[i], g_LFeature_ShadowPointProjectionTransform[i]);\n"
	       "\tfloat shadowFactor = PerformPointShadowMapping(projectedCoords);\n"
	       "\tlight += ComputePBRLightShadow(normal, lightDelta, viewVector, color, g_LPoint_Color[i].rgb, "
	       "g_LPoint_Color[i].w, g_LPoint_Origin[i].w, specularTint, ambient, roughness, metallic, shadowFactor);\n"
	       "}\n";
    }
    for (; light < points; light++) {
	out += "{\n" + index ("i", light);
	out += "\tvec3 lightDelta = g_LPoint_Origin[i].xyz - worldPos;\n"
	       "\tlight += ComputePBRLightShadow(normal, lightDelta, viewVector, color, g_LPoint_Color[i].rgb, "
	       "g_LPoint_Color[i].w, g_LPoint_Origin[i].w, specularTint, ambient, roughness, metallic, 1.0);\n"
	       "}\n";
    }

    // spots: shadow and cookie, cookie, shadow, plain, one running index
    const std::string spotCookieSample = "\tvec3 projectedCoords = CalculateProjectedCoords(worldPos, "
					 "g_LFeature_ShadowProjection[i]);\n";
    const std::string spotShadowSample = "\tfloat shadowFactor = PerformShadowMapping(projectedCoords, "
					 "g_LFeature_ShadowProjectionTransform[i]);\n";
    const std::string spotCone
	= "\tfloat spotCookie = -dot(normalize(lightDelta), g_LSpot_Direction[i].xyz);\n"
	  "\tspotCookie = smoothstep(g_LSpot_Direction[i].w, g_LSpot_Origin[i].w, spotCookie);\n";
    const auto spotLight = [] (const char* scale, const char* shadow) {
	return std::string (
		   "\tlight += ComputePBRLightShadow(normal, lightDelta, viewVector, color, g_LSpot_Color[i].rgb * "
	       )
	    + scale + ", g_LSpot_Color[i].w, g_LSpot_Exponent[i].x, specularTint, ambient, roughness, metallic, "
	    + shadow + ");\n";
    };
    const std::string spotDelta = "\tvec3 lightDelta = g_LSpot_Origin[i].xyz - worldPos;\n";
    const std::string cookieSample = "\tvec3 colorCookie = texSample2D(COOKIE_SAMPLER, projectedCoords.xy).rgb;\n";
    int spot = 0;

    for (int n = 0; n < spotShadowCookies; n++, spot++) {
	out += "{\n" + index ("i", spot) + spotDelta + spotCookieSample + spotShadowSample + cookieSample
	    + spotLight ("colorCookie", "shadowFactor") + "}\n";
    }
    for (int n = 0; n < spotCookies; n++, spot++) {
	out += "{\n" + index ("i", spot) + spotDelta + spotCookieSample + cookieSample
	    + spotLight ("colorCookie", "1.0") + "}\n";
    }
    for (int n = 0; n < spotShadows; n++, spot++) {
	out += "{\n" + index ("i", spot) + spotDelta + spotCone + spotCookieSample + spotShadowSample
	    + spotLight ("spotCookie", "shadowFactor") + "}\n";
    }
    for (; spot < spots; spot++) {
	out += "{\n" + index ("i", spot) + spotDelta + spotCone + spotLight ("spotCookie", "1.0") + "}\n";
    }

    for (int tube = 0; tube < tubes; tube++) {
	out += "{\n" + index ("i", tube);
	out += "\tvec3 lightDelta = PointSegmentDelta(worldPos, g_LTube_OriginA[i].xyz, g_LTube_OriginB[i].xyz);\n"
	       "\tlight += ComputePBRLightShadow(normal, lightDelta, viewVector, color, g_LTube_Color[i].rgb, "
	       "g_LTube_Color[i].w, g_LTube_OriginA[i].w, specularTint, ambient, roughness, metallic, 1.0);\n"
	       "}\n";
    }

    // shadowed directional lights read three cascades starting after the spots' shadow/cookie matrices; WE moves that
    // start by one per light, not three
    int cascade = spotShadowCookies + spotCookies + spotShadows;
    int directional = 0;

    for (; directional < directionalShadows; directional++, cascade++) {
	out += "{\n" + index ("i", directional) + index ("p1", cascade) + index ("p2", cascade + 1)
	    + index ("p3", cascade + 2);
	out += "\tvec4 projectedCoords1 = CalculateProjectedCoordsCascades(worldPos, "
	       "g_LFeature_ShadowProjection[p1]);\n"
	       "\tvec4 projectedCoords2 = CalculateProjectedCoordsCascades(worldPos, "
	       "g_LFeature_ShadowProjection[p2]);\n"
	       "\tvec4 projectedCoords3 = CalculateProjectedCoordsCascades(worldPos, "
	       "g_LFeature_ShadowProjection[p3]);\n"
	       "\tprojectedCoords1.xyz = mix(projectedCoords1.xyz, projectedCoords2.xyz, projectedCoords1.w);\n"
	       "\tprojectedCoords1.xyz = mix(projectedCoords1.xyz, projectedCoords3.xyz, projectedCoords2.w);\n"
	       "\tvec4 uvTransforms = mix(g_LFeature_ShadowProjectionTransform[p1], "
	       "g_LFeature_ShadowProjectionTransform[p2], projectedCoords1.w);\n"
	       "\tuvTransforms = mix(uvTransforms, g_LFeature_ShadowProjectionTransform[p3], projectedCoords2.w);\n"
	       "\tfloat shadowFactor = max(projectedCoords3.w, PerformShadowMapping(projectedCoords1.xyz, "
	       "uvTransforms));\n"
	       "\tlight += ComputePBRLightShadowInfinite(normal, g_LDirectional_Direction[i].xyz, viewVector, color, "
	       "g_LDirectional_Color[i].rgb, specularTint, ambient, roughness, metallic, shadowFactor);\n"
	       "}\n";
    }
    for (; directional < directionals; directional++) {
	out += "{\n" + index ("i", directional);
	out += "\tlight += ComputePBRLightShadowInfinite(normal, g_LDirectional_Direction[i].xyz, viewVector, color, "
	       "g_LDirectional_Color[i].rgb, specularTint, ambient, roughness, metallic, 1.0);\n"
	       "}\n";
    }

    out += "\treturn light;\n}\n#endif\n// end of generated module LightingV1\n";
    return out;
}

void ShaderUnit::preprocessBalanceConditionals () {
    static const std::regex directive (R"((?:^|\n)[ \t]*#(ifndef|ifdef|if|endif)\b)");

    int depth = 0;
    std::vector<size_t> extraEndifs;

    auto begin = std::sregex_iterator (this->m_preprocessed.cbegin (), this->m_preprocessed.cend (), directive);
    auto end = std::sregex_iterator ();

    for (auto it = begin; it != end; ++it) {
	const std::string& keyword = (*it)[1].str ();

	if (keyword == "endif") {
	    if (depth == 0) {
		// position of the '#' character for this directive
		extraEndifs.push_back (it->position (1) - 1);
	    } else {
		depth--;
	    }
	} else {
	    depth++;
	}
    }

    // comment out the extra #endif directives, from the end so earlier offsets stay valid
    for (auto it = extraEndifs.rbegin (); it != extraEndifs.rend (); ++it) {
	sLog.out ("Found #endif with no matching #if in shader ", this->m_file, ", ignoring it");
	this->m_preprocessed.replace (*it, 2, "//");
    }
}

void ShaderUnit::preprocessSwizzledDeclarations () {
    static const std::regex swizzledDecl (
	R"(\b(varying|uniform|attribute)(\s+[A-Za-z0-9_]+\s+[A-Za-z_][A-Za-z0-9_]*)\.[xyzwrgba]+(\s*;))"
    );

    const std::string original = this->m_preprocessed;
    this->m_preprocessed = std::regex_replace (this->m_preprocessed, swizzledDecl, "$1$2$3");

    if (this->m_preprocessed != original) {
	sLog.out ("Dropped swizzle from declaration name in shader ", this->m_file);
    }
}

void ShaderUnit::preprocessScalarSwizzles () {
    // genericropeparticle's non geometry shader TRAILSCROLLALPHA + TRAILFADESIZE branch writes sizeStart.w on a
    // float, WE never compiles that branch (it draws ropes with a geometry shader) and GLSL rejects it. A single
    // component swizzle of a variable that is only ever declared as float is the variable itself
    static const std::regex declaration (
	R"(\b(float|int|bool|u?int|[biu]?vec[234]|mat[234](?:x[234])?)\s+([A-Za-z_][A-Za-z0-9_]*)\b)"
    );

    std::set<std::string> scalars;
    std::set<std::string> others;
    for (auto it = std::sregex_iterator (this->m_preprocessed.begin (), this->m_preprocessed.end (), declaration);
	 it != std::sregex_iterator (); ++it) {
	((*it)[1] == "float" ? scalars : others).insert ((*it)[2]);
    }

    for (const auto& name : scalars) {
	// std::regex is slow, most names never show up with a dot after them
	if (others.contains (name) || this->m_preprocessed.find (name + ".") == std::string::npos) {
	    continue;
	}

	const std::regex swizzle ("\\b" + name + "\\.[xyzwrgba](?![A-Za-z0-9_])");
	this->m_preprocessed = std::regex_replace (this->m_preprocessed, swizzle, name);
    }
}

std::string ShaderUnit::applyVectorTruncationCompatibility (std::string source) const {
    static const std::regex vectorDecl (R"(\b(vec[234])\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    static const std::regex narrowAssign (R"(\b(vec[23])\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*([^;{}]+);)");

    // name -> width, 0 when the same name is declared with different widths in different scopes
    std::unordered_map<std::string, int> widths;
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), vectorDecl); it != std::sregex_iterator ();
	 ++it) {
	const int width = (*it)[1].str ().back () - '0';
	const std::string name = (*it)[2].str ();
	const auto found = widths.find (name);

	if (found == widths.end ()) {
	    widths.emplace (name, width);
	} else if (found->second != width) {
	    found->second = 0;
	}
    }

    static const char* swizzles[] = { "", "", ".xy", ".xyz" };

    bool changed = false;

    const auto truncate = [&] (const std::string& expr, const int targetWidth) {
	// HLSL truncates a wider operand anywhere the result narrows, so plain grouping parens are
	// looked through too: "vec2 uv = (v4 * res.xy) / 4" -> "(v4.xy * res.xy) / 4". call arguments,
	// indices and groups that get swizzled or indexed afterwards keep their operands as they are
	std::vector<bool> groups;
	int blocked = 0;
	std::string fixed;

	for (size_t i = 0; i < expr.size ();) {
	    const char c = expr[i];

	    if (c == '(' || c == '[') {
		bool grouping = c == '(';

		if (grouping) {
		    size_t prev = i;
		    while (prev > 0 && std::isspace (static_cast<unsigned char> (expr[prev - 1]))) {
			prev--;
		    }
		    if (prev > 0
			&& (std::isalnum (static_cast<unsigned char> (expr[prev - 1])) || expr[prev - 1] == '_')) {
			grouping = false;
		    }
		}

		if (grouping) {
		    int nested = 0;
		    size_t close = i;
		    for (; close < expr.size (); close++) {
			if (expr[close] == '(' || expr[close] == '[') {
			    nested++;
			} else if ((expr[close] == ')' || expr[close] == ']') && --nested == 0) {
			    break;
			}
		    }
		    size_t next = close + 1;
		    while (next < expr.size () && std::isspace (static_cast<unsigned char> (expr[next]))) {
			next++;
		    }
		    if (close >= expr.size () || (next < expr.size () && (expr[next] == '.' || expr[next] == '['))) {
			grouping = false;
		    }
		}

		groups.push_back (grouping);
		blocked += grouping ? 0 : 1;
	    } else if ((c == ')' || c == ']') && !groups.empty ()) {
		blocked -= groups.back () ? 0 : 1;
		groups.pop_back ();
	    }

	    if (!(std::isalpha (static_cast<unsigned char> (c)) || c == '_')) {
		fixed += c;
		i++;
		continue;
	    }

	    size_t end = i;
	    while (end < expr.size () && (std::isalnum (static_cast<unsigned char> (expr[end])) || expr[end] == '_')) {
		end++;
	    }

	    const std::string ident = expr.substr (i, end - i);
	    fixed += ident;

	    size_t before = i;
	    while (before > 0 && std::isspace (static_cast<unsigned char> (expr[before - 1]))) {
		before--;
	    }
	    size_t after = end;
	    while (after < expr.size () && std::isspace (static_cast<unsigned char> (expr[after]))) {
		after++;
	    }

	    const bool member = before > 0 && expr[before - 1] == '.';
	    const bool accessed
		= after < expr.size () && (expr[after] == '.' || expr[after] == '(' || expr[after] == '[');
	    const auto found = widths.find (ident);

	    if (blocked == 0 && !member && !accessed && found != widths.end () && found->second > targetWidth) {
		fixed += swizzles[targetWidth];
		changed = true;
	    }

	    i = end;
	}

	return fixed;
    };

    std::string result;
    size_t last = 0;

    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), narrowAssign); it != std::sregex_iterator ();
	 ++it) {
	const size_t exprStart = it->position (2);

	result.append (source, last, exprStart - last);
	result += truncate ((*it)[2].str (), (*it)[1].str ().back () - '0');
	last = exprStart + it->length (2);
    }

    result.append (source, last, std::string::npos);

    // the coordinates of a 2D sample are a float2 in HLSL, "texSample2D(s, uv.xy - (k * v4))" truncates v4
    static const std::regex sampleCall (R"(\btexSample2D(?:Lod)?\s*\()");
    std::string sampled;
    last = 0;

    for (auto it = std::sregex_iterator (result.cbegin (), result.cend (), sampleCall); it != std::sregex_iterator ();
	 ++it) {
	const size_t open = it->position () + it->length () - 1;
	size_t argStart = std::string::npos;
	size_t argEnd = std::string::npos;
	int nested = 0;

	for (size_t i = open; i < result.size (); i++) {
	    const char c = result[i];

	    if (c == '(' || c == '[') {
		nested++;
	    } else if (c == ')' || c == ']') {
		if (--nested == 0) {
		    if (argStart != std::string::npos && argEnd == std::string::npos) {
			argEnd = i;
		    }
		    break;
		}
	    } else if (c == ',' && nested == 1) {
		if (argStart == std::string::npos) {
		    argStart = i + 1;
		} else if (argEnd == std::string::npos) {
		    argEnd = i;
		}
	    } else if (c == ';' || c == '{' || c == '}') {
		break;
	    }
	}

	if (argStart == std::string::npos || argEnd == std::string::npos || argStart < last) {
	    continue;
	}

	sampled.append (result, last, argStart - last);
	sampled += truncate (result.substr (argStart, argEnd - argStart), 2);
	last = argEnd;
    }

    if (!changed) {
	return source;
    }

    sampled.append (result, last, std::string::npos);
    sLog.out ("Applied vector truncation compatibility in shader ", this->m_file);

    return sampled;
}

std::string ShaderUnit::applyFloatConditionCompatibility (std::string source) const {
    static const std::regex floatDecl (R"(\b(float|vec[234]|int|bool|mat[234])\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    static const std::regex ternaryCond (R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\?)");
    static const std::regex ifCond (R"(\bif\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))");

    // names that are only ever declared as float
    std::unordered_map<std::string, bool> onlyFloat;
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), floatDecl); it != std::sregex_iterator ();
	 ++it) {
	const bool isFloat = (*it)[1].str () == "float";
	const std::string name = (*it)[2].str ();
	const auto found = onlyFloat.find (name);

	if (found == onlyFloat.end ()) {
	    onlyFloat.emplace (name, isFloat);
	} else if (!isFloat) {
	    found->second = false;
	}
    }

    std::string result;
    size_t last = 0;
    bool changed = false;

    auto rewrite = [&] (const std::regex& pattern, bool ternary) {
	result.clear ();
	last = 0;

	for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), pattern); it != std::sregex_iterator ();
	     ++it) {
	    const std::string name = (*it)[1].str ();
	    const auto found = onlyFloat.find (name);
	    if (found == onlyFloat.end () || !found->second) {
		continue;
	    }

	    const size_t nameStart = it->position (1);
	    const size_t nameEnd = nameStart + name.size ();

	    if (ternary) {
		size_t before = nameStart;
		while (before > 0 && std::isspace (static_cast<unsigned char> (source[before - 1]))) {
		    before--;
		}
		if (before == 0) {
		    continue;
		}

		// only where the float is the whole condition (or a whole && / || operand), not e.g. `x == f ? ..`
		const char prev = source[before - 1];
		const char beforePrev = before > 1 ? source[before - 2] : ' ';
		const bool assignment = prev == '=' && std::string ("=!<>").find (beforePrev) == std::string::npos;

		if (!assignment && prev != '(' && prev != ',' && prev != '&' && prev != '|') {
		    continue;
		}
	    }

	    result.append (source, last, nameStart - last);
	    result += "(" + name + " != 0.0)";
	    last = nameEnd;
	    changed = true;
	}

	result.append (source, last, std::string::npos);
	source = result;
    };

    rewrite (ternaryCond, true);
    rewrite (ifCond, false);

    if (changed) {
	sLog.out ("Applied float condition compatibility in shader ", this->m_file);
    }

    return source;
}

std::string ShaderUnit::applyBoolArithmeticCompatibility (std::string source) const {
    static const std::regex decl (R"(\b(float|int|uint|bool|[biu]?vec[234]|mat[234])\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    static const std::regex assign (R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*([-+*/]?=)\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)");

    // every type each name is declared with (#if branches may declare it differently)
    std::unordered_map<std::string, std::set<std::string>> types;
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), decl); it != std::sregex_iterator (); ++it) {
	types[(*it)[2].str ()].insert ((*it)[1].str ());
    }

    const auto only = [] (const std::set<std::string>& set, std::initializer_list<const char*> allowed) {
	return std::ranges::all_of (set, [&allowed] (const std::string& type) {
	    return std::ranges::any_of (allowed, [&type] (const char* a) { return type == a; });
	});
    };

    std::string result;
    size_t last = 0;

    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), assign); it != std::sregex_iterator ();
	 ++it) {
	const auto target = types.find ((*it)[1].str ());
	const auto value = types.find ((*it)[3].str ());
	if (target == types.end () || value == types.end () || value->second != std::set<std::string> { "bool" }
	    || !only (target->second, { "int", "uint", "float" })) {
	    continue;
	}

	// int converts to float implicitly, so int () fits either way
	const std::string cast = target->second == std::set<std::string> { "uint" } ? "uint" : "int";
	result.append (source, last, it->position (3) - last);
	result += cast + "(" + (*it)[3].str () + ")";
	last = it->position (3) + it->length (3);
    }

    if (last == 0) {
	return source;
    }

    result.append (source, last, std::string::npos);
    sLog.out ("Applied bool arithmetic compatibility in shader ", this->m_file);
    return result;
}

std::string ShaderUnit::applyLinkedVaryingCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Vertex || this->m_link == nullptr) {
	return source;
    }

    std::regex fragmentVec4Varying (R"(\bvarying\s+vec4\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    std::smatch varyingMatch;
    std::string linked = this->m_link->m_preprocessed;
    size_t linkedOffset = 0;

    while (std::regex_search (linked.cbegin () + linkedOffset, linked.cend (), varyingMatch, fragmentVec4Varying)) {
	const std::string name = varyingMatch[1].str ();
	linkedOffset += varyingMatch.position () + varyingMatch.length ();

	const std::regex vertexVec2Decl ("\\bvarying\\s+vec2\\s+" + name + "\\s*;");
	if (!std::regex_search (source, vertexVec2Decl)) {
	    continue;
	}

	// m_preprocessed still has the #if branches: a varying declared once per branch (generic's v_TexCoord, vec4
	// with LIGHTMAP, vec2 without) already matches the fragment side in every combination
	const std::regex anyVertexDecl ("\\bvarying\\s+\\w+\\s+" + name + "\\s*;");
	if (std::distance (
		std::sregex_iterator (source.cbegin (), source.cend (), anyVertexDecl), std::sregex_iterator ()
	    )
	    != 1) {
	    continue;
	}

	source = std::regex_replace (source, vertexVec2Decl, "varying vec4 " + name + ";");

	const std::regex assignment ("(^|\\n)([ \\t]*)" + name + "\\s*=\\s*([^;\\n]+);");
	std::smatch assignmentMatch;
	size_t offset = 0;
	while (std::regex_search (source.cbegin () + offset, source.cend (), assignmentMatch, assignment)) {
	    const std::string prefix = assignmentMatch[1].str ();
	    const std::string indent = assignmentMatch[2].str ();
	    const std::string expression = assignmentMatch[3].str ();
	    const std::string replacement = prefix + indent + name + " = vec4(" + expression + ", 0.0, 1.0);";
	    const size_t position = offset + assignmentMatch.position ();
	    source.replace (position, assignmentMatch.length (), replacement);
	    offset = position + replacement.length ();
	}
    }

    return source;
}

std::string ShaderUnit::applyNarrowFragmentVaryingCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment || this->m_link == nullptr) {
	return source;
    }

    static const std::regex vertexVarying (R"(\bvarying\s+vec([34])\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    static const std::regex mainOpen (R"(\bvoid\s+main\s*\([^)]*\)\s*\{)");
    static const char* swizzles[] = { "", "", ".xy", ".xyz" };

    if (!std::regex_search (source, mainOpen)) {
	return source;
    }

    const std::string& linked = this->m_link->m_preprocessed;
    std::string copyCode;
    std::string names;

    for (auto it = std::sregex_iterator (linked.cbegin (), linked.cend (), vertexVarying);
	 it != std::sregex_iterator (); ++it) {
	const int vertexWidth = (*it)[1].str ()[0] - '0';
	const std::string name = (*it)[2].str ();
	const std::regex anyDecl ("\\bvarying\\s+\\w+\\s+" + name + "\\s*;");
	const std::regex narrowDecl ("\\bvarying\\s+vec([23])\\s+" + name + "\\s*;");
	std::smatch declMatch;

	// conditionals are still in the source here, a varying declared once per #if branch
	// (the stock generic shaders) can't be told apart from a real mismatch, leave those alone
	const auto declarations = [&anyDecl] (const std::string& text) {
	    return std::distance (
		std::sregex_iterator (text.cbegin (), text.cend (), anyDecl), std::sregex_iterator ()
	    );
	};

	if (declarations (linked) != 1 || declarations (source) != 1
	    || !std::regex_search (source, declMatch, narrowDecl) || declMatch[1].str ()[0] - '0' >= vertexWidth) {
	    continue;
	}

	const int width = declMatch[1].str ()[0] - '0';
	const std::string type = "vec" + std::to_string (width);
	const std::string copy = "wpeVar_" + name;

	source = std::regex_replace (source, std::regex ("(^|[^.\\w])" + name + "\\b"), "$1" + copy);
	source = std::regex_replace (
	    source, std::regex ("\\bvarying\\s+" + type + "\\s+" + copy + "\\s*;"),
	    "varying vec" + std::to_string (vertexWidth) + " " + name + "; " + type + " " + copy + ";"
	);
	copyCode += " " + copy + " = " + name + swizzles[width] + ";";
	names += (names.empty () ? "" : ", ") + name;
    }

    if (copyCode.empty ()) {
	return source;
    }

    std::smatch mainMatch;
    std::regex_search (source, mainMatch, mainOpen);
    source.insert (mainMatch.position (0) + mainMatch.length (0), copyCode);

    sLog.out ("Applied narrow fragment varying compatibility in ", this->m_file, " for ", names);

    return source;
}

std::string ShaderUnit::applyFragmentTexCoordCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment) {
	return source;
    }

    const std::regex texCoordBeforeCast2 (R"(\bv_TexCoord\b(\s*[-+*/]\s*CAST2\s*\())");
    const std::regex cast2BeforeTexCoord (R"((CAST2\s*\([^)]+\)\s*[-+*/]\s*)\bv_TexCoord\b)");

    const std::regex wideTexCoordDecl (R"(\bvarying\s+vec[34]\s+v_TexCoord\s*;)");
    if (!std::regex_search (source, wideTexCoordDecl)
	|| (!std::regex_search (source, texCoordBeforeCast2) && !std::regex_search (source, cast2BeforeTexCoord))) {
	return source;
    }

    const std::string original = source;
    source = std::regex_replace (source, texCoordBeforeCast2, "v_TexCoord.xy$1");
    source = std::regex_replace (source, cast2BeforeTexCoord, "$1v_TexCoord.xy");

    if (source != original) {
	sLog.out ("Applied fragment TexCoord vec2 compatibility in ", this->m_file);
    }

    return source;
}

std::string ShaderUnit::applyFragmentVaryingShadowCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment) {
	return source;
    }

    static const std::regex varyingDecl (R"(\bvarying\s+(vec[234]|float)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");

    std::vector<std::pair<std::string, std::string>> shadowed;

    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), varyingDecl); it != std::sregex_iterator ();
	 ++it) {
	const std::string type = (*it)[1].str ();
	const std::string name = (*it)[2].str ();

	// only shadow varyings the shader actually reassigns - a plain read-only "in" is fine as-is,
	// and touching the declaration unnecessarily risks breaking a shader that works today.
	// writes to a typed local of the same name don't count, that local already shadows the varying
	const std::regex localDecl ("\\b(?:vec[234]|float|int|bool)\\s+" + name + "\\b");
	const std::string withoutLocals = std::regex_replace (source, localDecl, " ");

	const std::regex assignmentUse (
	    "(?:\\b" + name + "\\b(?:\\.[xyzwrgba]+)?\\s*(?:=(?!=)|\\+=|-=|\\*=|/=|\\+\\+|--))|(?:(?:\\+\\+|--)\\s*"
	    + name + "\\b)"
	);
	if (!std::regex_search (withoutLocals, assignmentUse)) {
	    continue;
	}

	shadowed.emplace_back (type, name);
    }

    if (shadowed.empty ()) {
	return source;
    }

    static const std::regex mainOpen (R"(\bvoid\s+main\s*\([^)]*\)\s*\{)");
    if (!std::regex_search (source, mainOpen)) {
	return source;
    }

    // every use goes through a writable global copy instead, filled from the real (read-only)
    // input at the top of main(). a global rather than a local in main() so helper functions
    // writing to it work too, and the input keeps its name so it still links to the vertex output
    std::string copyCode;
    for (const auto& [type, name] : shadowed) {
	const std::string copy = "wpeVar_" + name;
	const std::regex use ("(^|[^.\\w])" + name + "\\b");
	const std::regex decl ("\\bvarying\\s+" + type + "\\s+" + copy + "\\s*;");

	source = std::regex_replace (source, use, "$1" + copy);
	source = std::regex_replace (source, decl, "varying " + type + " " + name + "; " + type + " " + copy + ";");
	copyCode += " " + copy + " = " + name + ";";
    }

    std::smatch mainMatch;
    std::regex_search (source, mainMatch, mainOpen);
    source.insert (mainMatch.position (0) + mainMatch.length (0), copyCode);

    std::string names;
    for (const auto& [type, name] : shadowed) {
	names += (names.empty () ? "" : ", ") + name;
    }
    sLog.out ("Applied fragment varying shadow compatibility in ", this->m_file, " for ", names);

    return source;
}

std::string ShaderUnit::applyDirectiveSemicolonCompatibility (std::string source) const {
    static const std::regex directive (R"((^|\n)([ \t]*#[ \t]*(?:if|elif)\b[^\n;]*?)[ \t]*;[ \t]*(?=\r?\n|$))");

    std::string result = std::regex_replace (source, directive, "$1$2");
    if (result != source) {
	sLog.out ("Dropped trailing semicolon from #if/#elif directive(s) in ", this->m_file);
    }

    return result;
}

std::string ShaderUnit::applyHlslAttributeCompatibility (std::string source) const {
    static const std::regex attribute (
	R"(([;{}]|^|\n)([ \t]*)\[\s*(?:loop|unroll|branch|flatten|fastopt|allow_uav_condition|forcecase|call)\s*(?:\(\s*\w*\s*\))?\s*\](?=\s*(?:for|while|do|if|switch)\b))"
    );

    std::string result = std::regex_replace (source, attribute, "$1$2");
    if (result != source) {
	sLog.out ("Dropped HLSL flow control attribute(s) in ", this->m_file);
    }

    return result;
}

std::string ShaderUnit::applyPackedFloatArrayCompatibility (std::string source) const {
    // WE packs a uniform float array into vec4 registers (every cached g_AudioSpectrum64Left is float4[16] in its
    // compiled shader's RDEF), so "name[a][b]" reads register a, component b (3605510527's video effect). GLSL
    // can't index a float, the same element is name[a * 4 + b]. HLSL's % on floats is fmod
    static const std::regex declaration (R"(\buniform\s+float\s+([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\]\s*;)");

    const auto closing = [&source] (size_t open) -> size_t {
	int depth = 0;
	for (size_t i = open; i < source.size (); i++) {
	    if (source[i] == '[' || source[i] == '(') {
		depth++;
	    } else if (source[i] == ']' || source[i] == ')') {
		if (--depth == 0) {
		    return source[i] == ']' ? i : std::string::npos;
		}
	    }
	}
	return std::string::npos;
    };

    const auto component = [] (const std::string& expression) {
	int depth = 0;
	for (size_t i = 0; i < expression.size (); i++) {
	    if (expression[i] == '(' || expression[i] == '[') {
		depth++;
	    } else if (expression[i] == ')' || expression[i] == ']') {
		depth--;
	    } else if (expression[i] == '%' && depth == 0) {
		return "int(fmod(float(" + expression.substr (0, i) + "), float(" + expression.substr (i + 1) + ")))";
	    }
	}
	return "int(" + expression + ")";
    };

    std::set<std::string> names;
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), declaration); it != std::sregex_iterator ();
	 ++it) {
	if (std::stoi ((*it)[2].str ()) % 4 == 0) {
	    names.insert ((*it)[1].str ());
	}
    }

    bool changed = false;

    for (const auto& name : names) {
	const std::regex use ("\\b" + name + "\\s*\\[");
	size_t offset = 0;
	std::smatch match;

	while (std::regex_search (source.cbegin () + offset, source.cend (), match, use)) {
	    const size_t start = offset + match.position ();
	    const size_t firstOpen = start + match.length () - 1;
	    const size_t firstClose = closing (firstOpen);
	    offset = start + match.length ();

	    if (firstClose == std::string::npos) {
		continue;
	    }

	    size_t secondOpen = firstClose + 1;
	    while (secondOpen < source.size () && std::isspace (static_cast<unsigned char> (source[secondOpen]))) {
		secondOpen++;
	    }

	    if (secondOpen >= source.size () || source[secondOpen] != '[') {
		continue;
	    }

	    const size_t secondClose = closing (secondOpen);
	    if (secondClose == std::string::npos) {
		continue;
	    }

	    const std::string first = source.substr (firstOpen + 1, firstClose - firstOpen - 1);
	    const std::string second = source.substr (secondOpen + 1, secondClose - secondOpen - 1);
	    const std::string replacement = name + "[int(" + first + ") * 4 + " + component (second) + "]";

	    source.replace (start, secondClose + 1 - start, replacement);
	    offset = start + replacement.size ();
	    changed = true;
	}
    }

    if (changed) {
	sLog.out ("Applied packed float array compatibility in ", this->m_file);
    }

    return source;
}

std::string ShaderUnit::applyNonConstantConstCompatibility (std::string source) const {
    // locals only, globals sit at column 0
    static const std::regex constLocal (R"((^|\n)([ \t]+)const\s+([^;=]+=([^;]*);))");
    // plain variables and function parameters, anything declared const is skipped below
    static const std::regex variableDecl (
	R"((\bconst\s+)?\b(?:float|int|uint|bool|[biu]?vec[234]|mat[234](?:x[234])?)\s+([A-Za-z_]\w*)\s*(?=[=;,)\[]))"
    );
    static const std::regex declaredName (R"(^[^=]*?([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)?=)");

    std::string names = R"(texSample2D\w*|texture\w*|g_\w+|v_\w+|wpeVar_\w+)";
    if (const std::string inputs = declaredInputNames (source); !inputs.empty ()) {
	names += "|" + inputs;
    }
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), variableDecl); it != std::sregex_iterator ();
	 ++it) {
	if (!(*it)[1].matched) {
	    names += "|" + (*it)[2].str ();
	}
    }

    std::string result;
    size_t count = 0;
    auto last = source.cbegin ();

    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), constLocal); it != std::sregex_iterator ();
	 ++it) {
	const auto& match = *it;
	const std::regex nonConstant (R"(\b(?:)" + names + R"()\b)");

	if (!std::regex_search (match[4].first, match[4].second, nonConstant)) {
	    continue;
	}

	// later consts reading this one aren't constant either
	if (std::smatch nameMatch; std::regex_search (match[3].first, match[3].second, nameMatch, declaredName)) {
	    names += "|" + nameMatch[1].str ();
	}

	result.append (last, match[0].first);
	result += match[1].str () + match[2].str () + match[3].str ();
	last = match[0].second;
	count++;
    }

    if (count == 0) {
	return source;
    }

    result.append (last, source.cend ());
    sLog.out ("Dropped const from ", count, " non-constant local(s) in ", this->m_file);

    return result;
}

std::string ShaderUnit::applyNonConstantGlobalConstCompatibility (std::string source) const {
    static const std::regex constGlobal (
	R"((^|\n)[ \t]*const\s+((?:float|int|uint|bool|[biu]?vec[234]|mat[234](?:x[234])?)\s+([A-Za-z_]\w*))\s*=([^;]*);)"
    );
    static const std::regex mainOpen (R"(\bvoid\s+main\s*\([^)]*\)\s*\{)");

    // only statements outside any function body count as globals
    std::vector<bool> topLevel (source.size () + 1, false);
    int depth = 0;
    bool comment = false;
    for (size_t i = 0; i < source.size (); i++) {
	topLevel[i] = depth == 0;

	if (source[i] == '\n') {
	    comment = false;
	} else if (!comment && source.compare (i, 2, "//") == 0) {
	    comment = true;
	} else if (!comment && source[i] == '{') {
	    depth++;
	} else if (!comment && source[i] == '}' && depth > 0) {
	    depth--;
	}
    }

    std::string nonConstant = R"(texSample2D\w*|texture\w*|wpeVar_\w+)";
    if (const std::string inputs = declaredInputNames (source); !inputs.empty ()) {
	nonConstant += "|" + inputs;
    }

    std::string result;
    std::string assignments;
    std::vector<std::string> moved;
    auto last = source.cbegin ();

    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), constGlobal); it != std::sregex_iterator ();
	 ++it) {
	const auto& match = *it;
	const std::string name = match[3].str ();
	const std::string init = match[4].str ();

	if (!topLevel[match.position (0) + match[1].length ()]) {
	    continue;
	}

	// a const that reads an earlier moved one isn't constant anymore either
	const std::regex uses (R"(\b(?:)" + nonConstant + R"()\b)");
	if (!std::regex_search (init, uses)) {
	    continue;
	}

	result.append (last, match[0].first);
	result += match[1].str () + match[2].str () + ";";
	last = match[0].second;

	// a declaration inside #if PROPERTIES == 6 only exists in that branch, so its assignment must too
	const auto branch = conditionalBranch (source, match.position (0) + match[1].length ());
	if (branch.empty ()) {
	    assignments += " " + name + " =" + init + ";";
	} else {
	    for (const auto& level : branch) {
		for (const auto& line : level) {
		    assignments += "\n" + line;
		}
	    }
	    assignments += "\n" + name + " =" + init + ";";
	    for (size_t i = 0; i < branch.size (); i++) {
		assignments += "\n#endif";
	    }
	    assignments += "\n";
	}
	nonConstant += "|" + name;
	moved.push_back (name);
    }

    if (moved.empty ()) {
	return source;
    }

    result.append (last, source.cend ());

    const auto mains
	= std::distance (std::sregex_iterator (result.cbegin (), result.cend (), mainOpen), std::sregex_iterator ());
    if (mains != 1) {
	return source;
    }

    std::smatch mainMatch;
    std::regex_search (result, mainMatch, mainOpen);
    result.insert (mainMatch.position (0) + mainMatch.length (0), assignments);

    std::string names;
    for (const auto& name : moved) {
	names += (names.empty () ? "" : ", ") + name;
    }
    sLog.out ("Moved non-constant global const initializer(s) into main() in ", this->m_file, ": ", names);

    return result;
}

int ShaderUnit::defineValue (const std::string& name, const std::string& source) const {
    for (const ComboMap* combos : { &this->m_overrideCombos, &this->m_combos, &this->m_discoveredCombos }) {
	for (const auto& [comboName, value] : *combos) {
	    std::string uppercase;
	    std::ranges::transform (comboName, std::back_inserter (uppercase), ::toupper);
	    if (uppercase == name) {
		return value;
	    }
	}
    }

    // the shader's own fallback, like "#ifndef TRAILSUBDIVISION #define TRAILSUBDIVISION 0"
    std::smatch match;
    if (std::regex_search (source, match, std::regex ("#define\\s+" + name + "\\s+(-?\\d+)"))) {
	return std::stoi (match[1].str ());
    }

    return 0;
}

namespace {
/** + - * / and parentheses over integers, what WE's [maxvertexcount(...)] expressions use */
int evaluateCount (const std::string& text, size_t& at) {
    const auto skip = [&] () {
	while (at < text.size () && std::isspace (static_cast<unsigned char> (text[at]))) {
	    at++;
	}
    };
    const std::function<int ()> sum = [&] () -> int {
	const std::function<int ()> factor = [&] () -> int {
	    skip ();
	    if (at < text.size () && text[at] == '(') {
		at++;
		const int value = sum ();
		skip ();
		at++;
		return value;
	    }
	    if (at < text.size () && text[at] == '-') {
		at++;
		return -factor ();
	    }
	    int value = 0;
	    while (at < text.size () && std::isdigit (static_cast<unsigned char> (text[at]))) {
		value = value * 10 + (text[at++] - '0');
	    }
	    return value;
	};
	const auto product = [&] () {
	    int value = factor ();
	    for (skip (); at < text.size () && (text[at] == '*' || text[at] == '/'); skip ()) {
		const char op = text[at++];
		const int right = factor ();
		value = op == '*' ? value * right : (right != 0 ? value / right : 0);
	    }
	    return value;
	};
	int value = product ();
	for (skip (); at < text.size () && (text[at] == '+' || text[at] == '-'); skip ()) {
	    const char op = text[at++];
	    const int right = product ();
	    value = op == '+' ? value + right : value - right;
	}
	return value;
    };

    return sum ();
}
} // namespace

std::string ShaderUnit::applyGeometryOutputNames (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Vertex || !this->m_feedsGeometry) {
	return source;
    }

    static const std::regex varyingDecl (R"(\bvarying\s+\w+\s+([A-Za-z_]\w*)\s*;)");
    std::set<std::string> names;
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), varyingDecl); it != std::sregex_iterator ();
	 ++it) {
	names.insert ((*it)[1].str ());
    }
    for (const auto& name : names) {
	source = std::regex_replace (source, std::regex ("\\b" + name + "\\b"), GEOMETRY_INPUT_PREFIX + name);
    }

    return source;
}

std::string ShaderUnit::applyGeometryDialect (std::string source) const {
    // [maxvertexcount(N)] -> the input and output layouts, N worked out with the combos this unit gets
    static const std::regex maxVertexCount (R"(\[\s*maxvertexcount\s*\(([^\]]*)\)\s*\])");
    std::smatch match;
    if (std::regex_search (source, match, maxVertexCount)) {
	std::string expression = match[1].str ();
	static const std::regex identifier (R"([A-Za-z_]\w*)");
	std::string resolved;
	auto last = expression.cbegin ();
	for (auto it = std::sregex_iterator (expression.cbegin (), expression.cend (), identifier);
	     it != std::sregex_iterator (); ++it) {
	    resolved.append (last, (*it)[0].first);
	    resolved += std::to_string (this->defineValue ((*it)[0].str (), source));
	    last = (*it)[0].second;
	}
	resolved.append (last, expression.cend ());
	size_t at = 0;
	const int count = std::max (1, evaluateCount (resolved, at));

	source.replace (
	    match.position (0), match.length (0),
	    "layout(points) in;\nlayout(triangle_strip, max_vertices = " + std::to_string (count) + ") out;\n"
	);
    }

    // the vertex stage's outputs arrive as arrays under the names applyGeometryOutputNames gave them (the same
    // names are often outputs here too, v_Color), gl_Position is a builtin on both sides
    source = std::regex_replace (source, std::regex (R"(\b(in|out)\s+vec4\s+gl_Position\s*;)"), "");
    source = std::regex_replace (
	source, std::regex (R"((^|\n)(\s*)in\s+(\w+)\s+(\w+)\s*;)"), "$1$2in $3 " GEOMETRY_INPUT_PREFIX "$4[];"
    );

    source = std::regex_replace (
	source, std::regex (R"(\bIN\s*\[([^\]]+)\]\s*\.\s*gl_Position\b)"), "gl_in[$1].gl_Position"
    );
    source = std::regex_replace (
	source, std::regex (R"(\bIN\s*\[([^\]]+)\]\s*\.\s*(\w+))"), GEOMETRY_INPUT_PREFIX "$2[$1]"
    );

    // "PS_INPUT v;" is the vertex being written: its fields are the outputs themselves
    static const std::regex vertexDecl (R"(\bPS_INPUT\s+(\w+)\s*;)");
    if (std::regex_search (source, match, vertexDecl)) {
	const std::string name = match[1].str ();
	source = std::regex_replace (source, vertexDecl, "");
	source = std::regex_replace (source, std::regex ("(^|[^\\w.])" + name + "\\s*\\.\\s*(\\w+)"), "$1$2");
	source = std::regex_replace (
	    source, std::regex ("\\bOUT\\s*\\.\\s*Append\\s*\\(\\s*" + name + "\\s*\\)\\s*;"), "EmitVertex();"
	);
    }
    source = std::regex_replace (source, std::regex (R"(\bOUT\s*\.\s*RestartStrip\s*\(\s*\)\s*;)"), "EndPrimitive();");

    // HLSL lets a loop body redeclare its counter ("for (int s ...) { float s = ...") and use the new one from then
    // on, GLSL doesn't: the counter gets another name in the loop header
    static const std::regex loopHeader (R"(for\s*\(\s*int\s+(\w+)\s*=[^;]*;[^;]*;[^)]*\))");
    std::string result;
    auto last = source.cbegin ();
    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), loopHeader); it != std::sregex_iterator ();
	 ++it) {
	const std::string counter = (*it)[1].str ();
	const size_t bodyStart = static_cast<size_t> ((*it)[0].second - source.cbegin ());
	const size_t bodyEnd = std::min (source.size (), source.find ('}', bodyStart));
	const std::string body = source.substr (bodyStart, bodyEnd - bodyStart);
	std::string header = (*it)[0].str ();

	if (std::regex_search (body, std::regex ("\\b(?:float|int|uint|vec[234])\\s+" + counter + "\\s*="))) {
	    header = std::regex_replace (header, std::regex ("\\b" + counter + "\\b"), "wpeLoop_" + counter);
	}

	result.append (last, (*it)[0].first);
	result += header;
	last = (*it)[0].second;
    }
    result.append (last, source.cend ());

    return result;
}

void ShaderUnit::parseComboConfiguration (const std::string& content, const int defaultValue) {
    // "require"/"requireany" on a combo are editor-only, they decide whether the editor shows the
    // option. wallpaper64.exe (sub_140133B60) never reads them, it just takes the default
    JSON data;
    try {
	data = JSON::parseAsset (content);
    } catch (const std::exception& e) {
	sLog.error ("Cannot parse combo metadata in shader ", this->m_file, ": ", e.what ());
	return;
    }
    const auto combo = data.require<std::string> ("combo", "cannot parse combo information");
    // "type" is ignored - appears to be editor-only metadata
    const auto defvalue = data.find ("default");

    const auto entry = this->m_combos.find (combo);
    const auto entryOverride = this->m_overrideCombos.find (combo);

    this->m_usedCombos.emplace (combo, true);

    // not predefined anywhere -> fall back to the JSON's own default value
    if (entry == this->m_combos.end () && entryOverride == this->m_overrideCombos.end ()) {
	// like WE: whole numbers (floats included) are taken as-is, a missing or any other default is 0
	int value = defaultValue;

	if (defvalue != data.end () && defvalue->is_number_integer ()) {
	    value = defvalue->get<int> ();
	} else if (defvalue != data.end () && defvalue->is_number_float ()) {
	    const double number = defvalue->get<double> ();

	    if (std::trunc (number) == number) {
		value = static_cast<int> (number);
	    }
	}

	this->m_discoveredCombos.emplace (combo, value);
    }
}

void ShaderUnit::parseParameterConfiguration (
    const std::string& type, const std::string& name, const std::string& content
) {
    // WE only takes the comment as metadata when it parses to a JSON object and skips the uniform silently
    // otherwise (sub_14016CE60), a plain "// note" behind a uniform is common
    JSON data;
    try {
	data = JSON::parseAsset (content);
    } catch (const std::exception&) {
	return;
    }

    if (!data.is_object ()) {
	return;
    }

    const auto material = data.optional ("material");
    const auto defvalue = data.optional ("default");
    const auto combo = data.find ("combo");

    auto constant = this->m_constants.end ();

    if (material.has_value ()) {
	constant = this->m_constants.find (*material);
    }

    if (constant == this->m_constants.end () && !defvalue.has_value ()) {
	if (type != "sampler2D") {
	    sLog.exception ("Cannot parse parameter data for ", name, " in shader ", this->m_file);
	}
    }

    Variables::ShaderVariable* parameter = nullptr;

    if (type == "vec4") {
	parameter
	    = new Variables::ShaderVariableVector4 (VectorBuilder::parse<glm::vec4> (defvalue->get<std::string> ()));
    } else if (type == "vec3") {
	parameter = new Variables::ShaderVariableVector3 (VectorBuilder::parse<glm::vec3> (*defvalue));
    } else if (type == "vec2") {
	parameter = new Variables::ShaderVariableVector2 (VectorBuilder::parse<glm::vec2> (*defvalue));
    } else if (type == "float") {
	if (defvalue->is_string ()) {
	    parameter = new Variables::ShaderVariableFloat (std::stoi (defvalue->get<std::string> ()));
	} else {
	    parameter = new Variables::ShaderVariableFloat (defvalue->get<float> ());
	}
    } else if (type == "int") {
	if (defvalue->is_string ()) {
	    parameter = new Variables::ShaderVariableInteger (std::stoi (defvalue->get<std::string> ()));
	} else {
	    parameter = new Variables::ShaderVariableInteger (defvalue->get<int> ());
	}
    } else if (type == "sampler2D" || type == "sampler2DComparison") {
	const auto textureName = data.find ("default");
	const auto requireany = data.find ("requireany");
	const auto require = data.find ("require");
	constexpr std::string_view prefix = "g_Texture";
	size_t index = 0;
	const char* digits = name.data () + std::min (name.size (), prefix.size ());
	const char* nameEnd = name.data () + name.size ();

	if (!name.starts_with (prefix) || std::from_chars (digits, nameEnd, index).ptr != nameEnd) {
	    sLog.error ("Cannot determine texture slot for ", name, " in shader ", this->m_file);
	    return;
	}

	if (combo != data.end ()) {
	    // TODO: CLEANUP HOW THIS IS DETERMINED FIRST
	    const auto textureSlotUsed
		= this->m_passTextures.contains (index) || this->m_overrideTextures.contains (index);
	    bool isRequired = false;
	    int comboValue = 1;

	    if (textureSlotUsed) {
		// texture already exists, so the combo must be set; these tend to have no default value
		isRequired = true;
	    } else if (require != data.end ()) {
		if (requireany != data.end () && requireany->get<bool> ()) {
		    // requireany: any one mismatching value makes this required (OR semantics)
		    for (const auto& item : require->items ()) {
			const std::string& macro = item.key ();
			const auto it = this->m_combos.find (macro);

			if (it == this->m_combos.end () || this->m_overrideCombos.contains (macro)
			    || it->second != item.value ()) {
			    isRequired = true;
			    break;
			}
		    }
		} else {
		    isRequired = true;

		    // require without requireany: every listed value must match (AND semantics)
		    for (const auto& item : require->items ()) {
			const std::string& macro = item.key ();
			const auto it = this->m_combos.find (macro);

			// a missing macro is fine here, only the value comparison matters
			if ((it != this->m_combos.end () || this->m_overrideCombos.contains (macro))
			    && it->second == item.value ()) {
			    isRequired = false;
			    break;
			}
		    }
		}
	    }

	    if (isRequired && !textureSlotUsed) {
		if (!defvalue.has_value ()) {
		    isRequired = false;
		} else {
		    if (this->m_combos.contains (*combo) || this->m_overrideCombos.contains (*combo)) {
			isRequired = false;
		    } else if (defvalue->is_string ()) {
			comboValue = std::stoi (defvalue->get<std::string> ().c_str ());
		    } else if (defvalue->is_number ()) {
			comboValue = *defvalue;
		    } else {
			sLog.exception (
			    "Cannot determine default value for combo ", combo->get<std::string> (),
			    " because it's not specified by the shader and is not given a default value: ", this->m_file
			);
		    }
		}
	    }

	    if (isRequired) {
		this->m_discoveredCombos.emplace (*combo, comboValue);
		this->m_usedCombos.emplace (*combo, true);
	    }
	}

	// Some shaders (e.g. effects/refract.frag's normal map) declare `"default":""` on purpose -
	// an explicitly empty default means "no texture unless the object's own effect config
	// supplies one", not "a texture literally named the empty string". Registering it anyway
	// sent every such object into a doomed asset lookup for a blank path on every use of that
	// shader, whether or not the combo gating it was even active.
	if (textureName != data.end () && !textureName->get<std::string> ().empty ()) {
	    this->m_defaultTextures.emplace (index, *textureName);
	}

	if (const auto formatCombo = data.find ("formatcombo");
	    formatCombo != data.end () && formatCombo->is_boolean () && formatCombo->get<bool> ()) {
	    this->m_formatComboSlots.insert (static_cast<int> (index));
	}

	if (const auto components = data.find ("components"); components != data.end () && components->is_array ()) {
	    auto& combos = this->m_componentCombos[static_cast<int> (index)];

	    for (const auto& component : *components) {
		const auto componentCombo = component.is_object () ? component.find ("combo") : component.end ();
		combos.push_back (
		    componentCombo != component.end () && componentCombo->is_string ()
			? componentCombo->get<std::string> ()
			: std::string ()
		);
	    }
	}

	return;
    } else {
	sLog.error ("Unknown parameter type: ", type, " for ", name, " in shader ", this->m_file);
	return;
    }

    if (material.has_value () && parameter != nullptr) {
	parameter->setIdentifierName (*material);
	parameter->setName (name);

	this->m_parameters.push_back (parameter);
    }
}

const ComboMap& ShaderUnit::getCombos () const { return this->m_combos; }

const ComboMap& ShaderUnit::getDiscoveredCombos () const { return this->m_discoveredCombos; }

void ShaderUnit::linkToUnit (const ShaderUnit* unit) { this->m_link = unit; }

void ShaderUnit::feedGeometryStage () { this->m_feedsGeometry = true; }

const ShaderUnit* ShaderUnit::getLinkedUnit () const { return this->m_link; }

const std::string& ShaderUnit::compile () {
    if (!this->m_final.empty ()) {
	return this->m_final;
    }

    this->m_final = SHADER_HEADER (this->m_file);

    // GLSL has no builtin log10 (unlike HLSL), so some shaders provide their own under "#if GLSL"
    // (which is always true here). Adding a blanket compatibility macro would get expanded right
    // over such a shader's own function definition and mangle it, so only add it when the shader
    // doesn't already define log10 itself.
    static const std::regex log10Definition (
	R"(\b(?:void|float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|bvec[234]|mat[234](?:x[234])?)\s+log10\s*\()"
    );
    // memoized per source, compile() runs again for every pass a text layer rebuilds
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, bool> definesLog10;
    static std::unordered_map<std::string, std::string> compatCache;

    bool hasLog10 = false;
    {
	std::lock_guard lock (cacheMutex);
	auto found = definesLog10.find (this->m_content);

	if (found == definesLog10.end ()) {
	    found = definesLog10.emplace (this->m_content, std::regex_search (this->m_content, log10Definition)).first;
	}

	hasLog10 = found->second;
    }

    if (!hasLog10) {
	this->m_final += "#define log10(x) (log2(x) * 0.301029995663981)\n";
    }

    if (this->m_feedsGeometry) {
	this->m_final += "// outputs renamed for the geometry stage\n";
    }

    if (this->m_type == GLSLContext::UnitType_Fragment) {
	this->m_final += FRAGMENT_SHADER_DEFINES;
    } else if (this->m_type == GLSLContext::UnitType_Geometry) {
	this->m_final += GEOMETRY_SHADER_DEFINES;
    } else {
	this->m_final += VERTEX_SHADER_DEFINES;
    }

    std::map<std::string, bool> addedCombos;

    for (const auto& [name, value] : this->m_overrideCombos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, value);
	    addedCombos.emplace (uppercase, true);
	}
    }

    for (const auto& [name, value] : this->m_combos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, value);
	    addedCombos.emplace (uppercase, true);
	}
    }

    for (const auto& [name, value] : this->m_discoveredCombos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, value);
	    addedCombos.emplace (uppercase, true);
	}
    }

    if (this->m_link != nullptr) {
	for (const auto& [name, value] : this->m_link->getCombos ()) {
	    std::string uppercase;
	    std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	    if (!addedCombos.contains (uppercase)) {
		this->m_final += DEFINE_COMBO (uppercase, value);
		addedCombos.emplace (uppercase, true);
	    }
	}

	for (const auto& [name, value] : this->m_link->getDiscoveredCombos ()) {
	    std::string uppercase;
	    std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	    if (!addedCombos.contains (uppercase)) {
		this->m_final += DEFINE_COMBO (uppercase, value);
		addedCombos.emplace (uppercase, true);
	    }
	}
    }

    // the header above already encodes the unit type and every define, so it works as the key
    std::string cacheKey = this->m_final;
    cacheKey += '\x1f';
    cacheKey += this->m_preprocessed;
    cacheKey += '\x1f';

    if (this->m_link != nullptr) {
	cacheKey += this->m_link->m_preprocessed;
    }

    for (const auto& [name, value] : this->m_combos) {
	cacheKey += '\x1f';
	cacheKey += name;
	cacheKey += '=';
	cacheKey += std::to_string (value);
    }

    {
	std::lock_guard lock (cacheMutex);

	if (const auto cached = compatCache.find (cacheKey); cached != compatCache.end ()) {
	    this->m_final += cached->second;

	    return this->m_final;
	}
    }

    const std::string compat = this->applyNonConstantConstCompatibility (this->applyBoolArithmeticCompatibility (
	this->applyFloatConditionCompatibility (this->applyVectorTruncationCompatibility (
	    this->applyFragmentVaryingShadowCompatibility (this->applyFragmentTexCoordCompatibility (
		this->applyNarrowFragmentVaryingCompatibility (this->applyLinkedVaryingCompatibility (
		    this->applyNonConstantGlobalConstCompatibility (this->applyDirectiveSemicolonCompatibility (
			this->applyHlslAttributeCompatibility (this->applyPackedFloatArrayCompatibility (
			    this->m_type == GLSLContext::UnitType_Geometry
				? this->applyGeometryDialect (this->m_preprocessed)
				: this->applyGeometryOutputNames (this->m_preprocessed)
			))
		    ))
		))
	    ))
	))
    ));

    {
	std::lock_guard lock (cacheMutex);
	compatCache.emplace (std::move (cacheKey), compat);
    }

    this->m_final += compat;

    // actual GLSL compilation happens in the pass, which has the context this unit doesn't
    return this->m_final;
}

const std::vector<Variables::ShaderVariable*>& ShaderUnit::getParameters () const { return this->m_parameters; }
const TextureMap& ShaderUnit::getTextures () const { return this->m_defaultTextures; }

const std::set<int>& ShaderUnit::getFormatComboSlots () const { return this->m_formatComboSlots; }

const std::map<int, std::vector<std::string>>& ShaderUnit::getComponentCombos () const {
    return this->m_componentCombos;
}
