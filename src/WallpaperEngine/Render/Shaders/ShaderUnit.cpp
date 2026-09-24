#include "ShaderUnit.h"

#include "WallpaperEngine/Logging/Log.h"
#include <cctype>
#include <charconv>
#include <cmath>
#include <exception>
#include <mutex>
#include <optional>
#include <regex>
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
	  "#define atan2 atan\n"                                                                                       \
	  "#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))\n"                                                              \
	  "#define ddx dFdx\n"                                                                                         \
	  "#define ddy(x) dFdy(-(x))\n"                                                                                \
	  "#define GLSL 1\n\n";
#define FRAGMENT_SHADER_DEFINES                                                                                        \
    "out vec4 out_FragColor;\n"                                                                                        \
    "#define varying in\n"
#define VERTEX_SHADER_DEFINES                                                                                          \
    "#define attribute in\n"                                                                                           \
    "#define varying out\n"
#define DEFINE_COMBO(name, value) "#define " + name + " " + std::to_string (value) + "\n";

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Render::Shaders;

namespace {
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
    this->m_includes = "";

    this->preprocessIncludes ();
    this->preprocessRequires ();
    this->preprocessVariables ();
    this->preprocessBalanceConditionals ();
    this->preprocessSwizzledDeclarations ();

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
    size_t start = 0, end = 0;
    while ((start = this->m_preprocessed.find ("#include", end)) != std::string::npos) {
	const auto parsed = includeFilename (this->m_preprocessed, start);

	// comment out just the "#i" so the string length/offsets are unaffected
	this->m_preprocessed = this->m_preprocessed.replace (start, 2, "//");
	end = start;

	if (!parsed.has_value ()) {
	    sLog.error ("Malformed #include directive in shader ", this->m_file);
	    continue;
	}

	const std::string& filename = *parsed;

	// a missing include isn't necessarily an error - it may come from commented-out content
	std::string content;

	try {
	    content += "// begin of include from file ";
	    content += filename;
	    content += "\n";
	    content += this->m_assetLocator.includeShader (filename);
	    content += "\n// end of included from file ";
	    content += filename;
	    content += "\n";
	} catch (AssetLoadException&) {
	    content += "// tried including file ";
	    content += filename;
	    content += " but was not found\n";
	}

	this->m_includes += content;
    }

    // resolve #include directives found inside already-included content too
    end = 0;

    while ((start = this->m_includes.find ("#include", end)) != std::string::npos) {
	const size_t lineEnd = this->m_includes.find_first_of ('\n', start);
	const auto parsed = includeFilename (this->m_includes, start);
	end = start;

	if (!parsed.has_value ()) {
	    sLog.error ("Malformed #include directive in an include of shader ", this->m_file);
	    this->m_includes = this->m_includes.replace (start, 2, "//");
	    continue;
	}

	const std::string& filename = *parsed;

	// a missing include isn't necessarily an error - it may come from commented-out content
	std::string content;

	try {
	    content = "// begin of include from file ";
	    content += filename;
	    content += "\n";
	    content += this->m_assetLocator.includeShader (filename);
	    content += "\n// end of included from file ";
	    content += filename;
	    content += "\n";
	} catch (AssetLoadException&) {
	    content = "// tried including file ";
	    content += filename;
	    content += " but was not found\n";
	}

	this->m_includes = this->m_includes.replace (start, lineEnd - start, content);
    }

    // place the accumulated include contents right before the main function
    end = 0;
    bool includesAdded = false;

    while ((start = this->m_preprocessed.find (" main", end)) != std::string::npos) {
	char value = this->m_preprocessed.at (start + 5);

	end = start + 5;

	if (value != ' ' && value != '(') {
	    continue;
	}

	size_t lastAttribute = this->m_preprocessed.rfind ("attribute", start);
	size_t lastVarying = this->m_preprocessed.rfind ("varying", start);
	size_t lastUniform = this->m_preprocessed.rfind ("uniform", start);
	size_t latest = lastAttribute;

	if (latest == std::string::npos) {
	    latest = lastVarying;
	} else if (latest < lastVarying && lastVarying != std::string::npos) {
	    latest = lastVarying;
	}

	if (latest == std::string::npos) {
	    latest = lastUniform;
	} else if (latest < lastUniform && lastUniform != std::string::npos) {
	    latest = lastUniform;
	}

	if (latest < start) {
	    // find the end of the current line
	    latest = this->m_preprocessed.find ('\n', latest);
	} else {
	    // find the end of the previous line
	    latest = this->m_preprocessed.rfind ('\n', start);
	}

	// start points at the end of the previous line, used below to place the includes
	start = this->m_preprocessed.rfind ('\n', start);

	// tracks nested #if/#endif so the includes can be moved before the start of the enclosing chain
	std::stack<size_t> ifdefStack;

	const std::regex ifdef (R"((#if|#endif))");
	std::smatch match;
	size_t current = 0;

	while (
	    std::regex_search (this->m_preprocessed.cbegin () + current, this->m_preprocessed.cend (), match, ifdef)) {
	    current += match.position ();

	    if (this->m_preprocessed.substr (current, 3) == "#if") {
		ifdefStack.push (current++); // advance past this match so regex_search doesn't rematch it
		continue;
	    }

	    current++; // same reason: advance past this match

	    // an unmatched #endif is most likely a syntax error; ignored for now
	    if (ifdefStack.empty ()) {
		continue;
	    }

	    size_t stackStart = ifdefStack.top ();
	    ifdefStack.pop ();

	    if (latest > stackStart && latest <= current) {
		// insertion point is inside a conditional block - move before the #if so includes are
		// available to all branches (e.g. genericropeparticle.vert has #if GS_ENABLED wrapping two main()s)
		size_t beforeIfdef = this->m_preprocessed.rfind ('\n', stackStart);
		latest = (beforeIfdef != std::string::npos) ? beforeIfdef : 0;
	    }
	}

	// TODO: IS THIS GOOD ENOUGH? MAYBE WE SHOULD BE GETTING THE FIRST #IF BLOCK INSTEAD?
	latest = std::min (latest, start);

	this->m_preprocessed.insert (latest + 1, this->m_includes + '\n');
	includesAdded = true;
	break;
    }

    if (!includesAdded) {
	sLog.exception ("Could not find where to place includes for shader unit ", this->m_file);
    }
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
	    // inserted directly here, not appended to m_includes - that was already consumed by preprocessIncludes
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
    // Wallpaper Engine generates this from the scene's light sources; since light objects
    // aren't supported yet, stub it out with no dynamic light contribution.
    return "// begin of generated module LightingV1\n"
	   "vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal, vec3 viewDir,\n"
	   "    vec3 specularTint, vec3 baseReflectance, float roughness, float metallic)\n"
	   "{\n"
	   "    return vec3(0.0);\n"
	   "}\n"
	   "// end of generated module LightingV1\n";
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

    std::string result;
    size_t last = 0;
    bool changed = false;

    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), narrowAssign);
	 it != std::sregex_iterator (); ++it) {
	const int targetWidth = (*it)[1].str ().back () - '0';
	const size_t exprStart = it->position (2);
	const std::string expr = (*it)[2].str ();

	std::string fixed;
	int depth = 0;

	for (size_t i = 0; i < expr.size ();) {
	    const char c = expr[i];

	    if (c == '(' || c == '[') {
		depth++;
	    } else if (c == ')' || c == ']') {
		depth--;
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
	    const bool accessed = after < expr.size () && (expr[after] == '.' || expr[after] == '(' || expr[after] == '[');
	    const auto found = widths.find (ident);

	    if (depth == 0 && !member && !accessed && found != widths.end () && found->second > targetWidth) {
		fixed += swizzles[targetWidth];
		changed = true;
	    }

	    i = end;
	}

	result.append (source, last, exprStart - last);
	result += fixed;
	last = exprStart + expr.size ();
    }

    if (!changed) {
	return source;
    }

    result.append (source, last, std::string::npos);
    sLog.out ("Applied vector truncation compatibility in shader ", this->m_file);

    return result;
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

	if (found == onlyFloat.end()) {
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

std::string ShaderUnit::applyNonConstantConstCompatibility (std::string source) const {
    // locals only, globals sit at column 0
    static const std::regex constLocal (R"((^|\n)([ \t]+)const\s+([^;=]+=([^;]*);))");
    static const std::regex nonConstant (R"(\b(?:texSample2D\w*|texture\w*|g_\w+|v_\w+|wpeVar_\w+)\b)");

    std::string result;
    size_t count = 0;
    auto last = source.cbegin ();

    for (auto it = std::sregex_iterator (source.cbegin (), source.cend (), constLocal); it != std::sregex_iterator ();
	 ++it) {
	const auto& match = *it;

	if (!std::regex_search (match[4].first, match[4].second, nonConstant)) {
	    continue;
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
    JSON data;
    try {
	data = JSON::parseAsset (content);
    } catch (const std::exception& e) {
	sLog.error ("Cannot parse parameter metadata for ", name, " in shader ", this->m_file, ": ", e.what ());
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

    if (this->m_type == GLSLContext::UnitType_Fragment) {
	this->m_final += FRAGMENT_SHADER_DEFINES;
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

    const std::string compat = this->applyNonConstantConstCompatibility (this->applyFloatConditionCompatibility (
	this->applyVectorTruncationCompatibility (this->applyFragmentVaryingShadowCompatibility (
	    this->applyFragmentTexCoordCompatibility (this->applyLinkedVaryingCompatibility (this->m_preprocessed))
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
