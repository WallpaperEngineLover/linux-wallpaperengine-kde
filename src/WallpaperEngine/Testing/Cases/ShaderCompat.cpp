#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <string>
#include <utility>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Shaders/ShaderCache.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Shaders;
using Catch::Matchers::ContainsSubstring;

namespace {
struct Compiled {
    Assets::AssetLocator locator { std::make_unique<FileSystem::Container> () };
    ShaderConstantMap constants;
    TextureMap passTextures, overrideTextures;
    ComboMap combos, overrideCombos;
    std::unique_ptr<ShaderUnit> unit;
    bool cacheWasEnabled = ShaderCache::get ().enabled ();

    explicit Compiled (
	const std::string& source, const GLSLContext::UnitType type = GLSLContext::UnitType_Fragment,
	ComboMap withCombos = {}
    ) : combos (std::move (withCombos)) {
	ShaderCache::get ().setEnabled (false);
	unit = std::make_unique<ShaderUnit> (
	    type, "compat.frag", source, locator, constants, passTextures, overrideTextures, combos, overrideCombos
	);
    }

    ~Compiled () { ShaderCache::get ().setEnabled (cacheWasEnabled); }

    [[nodiscard]] const std::string& text () const { return unit->compile (); }
};
}

TEST_CASE ("Includes that cannot be resolved leave the rest of the shader alone", "[shadercompat]") {
    Compiled shader (
	"#include \"missing.h\"\n#include \"missing.h\"\n#include broken\nvarying vec2 v_TexCoord;\nvoid main () { }\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("void main"));
}

TEST_CASE ("Malformed and unknown requires are dropped", "[shadercompat]") {
    Compiled shader ("#require \n#require Nothing\n#require   \nvoid main () { }\n");

    CHECK_THAT (shader.text (), ContainsSubstring ("void main"));
}

TEST_CASE ("LightingV1 writes one block per light kind and shadow variant", "[shadercompat]") {
    Compiled shader (
	"#require LightingV1\nvoid main () { }\n", GLSLContext::UnitType_Fragment,
	{ { "LIGHTING", 1 },
	  { "LIGHTS_POINT", 2 },
	  { "LIGHTS_POINT_SHADOW", 1 },
	  { "LIGHTS_SPOT", 5 },
	  { "LIGHTS_SPOT_SHADOW_COOKIE", 1 },
	  { "LIGHTS_SPOT_COOKIE", 1 },
	  { "LIGHTS_SPOT_SHADOW", 1 },
	  { "LIGHTS_TUBE", 2 },
	  { "LIGHTS_DIRECTIONAL", 2 },
	  { "LIGHTS_DIRECTIONAL_SHADOW", 1 } }
    );
    const auto& text = shader.text ();

    CHECK_THAT (text, ContainsSubstring ("g_LPoint_Color"));
    CHECK_THAT (text, ContainsSubstring ("g_LSpot_Color"));
    CHECK_THAT (text, ContainsSubstring ("g_LTube_OriginA"));
    CHECK_THAT (text, ContainsSubstring ("colorCookie"));
    CHECK_THAT (text, ContainsSubstring ("spotCookie"));
}

TEST_CASE ("An extra #endif is commented out", "[shadercompat]") {
    Compiled shader ("#if 1\n#endif\n#endif\nvoid main () { }\n");

    CHECK_THAT (shader.text (), ContainsSubstring ("//ndif"));
}

TEST_CASE ("A swizzle on a declaration name and on a scalar is dropped", "[shadercompat]") {
    Compiled shader (
	"uniform vec4 g_Tint.xyz;\nvoid main () { float s = 1.0; float t = s.x + s.xx.x; vec3 v = vec3(s.xxx); }\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("uniform vec4 g_Tint;"));
    CHECK_THAT (shader.text (), ContainsSubstring ("vec3 (s)"));
}

TEST_CASE ("A float used as a condition is compared with zero", "[shadercompat]") {
    Compiled shader (
	"uniform float g_Amount;\nuniform float g_Other;\n"
	"void main () {\n"
	"  float a = g_Amount ? 1.0 : 0.0;\n"
	"  float b = (g_Amount) ? 1.0 : 0.0;\n"
	"  float c = g_Other == g_Amount ? 1.0 : 0.0;\n"
	"  float d = 1.0, e = g_Amount ? 2.0 : 3.0;\n"
	"  bool x = g_Amount && g_Other ? true : false;\n"
	"  if (g_Amount) { a = 2.0; }\n"
	"  if (g_Amount || g_Other) { a = 3.0; }\n"
	"  float f = ? g_Amount;\n"
	"  g_Amount ? a : b;\n"
	"}\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("(g_Amount != 0.0)"));
}

TEST_CASE ("A bool assigned to a number is cast", "[shadercompat]") {
    Compiled shader (
	"void main () { bool flag = true; int count = 0; uint mask = 0u; float ratio = 1.0; count = flag; mask += flag;"
	" ratio *= flag; vec2 other = vec2 (1.0); other = flag; }\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("count = int(flag)"));
    CHECK_THAT (shader.text (), ContainsSubstring ("mask += uint(flag)"));
}

TEST_CASE ("A vertex varying the fragment stage reads wider is widened", "[shadercompat]") {
    Compiled fragment (
	"varying vec4 v_Wide;\nvarying vec4 v_Same;\nvarying vec4 v_Branches;\nvarying vec4 v_Missing;\n"
	"void main () { }\n"
    );
    Compiled vertex (
	"varying vec2 v_Wide;\nvarying vec4 v_Same;\n#if A\nvarying vec2 v_Branches;\n#else\nvarying vec4 "
	"v_Branches;\n#endif\n"
	"varying vec3 v_Other;\n"
	"void main () {\n  v_Wide = a_TexCoord.xy;\n  v_Wide = a_Other;\n}\n",
	GLSLContext::UnitType_Vertex
    );
    vertex.unit->linkToUnit (fragment.unit.get ());

    const auto& text = vertex.text ();

    CHECK (vertex.unit->getLinkedUnit () == fragment.unit.get ());
    CHECK_THAT (text, ContainsSubstring ("varying vec4 v_Wide;"));
    CHECK_THAT (text, ContainsSubstring ("v_Wide = vec4(a_TexCoord.xy, 0.0, 1.0);"));
}

TEST_CASE ("A fragment varying that is narrower than the vertex one is copied", "[shadercompat]") {
    Compiled vertex (
	"varying vec4 v_Coord;\nvarying vec3 v_Normal;\nvoid main () { v_Coord = vec4 (1.0); v_Normal = vec3 (1.0); "
	"}\n",
	GLSLContext::UnitType_Vertex
    );
    Compiled fragment (
	"varying vec2 v_Coord;\nvarying vec3 v_Normal;\nvarying vec2 v_Lost;\n"
	"void main () { gl_FragColor = vec4 (v_Coord, 0.0, 1.0); }\n"
    );
    fragment.unit->linkToUnit (vertex.unit.get ());

    CHECK_THAT (fragment.text (), ContainsSubstring ("void main"));
}

TEST_CASE ("A wide TexCoord next to CAST2 is cut to two components", "[shadercompat]") {
    Compiled shader (
	"varying vec4 v_TexCoord;\nvoid main () { vec2 a = v_TexCoord - CAST2(1.0); vec2 b = CAST2(0.5) * v_TexCoord; "
	"}\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("v_TexCoord.xy"));
}

TEST_CASE ("A fragment varying the shader writes goes through a copy", "[shadercompat]") {
    Compiled shader (
	"varying vec3 v_Pos;\nvarying float v_Fade;\nvarying vec2 v_Read;\n"
	"void main () { v_Pos.x = 1.0; v_Fade += 0.5; vec2 v_Local = v_Read; v_Local = vec2 (0.0); ++v_Fade;"
	" gl_FragColor = vec4 (v_Pos, v_Fade); }\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("wpeVar_v_Pos"));
    CHECK_THAT (shader.text (), ContainsSubstring ("wpeVar_v_Fade"));
}

TEST_CASE ("Directive semicolons and HLSL attributes are dropped", "[shadercompat]") {
    Compiled shader (
	"#if 1 ;\n#elif 0 ;\n#endif\nvoid main () { [unroll] for (int i = 0; i < 2; i++) { } [loop(4)] while (false) { "
	"}"
	" [branch] if (true) { } }\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("#if 1\n"));
    CHECK_THAT (shader.text (), !ContainsSubstring ("[unroll]"));
}

TEST_CASE ("A packed float array indexed twice is flattened", "[shadercompat]") {
    Compiled shader (
	"uniform float g_Spectrum[16];\nuniform float g_Odd[3];\n"
	"void main () {\n"
	"  float a = g_Spectrum[2][1];\n"
	"  float b = g_Spectrum[i + 1][j % 4];\n"
	"  float c = g_Spectrum[(k)][ (m % 3) ];\n"
	"  float d = g_Spectrum[1];\n"
	"  float e = g_Spectrum[unclosed;\n"
	"  float f = g_Odd[1][1];\n"
	"  float g = g_Spectrum[4] [ 2 ];\n"
	"  float h = g_Spectrum[1][\n"
	"}\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("g_Spectrum[int(2) * 4 + int(1)]"));
    CHECK_THAT (shader.text (), ContainsSubstring ("fmod(float(j ), float( 4))"));
}

TEST_CASE ("A const initialised from a runtime value loses its const", "[shadercompat]") {
    Compiled shader (
	"varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
	"void main () {\n"
	"  const vec4 sampled = texSample2D(g_Texture0, v_TexCoord);\n"
	"  const float later = sampled.x * 2.0;\n"
	"  const float fixed = 3.0;\n"
	"  float plain = 1.0;\n"
	"  const float fromPlain = plain + 1.0;\n"
	"  const float fromUniform = g_Time;\n"
	"}\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("  vec4 sampled = texSample2D"));
    CHECK_THAT (shader.text (), ContainsSubstring ("const float fixed = 3.0;"));
}

TEST_CASE ("A global const initialised from a runtime value moves into main", "[shadercompat]") {
    Compiled shader (
	"varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
	"const vec4 sampled = texSample2D(g_Texture0, v_TexCoord);\n"
	"const float fixed = 2.0;\n"
	"const float derived = sampled.x;\n"
	"#if A\n"
	"#if B\n"
	"const vec4 inner = texSample2D(g_Texture0, v_TexCoord);\n"
	"#else\n"
	"const vec4 other = texSample2D(g_Texture0, v_TexCoord);\n"
	"#endif\n"
	"#endif\n"
	"vec4 helper () {\n  const vec4 local = texSample2D(g_Texture0, v_TexCoord);\n  return local;\n}\n"
	"void main () { gl_FragColor = helper (); }\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("vec4 sampled;"));
    CHECK_THAT (shader.text (), ContainsSubstring ("const float fixed = 2.0;"));
}

TEST_CASE ("A global const with two main functions stays where it is", "[shadercompat]") {
    Compiled shader (
	"uniform sampler2D g_Texture0;\nconst vec4 sampled = texSample2D(g_Texture0, vec2 (0.0));\n"
	"void main () { }\nvoid main () { }\n"
    );

    CHECK_THAT (shader.text (), ContainsSubstring ("const vec4 sampled"));
}

TEST_CASE ("A geometry shader is translated to GLSL", "[shadercompat]") {
    ComboMap combos { { "SEGMENTS", 3 } };
    Compiled vertex (
	"varying vec4 v_Color;\nvarying vec2 v_Size;\nvoid main () { v_Color = vec4 (1.0); }\n",
	GLSLContext::UnitType_Vertex
    );
    vertex.unit->feedGeometryStage ();

    CHECK_THAT (vertex.text (), ContainsSubstring ("outputs renamed"));

    Compiled geometry (
	"in vec4 gl_Position;\nin vec4 v_Color;\nout vec4 v_Color2;\nstruct PS_INPUT { vec4 gl_Position; vec4 v_Color; "
	"};\n"
	"[maxvertexcount(SEGMENTS * (2 + 1) - 4 / 2)]\n"
	"void main (point VS_OUTPUT IN[1], inout TriangleStream<PS_INPUT> OUT) {\n"
	"  PS_INPUT v;\n"
	"  for (int s = 0; s < SEGMENTS; s++) {\n"
	"    float s = 1.0;\n"
	"    v.gl_Position = IN[0].gl_Position;\n"
	"    v.v_Color = IN[0].v_Color;\n"
	"    OUT.Append (v);\n"
	"  }\n"
	"  for (int t = 0; t < 2; t++) { v.v_Color = vec4 (0.0); }\n"
	"  OUT.RestartStrip ();\n"
	"}\n",
	GLSLContext::UnitType_Geometry, combos
    );

    const auto& text = geometry.text ();

    CHECK_THAT (text, ContainsSubstring ("max_vertices = 7"));
    CHECK_THAT (text, ContainsSubstring ("EmitVertex();"));
    CHECK_THAT (text, ContainsSubstring ("EndPrimitive();"));
    CHECK_THAT (text, ContainsSubstring ("wpeLoop_s"));
}

TEST_CASE ("A geometry vertex count falls back to the shader's own define", "[shadercompat]") {
    Compiled geometry (
	"#define TRAILSUBDIVISION 4\n[maxvertexcount(TRAILSUBDIVISION * 2 - -1 / 0 + (1)]\nvoid main () { }\n",
	GLSLContext::UnitType_Geometry
    );

    CHECK_THAT (geometry.text (), ContainsSubstring ("max_vertices"));
}

TEST_CASE ("A released shader remembers whether it referred to a name", "[shadercompat]") {
    Compiled shader ("void main () { float MORPHING_X = 1.0; MORPHING; }\n");

    CHECK (shader.unit->refersTo ("MORPHING"));
    CHECK_FALSE (shader.unit->refersTo ("NOTHING"));

    shader.unit->releaseSources ();

    CHECK (shader.unit->refersTo ("MORPHING"));
    CHECK_FALSE (shader.unit->refersTo ("NOTHING"));
    std::string text;
    CHECK_THROWS (text = shader.text ());
}

TEST_CASE ("Mixed type min and max overloads are only added to shaders that call them", "[shadercompat]") {
    Compiled neither ("uniform float minvalue; void main () { float maxed = minvalue; }\n");
    const auto& plain = neither.text ();

    CHECK_THAT (plain, !ContainsSubstring ("wpe_max"));
    CHECK_THAT (plain, !ContainsSubstring ("wpe_min"));

    Compiled onlyMin ("void main () { vec3 a = min (vec3 (1.0), 0.5); }\n");
    const auto& minText = onlyMin.text ();

    CHECK_THAT (minText, ContainsSubstring ("#define min(x, y) wpe_min(x, y)"));
    CHECK_THAT (minText, !ContainsSubstring ("wpe_max"));

    Compiled both ("void main () { vec3 a = max (0.5, min (vec3 (1.0), 2.0)); }\n");
    const auto& bothText = both.text ();

    CHECK_THAT (bothText, ContainsSubstring ("#define max(x, y) wpe_max(y, x)"));
    CHECK_THAT (bothText, ContainsSubstring ("vec3 wpe_min(vec3 a, float b)"));
    CHECK_THAT (bothText, ContainsSubstring ("uint wpe_min(uint a, int b)"));
    CHECK_THAT (bothText, ContainsSubstring ("uvec3 wpe_max(ivec3 a, uvec3 b)"));
}

TEST_CASE ("A shader that defines its own min or max keeps it", "[shadercompat]") {
    Compiled macro ("#define min(a, b) ((a) < (b) ? (a) : (b))\nvoid main () { float a = min (1.0, 2.0); }\n");

    CHECK_THAT (macro.text (), !ContainsSubstring ("wpe_min"));

    Compiled overload ("vec3 max (vec3 a, float b) { return a; }\nvoid main () { vec3 a = max (vec3 (1.0), 2.0); }\n");

    CHECK_THAT (overload.text (), !ContainsSubstring ("wpe_max"));

    for (const auto* type : { "float2", "float3", "float4", "int2", "int3", "int4" }) {
	for (const auto* function : { "min", "max" }) {
	    const std::string t = type;
	    const std::string f = function;
	    Compiled hlslType (
		t + " " + f + " (" + t + " a, float b) { return a; }\nvoid main () { " + t + " a = " + f + " (" + t
		+ " (1.0), 2.0); }\n"
	    );

	    INFO (t << " " << f);
	    CHECK_THAT (hlslType.text (), !ContainsSubstring ("wpe_" + f));
	}
    }

    Compiled other ("#define min(a, b) a\nvoid main () { float a = max (1.0, 2.0); }\n");

    CHECK_THAT (other.text (), ContainsSubstring ("#define max(x, y) wpe_max(y, x)"));
    CHECK_THAT (other.text (), !ContainsSubstring ("wpe_min"));
}

TEST_CASE ("A min or max definition is told from the text around it", "[shadercompat]") {
    SECTION ("type names the shader makes itself") {
	Compiled alias (
	    "#define uint2 uvec2\n#define half uint2\nhalf max (half a, half b) { return a; }\n"
	    "void main () { uint2 a = max (uint2 (1), uint2 (2)); }\n"
	);

	CHECK_THAT (alias.text (), !ContainsSubstring ("wpe_max"));

	Compiled structure (
	    "struct Pair { float a; };\nPair min (Pair a, Pair b) { return a; }\n"
	    "void main () { Pair p = min (Pair (1.0), Pair (2.0)); }\n"
	);

	CHECK_THAT (structure.text (), !ContainsSubstring ("wpe_min"));

	Compiled array (
	    "float[2] max (float a) { return float[2] (a, a); }\nvoid main () { float[2] m = max (1.0); }\n"
	);

	CHECK_THAT (array.text (), !ContainsSubstring ("wpe_max"));
    }

    SECTION ("a macro of that name in any form") {
	Compiled objectLike (
	    "#define max myMax\nfloat myMax (float a, float b) { return a; }\nvoid main () { max (1.0, 2.0); }\n"
	);

	CHECK_THAT (objectLike.text (), !ContainsSubstring ("wpe_max"));
	CHECK_THAT (objectLike.text (), !ContainsSubstring ("#define max(x, y)"));

	Compiled spaced ("#  define min(a, b) a\nvoid main () { min (1.0, 2.0); }\n");

	CHECK_THAT (spaced.text (), !ContainsSubstring ("wpe_min"));
    }

    SECTION ("a call in the body of a macro is still a call") {
	Compiled macroBody ("#define CLAMPED max (vec2 (1.0), 0.5)\nvoid main () { vec2 a = CLAMPED; }\n");

	CHECK_THAT (macroBody.text (), ContainsSubstring ("#define max(x, y) wpe_max(y, x)"));

	Compiled returned ("vec2 f () { return max (vec2 (1.0), 0.5); }\nvoid main () { }\n");

	CHECK_THAT (returned.text (), ContainsSubstring ("wpe_max"));
    }

    SECTION ("comments and dead branches are no code") {
	Compiled lineComment ("// vec3 max (vec3 a, float b)\nvoid main () { vec2 a = max (vec2 (1.0), 0.0); }\n");

	CHECK_THAT (lineComment.text (), ContainsSubstring ("#define max(x, y) wpe_max(y, x)"));

	Compiled blockComment (
	    "/* #define min(a, b) a\n   float min (float a, float b) { return a; } */\n"
	    "void main () { vec2 a = min (vec2 (1.0), 0.0); }\n"
	);

	CHECK_THAT (blockComment.text (), ContainsSubstring ("#define min(x, y) wpe_min(x, y)"));

	Compiled commentedCall ("// max (a, b)\n/* min (a, b) */\nvoid main () { float a = 1.0; }\n");

	CHECK_THAT (commentedCall.text (), !ContainsSubstring ("wpe_m"));

	Compiled dead (
	    "#if 0\nfloat max (float a, float b) { return a; }\n#if 1\n#endif\n#endif\n"
	    "void main () { vec2 a = max (vec2 (1.0), 0.0); }\n"
	);

	CHECK_THAT (dead.text (), ContainsSubstring ("#define max(x, y) wpe_max(y, x)"));

	Compiled otherBranch (
	    "#if 0\n#else\nfloat max (float a, float b) { return a; }\n#endif\n"
	    "void main () { float a = max (1.0, 0.0); }\n"
	);

	CHECK_THAT (otherBranch.text (), !ContainsSubstring ("wpe_max"));
    }
}

TEST_CASE ("log10 is only added to shaders that have none", "[shadercompat]") {
    const std::string added = "#define log10(x) (log2(x) * 0.301029995663981)";

    Compiled none ("void main () { float a = log10 (100.0); }\n");

    CHECK_THAT (none.text (), ContainsSubstring (added));

    for (const auto* source :
	 { "float log10 (float x) { return x; }\nvoid main () { float a = log10 (100.0); }\n",
	   "float3 log10 (float3 x) { return x; }\nvoid main () { float3 a = log10 (float3 (1.0)); }\n",
	   "#define log10(x) (log (x) / log (10.0))\nvoid main () { float a = log10 (100.0); }\n",
	   "#define log10 mylog\nvoid main () { float a = log10 (100.0); }\n" }) {
	Compiled own (source);

	INFO (source);
	CHECK_THAT (own.text (), !ContainsSubstring (added));
    }

    Compiled commented ("// float log10 (float x)\nvoid main () { float a = log10 (100.0); }\n");

    CHECK_THAT (commented.text (), ContainsSubstring (added));
}

TEST_CASE ("A max reached through an alias macro still gets the argument swap", "[shadercompat]") {
    Compiled alias ("#define MAX max\nvoid main () { float a = MAX (0.5, 1.0); }\n");

    CHECK_THAT (alias.text (), ContainsSubstring ("#define max(x, y) wpe_max(y, x)"));
    CHECK_THAT (alias.text (), !ContainsSubstring ("wpe_min"));

    Compiled unrelated ("#define LIMIT 4\nvoid main () { float maximum = 1.0; }\n");

    CHECK_THAT (unrelated.text (), !ContainsSubstring ("wpe_max"));
}
