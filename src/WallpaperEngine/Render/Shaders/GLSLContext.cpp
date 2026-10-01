#include "GLSLContext.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "SPIRV/GlslangToSpv.h"
#include "glslang/Include/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv_glsl.hpp"

using namespace WallpaperEngine::Render::Shaders;

TBuiltInResource BuiltInResource = { .maxLights = 32,
				     .maxClipPlanes = 6,
				     .maxTextureUnits = 32,
				     .maxTextureCoords = 32,
				     .maxVertexAttribs = 64,
				     .maxVertexUniformComponents = 4096,
				     .maxVaryingFloats = 64,
				     .maxVertexTextureImageUnits = 32,
				     .maxCombinedTextureImageUnits = 80,
				     .maxTextureImageUnits = 32,
				     .maxFragmentUniformComponents = 4096,
				     .maxDrawBuffers = 32,
				     .maxVertexUniformVectors = 128,
				     .maxVaryingVectors = 8,
				     .maxFragmentUniformVectors = 16,
				     .maxVertexOutputVectors = 16,
				     .maxFragmentInputVectors = 15,
				     .minProgramTexelOffset = -8,
				     .maxProgramTexelOffset = 7,
				     .maxClipDistances = 8,
				     .maxComputeWorkGroupCountX = 65535,
				     .maxComputeWorkGroupCountY = 65535,
				     .maxComputeWorkGroupCountZ = 65535,
				     .maxComputeWorkGroupSizeX = 1024,
				     .maxComputeWorkGroupSizeY = 1024,
				     .maxComputeWorkGroupSizeZ = 64,
				     .maxComputeUniformComponents = 1024,
				     .maxComputeTextureImageUnits = 16,
				     .maxComputeImageUniforms = 8,
				     .maxComputeAtomicCounters = 8,
				     .maxComputeAtomicCounterBuffers = 1,
				     .maxVaryingComponents = 60,
				     .maxVertexOutputComponents = 64,
				     .maxGeometryInputComponents = 64,
				     .maxGeometryOutputComponents = 128,
				     .maxFragmentInputComponents = 128,
				     .maxImageUnits = 8,
				     .maxCombinedImageUnitsAndFragmentOutputs = 8,
				     .maxCombinedShaderOutputResources = 8,
				     .maxImageSamples = 0,
				     .maxVertexImageUniforms = 0,
				     .maxTessControlImageUniforms = 0,
				     .maxTessEvaluationImageUniforms = 0,
				     .maxGeometryImageUniforms = 0,
				     .maxFragmentImageUniforms = 8,
				     .maxCombinedImageUniforms = 8,
				     .maxGeometryTextureImageUnits = 16,
				     .maxGeometryOutputVertices = 256,
				     .maxGeometryTotalOutputComponents = 1024,
				     .maxGeometryUniformComponents = 1024,
				     .maxGeometryVaryingComponents = 64,
				     .maxTessControlInputComponents = 128,
				     .maxTessControlOutputComponents = 128,
				     .maxTessControlTextureImageUnits = 16,
				     .maxTessControlUniformComponents = 1024,
				     .maxTessControlTotalOutputComponents = 4096,
				     .maxTessEvaluationInputComponents = 128,
				     .maxTessEvaluationOutputComponents = 128,
				     .maxTessEvaluationTextureImageUnits = 16,
				     .maxTessEvaluationUniformComponents = 1024,
				     .maxTessPatchComponents = 120,
				     .maxPatchVertices = 32,
				     .maxTessGenLevel = 64,
				     .maxViewports = 16,
				     .maxVertexAtomicCounters = 0,
				     .maxTessControlAtomicCounters = 0,
				     .maxTessEvaluationAtomicCounters = 0,
				     .maxGeometryAtomicCounters = 0,
				     .maxFragmentAtomicCounters = 8,
				     .maxCombinedAtomicCounters = 8,
				     .maxAtomicCounterBindings = 1,
				     .maxVertexAtomicCounterBuffers = 0,
				     .maxTessControlAtomicCounterBuffers = 0,
				     .maxTessEvaluationAtomicCounterBuffers = 0,
				     .maxGeometryAtomicCounterBuffers = 0,
				     .maxFragmentAtomicCounterBuffers = 1,
				     .maxCombinedAtomicCounterBuffers = 1,
				     .maxAtomicCounterBufferSize = 16384,
				     .maxTransformFeedbackBuffers = 4,
				     .maxTransformFeedbackInterleavedComponents = 64,
				     .maxCullDistances = 8,
				     .maxCombinedClipAndCullDistances = 8,
				     .maxSamples = 4,
				     .maxMeshOutputVerticesNV = 256,
				     .maxMeshOutputPrimitivesNV = 512,
				     .maxMeshWorkGroupSizeX_NV = 32,
				     .maxMeshWorkGroupSizeY_NV = 1,
				     .maxMeshWorkGroupSizeZ_NV = 1,
				     .maxTaskWorkGroupSizeX_NV = 32,
				     .maxTaskWorkGroupSizeY_NV = 1,
				     .maxTaskWorkGroupSizeZ_NV = 1,
				     .maxMeshViewCountNV = 4,
				     .limits = {
					 .nonInductiveForLoops = true,
					 .whileLoops = true,
					 .doWhileLoops = true,
					 .generalUniformIndexing = true,
					 .generalAttributeMatrixVectorIndexing = true,
					 .generalVaryingIndexing = true,
					 .generalSamplerIndexing = true,
					 .generalVariableIndexing = true,
					 .generalConstantMatrixVectorIndexing = true,
				     } };

GLSLContext::GLSLContext () {
    assert (this->sInstance == nullptr);

    glslang::InitializeProcess ();
}

GLSLContext::~GLSLContext () { glslang::FinalizeProcess (); }

GLSLContext& GLSLContext::get () {
    if (sInstance == nullptr) {
	sInstance = std::make_unique<GLSLContext> ();
    }

    return *sInstance;
}

namespace {
// glslang's log says "ERROR: 0:367: ..." against the exact string it was given, show those lines with some context
std::string
describeFailure (const std::string& name, const std::string& stage, const std::string& source, const std::string& log) {
    static const std::regex errorLine (R"((?:ERROR|WARNING): \d+:(\d+):)");
    constexpr int context = 4;

    std::vector<std::string> lines;
    std::istringstream input (source);
    for (std::string line; std::getline (input, line);) {
	lines.push_back (line);
    }

    std::set<int> errors;
    for (auto it = std::sregex_iterator (log.cbegin (), log.cend (), errorLine); it != std::sregex_iterator (); ++it) {
	errors.insert (std::stoi ((*it)[1].str ()));
    }

    std::ostringstream out;
    out << "GLSL " << stage << " unit of " << (name.empty () ? "<unnamed shader>" : name) << " failed to parse:\n"
	<< log;

    int shownUpTo = 0;
    for (const int error : errors) {
	const int from = std::max ({ 1, error - context, shownUpTo + 1 });
	const int to = std::min (static_cast<int> (lines.size ()), error + context);

	if (from > to) {
	    continue;
	}
	if (from > shownUpTo + 1) {
	    out << "  ...\n";
	}
	for (int number = from; number <= to; number++) {
	    out << (number == error ? ">" : " ") << std::setw (5) << number << ": " << lines[number - 1] << '\n';
	}
	shownUpTo = to;
    }

    // the whole thing is ~500 lines with includes, too much for the log every time
    if (const char* dir = std::getenv ("LWE_SHADER_DUMP_DIR"); dir != nullptr && *dir != '\0') {
	std::string file = name.empty () ? "shader" : name;
	std::replace (file.begin (), file.end (), '/', '_');
	const auto path = std::filesystem::path (dir) / (file + "." + stage + ".glsl");

	std::error_code ec;
	std::filesystem::create_directories (dir, ec);
	std::ofstream (path) << source;
	out << "full source written to " << path.string () << '\n';
    }

    return out.str ();
}
} // namespace

namespace {
/** Parses one stage, throwing the report describeFailure builds when glslang rejects it */
std::unique_ptr<glslang::TShader>
parseStage (EShLanguage stage, const std::string& source, const std::string& name, const char* stageName) {
    auto shader = std::make_unique<glslang::TShader> (stage);

    const char* text = source.c_str ();
    shader->setStrings (&text, 1);
    shader->setEntryPoint ("main");
    shader->setEnvInput (glslang::EShSourceGlsl, stage, glslang::EShClientOpenGL, 330);
    shader->setEnvClient (glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    shader->setEnvTarget (glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    shader->setAutoMapLocations (true);
    shader->setAutoMapBindings (true);

    if (!shader->parse (&BuiltInResource, 100, false, EShMsgDefault)) {
	throw std::runtime_error (describeFailure (name, stageName, source, shader->getInfoLog ()));
    }

    return shader;
}

std::string crossCompile (glslang::TProgram& program, EShLanguage stage) {
    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv (*program.getIntermediate (stage), spirv);

    spirv_cross::CompilerGLSL compiler (spirv);
    spirv_cross::CompilerGLSL::Options options;
    options.version = 330;
    options.es = false;
    options.force_zero_initialized_variables = true;
    compiler.set_common_options (options);

    return compiler.compile ();
}
} // namespace

GLSLContext::Sources GLSLContext::toGlsl (
    const std::string& vertex, const std::string& fragment, const std::string& name, const std::string& geometry
) {
    // pure function of the sources, and passes get rebuilt often
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, Sources> cache;

    std::string cacheKey = vertex;
    cacheKey += '\0';
    cacheKey += fragment;
    cacheKey += '\0';
    cacheKey += geometry;

    {
	std::lock_guard lock (cacheMutex);

	if (const auto cached = cache.find (cacheKey); cached != cache.end ()) {
	    return cached->second;
	}
    }

    const auto vertexShader = parseStage (EShLangVertex, vertex, name, "vertex");
    const auto fragmentShader = parseStage (EShLangFragment, fragment, name, "fragment");
    std::unique_ptr<glslang::TShader> geometryShader;
    if (!geometry.empty ()) {
	geometryShader = parseStage (EShLangGeometry, geometry, name, "geometry");
    }

    glslang::TProgram program;
    program.addShader (vertexShader.get ());
    if (geometryShader) {
	program.addShader (geometryShader.get ());
    }
    program.addShader (fragmentShader.get ());

    if (!program.link (EShMsgDefault)) {
	throw std::runtime_error (
	    "GLSL program " + (name.empty () ? std::string ("<unnamed shader>") : name)
	    + " failed to link: " + program.getInfoLog ()
	);
    }

    Sources result = {
	.vertex = crossCompile (program, EShLangVertex) + "#if 0\n" + vertex + "\n#endif",
	.fragment = crossCompile (program, EShLangFragment) + "#if 0\n" + fragment + "\n#endif",
	.geometry = geometryShader ? crossCompile (program, EShLangGeometry) + "#if 0\n" + geometry + "\n#endif" : "",
    };

    {
	std::lock_guard lock (cacheMutex);
	cache.emplace (std::move (cacheKey), result);
    }

    return result;
}

std::unique_ptr<GLSLContext> GLSLContext::sInstance = nullptr;