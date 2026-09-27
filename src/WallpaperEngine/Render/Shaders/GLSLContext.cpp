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
std::string describeFailure (
    const std::string& name, const std::string& stage, const std::string& source, const std::string& log
) {
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

std::pair<std::string, std::string> GLSLContext::toGlsl (
    const std::string& vertex, const std::string& fragment, const std::string& name
) {
    // pure function of the two sources, and passes get rebuilt often
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::pair<std::string, std::string>> cache;

    std::string cacheKey = vertex;
    cacheKey += '\0';
    cacheKey += fragment;

    {
	std::lock_guard lock (cacheMutex);

	if (const auto cached = cache.find (cacheKey); cached != cache.end ()) {
	    return cached->second;
	}
    }

    glslang::TShader vertexShader (EShLangVertex);

    const char* vertexSource = vertex.c_str ();
    vertexShader.setStrings (&vertexSource, 1);
    vertexShader.setEntryPoint ("main");
    vertexShader.setEnvInput (glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientOpenGL, 330);
    vertexShader.setEnvClient (glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    vertexShader.setEnvTarget (glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    vertexShader.setAutoMapLocations (true);
    vertexShader.setAutoMapBindings (true);

    if (!vertexShader.parse (&BuiltInResource, 100, false, EShMsgDefault)) {
	throw std::runtime_error (describeFailure (name, "vertex", vertex, vertexShader.getInfoLog ()));
    }
    glslang::TShader fragmentShader (EShLangFragment);

    const char* fragmentSource = fragment.c_str ();
    fragmentShader.setStrings (&fragmentSource, 1);
    fragmentShader.setEntryPoint ("main");
    fragmentShader.setEnvInput (glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientOpenGL, 330);
    fragmentShader.setEnvClient (glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    fragmentShader.setEnvTarget (glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    fragmentShader.setAutoMapLocations (true);
    fragmentShader.setAutoMapBindings (true);

    if (!fragmentShader.parse (&BuiltInResource, 100, false, EShMsgDefault)) {
	throw std::runtime_error (describeFailure (name, "fragment", fragment, fragmentShader.getInfoLog ()));
    }
    glslang::TProgram program;
    program.addShader (&vertexShader);
    program.addShader (&fragmentShader);

    if (!program.link (EShMsgDefault)) {
	throw std::runtime_error (
	    "GLSL program " + (name.empty () ? std::string ("<unnamed shader>") : name) + " failed to link: "
	    + program.getInfoLog ()
	);
    }

    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv (*program.getIntermediate (EShLangVertex), spirv);

    spirv_cross::CompilerGLSL vertexCompiler (spirv);
    spirv_cross::CompilerGLSL::Options options;
    options.version = 330;
    options.es = false;
    options.force_zero_initialized_variables = true;
    vertexCompiler.set_common_options (options);

    spirv.clear ();
    glslang::GlslangToSpv (*program.getIntermediate (EShLangFragment), spirv);

    spirv_cross::CompilerGLSL fragmentCompiler (spirv);
    options.version = 330;
    options.es = false;
    options.force_zero_initialized_variables = true;
    fragmentCompiler.set_common_options (options);

    std::pair<std::string, std::string> result = { vertexCompiler.compile () + "#if 0\n" + vertex + "\n#endif",
						   fragmentCompiler.compile () + "#if 0\n" + fragment + "\n#endif" };

    {
	std::lock_guard lock (cacheMutex);
	cache.emplace (std::move (cacheKey), result);
    }

    return result;
}

std::unique_ptr<GLSLContext> GLSLContext::sInstance = nullptr;