#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Render/Objects/CCamera.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CLight.h"
#include "WallpaperEngine/Render/Objects/CMesh.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CText.h"

#include "WallpaperEngine/Render/WallpaperState.h"

#include "CScene.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Render/Shaders/GLSLContext.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <regex>

extern float g_Time;
extern float g_TimeLast;

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Render::Wallpapers;

CScene::CScene (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    // caller should check this, if not a std::bad_cast is good to throw
    auto scene = wallpaper.as<Scene> ();
    this->m_startTime = g_Time;

    this->m_scriptEngine = std::make_unique<Scripting::ScriptEngine> (*this, context.getMediaSource ());
    this->m_camera = std::make_unique<Camera> (*this, scene->camera);
    // shaders get FOG_DIST/FOG_HEIGHT from the renderer flags set when the scene loads (sub_140186440, sub_1401A5C40)
    this->m_fogDistance = scene->fog.distanceEnabled->value->getBool ();
    this->m_fogHeight = scene->fog.heightEnabled->value->getBool ();

    float width = scene->camera.projection.width;
    float height = scene->camera.projection.height;

    // "auto" always matches the output resolution, same as real Wallpaper Engine
    if (scene->camera.projection.isAuto) {
	width = this->getContext ().getOutput ().getFullWidth ();
	height = this->getContext ().getOutput ().getFullHeight ();
    }

    this->m_cameraParallax = { 0, 0 };
    this->m_parallaxBias = { 0, 0 };
    this->m_parallaxPosition = { 0.5f, 0.5f };

    const bool perspective = scene->camera.projection.isPerspective;

    if (perspective) {
	// 3D scenes render at the output's size, the perspective camera stretches to whatever aspect that has
	this->m_camera->setPerspectiveProjection (width, height);
    } else {
	float canvasWidth = width;
	float canvasHeight = height;

	if (this->getContext ().getApp ().getContext ().settings.general.expandCanvas) {
	    this->expandCanvasToContent (*scene, width, height, canvasWidth, canvasHeight);
	}

	this->m_camera->setOrthogonalProjection (width, height, canvasWidth, canvasHeight);
    }

    // HDR rendering needs WE's "ultra" post processing plus bloom and hdr in the scene (sub_14010DF40), then the
    // scene and layer buffers are 16 bit float and bloom is the HDR mip chain (sub_140183610)
    this->m_hdr = this->getContext ().getApp ().getContext ().settings.general.ultraPostProcessing
	&& scene->camera.bloom.enabled->value->getBool () && scene->camera.bloom.hdr->value->getBool ();
    // postprocessing "displayhdr" sets 0x6000 instead of 0x2000 (sub_14010DF40): the same HDR scene with the
    // combine_dhdr_upsample combine
    this->m_displayHDR = this->m_hdr && this->getContext ().getApp ().getContext ().settings.general.displayHDR;
    this->m_volumetrics = std::make_unique<Volumetrics> (*this);

    // lightconfig as the scene constructor packs it (sub_140186C90): with WE's shadow setting off spotshadowcookie
    // folds into spotcookie and the other shadow counts are dropped. Passes read these when they're built
    const auto& lightConfig = this->getScene ().lightConfig;
    this->m_shadowQuality = this->getContext ().getApp ().getContext ().settings.general.shadowQuality;
    this->m_lightingV1.points = lightConfig.point;
    this->m_lightingV1.spots = lightConfig.spot;
    this->m_lightingV1.tubes = lightConfig.tube;
    this->m_lightingV1.directionals = lightConfig.directional;

    if (this->m_shadowQuality > 0) {
	this->m_lightingV1.spotCookies = lightConfig.spotCookie;
	this->m_lightingV1.spotShadowCookies = lightConfig.spotShadowCookie;
	this->m_lightingV1.spotShadows = lightConfig.spotShadow;
	this->m_lightingV1.directionalShadows = lightConfig.directionalShadow;
	this->m_lightingV1.pointShadows = lightConfig.pointShadow;
    } else {
	this->m_lightingV1.spotCookies = lightConfig.spotCookie | lightConfig.spotShadowCookie;
    }

    // models depth test against each other, 2D scenes can hold some too
    const bool hasModels
	= std::ranges::any_of (scene->objects, [] (const auto& object) { return object->template is<Mesh> (); });

    // needed before scene setup below, which creates FBOs
    this->setupFramebuffers (
	perspective || hasModels, this->m_hdr ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888
    );

    this->followOutputSize (this->find ("_rt_FullFrameBuffer"), 1);

    // wallpaper64.exe 2.8.42 sub_14010DF40 flags a scene when an object has a "model", only then the device gets the
    // msaa setting's sample count (sub_140110630, sub_14012AC60) and sub_140181AF0 makes the multisampled target
    if (hasModels) {
	this->m_msaaSamples = this->getContext ().getApp ().getContext ().settings.general.msaaSamples;
    }

    const uint32_t sceneWidth = this->m_camera->getCanvasWidth ();
    const uint32_t sceneHeight = this->m_camera->getCanvasHeight ();

    // WE creates it on the first frame with shadows at the size the tiles need (sub_140190C80), it only grows
    this->_rt_shadowAtlas
	= this->create ("_rt_shadowAtlas", TextureFormat_D32f, TextureFlags_ClampUVsBorder, 1.0, { 2, 2 }, { 2, 2 });
    this->alias ("_alias_lightCookie", "_rt_shadowAtlas");
    this->m_shadowMapping = std::make_unique<ShadowMapping> (*this, this->_rt_shadowAtlas);

    const glm::vec3 clearColor = scene->colors.clear->value->getVec3 ();

    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);

    // createObject recurses into each object's dependencies/parent first
    for (const auto& object : scene->objects) {
	this->createObject (*object);
    }

    // real Wallpaper Engine draws its object list in scene.json array order unless the scene sets
    // customsortorder (then by each object's sortorder); parent/attachment links don't reorder anything
    std::vector<std::pair<const Object*, int>> objectsByPaintOrder;
    objectsByPaintOrder.reserve (scene->objects.size ());
    for (int index = 0; index < static_cast<int> (scene->objects.size ()); index++) {
	const Object* object = scene->objects[index].get ();
	objectsByPaintOrder.emplace_back (object, object->sortOrder.value_or (index));
    }
    std::ranges::stable_sort (objectsByPaintOrder, [] (const auto& a, const auto& b) { return a.second < b.second; });

    for (const auto& [object, sortKey] : objectsByPaintOrder) {
	this->addObjectToRenderOrder (*object);
    }

    // for the bloom effect below. WE sizes them from the output like _rt_FullFrameBuffer (sub_14017F1B0)
    const auto quarter = this->create (
	"_rt_4FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 4, sceneHeight / 4 },
	{ sceneWidth / 4, sceneHeight / 4 }
    );
    const auto eighth = this->create (
	"_rt_8FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );
    const auto bloomBuffer = this->create (
	"_rt_Bloom", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );
    this->followOutputSize (quarter, 4);
    this->followOutputSize (eighth, 8);
    this->followOutputSize (bloomBuffer, 8);
    this->_rt_4FrameBuffer = quarter;
    this->_rt_8FrameBuffer = eighth;
    this->_rt_Bloom = bloomBuffer;

    // Bloom is achieved without any custom code by synthesizing a fake image object that loads
    // effect files from the virtual container - this costs two extra draw calls versus official WPE,
    // which renders bloom directly to the screen, something a scene here never does.
    const auto bloomOrigin = glm::vec3 { sceneWidth / 2, sceneHeight / 2, 0.0f };
    // WE sets bloomstrength, bloomthreshold and bloomtint on the bloom material (sub_14017F1B0)
    const glm::vec3 bloomTint = this->getScene ().camera.bloom.tint->value->getVec3 ();
    const std::string bloomTintValue
	= std::to_string (bloomTint.x) + " " + std::to_string (bloomTint.y) + " " + std::to_string (bloomTint.z);
    const auto bloomSize = glm::vec2 { sceneWidth, sceneHeight };

    const JSON bloom
	= { { "image", "models/wpenginelinux.json" },
	    { "name", "bloomimagewpenginelinux" },
	    { "visible", true },
	    { "scale", "1.0 1.0 1.0" },
	    { "angles", "0.0 0.0 0.0" },
	    { "origin",
	      std::to_string (bloomOrigin.x) + " " + std::to_string (bloomOrigin.y) + " "
		  + std::to_string (bloomOrigin.z) },
	    { "size", std::to_string (bloomSize.x) + " " + std::to_string (bloomSize.y) },
	    { "id", -1 },
	    { "effects",
	      JSON::array (
		  { { { "file", "effects/wpenginelinux/bloomeffect.json" },
		      { "id", 15242000 },
		      { "name", "" },
		      { "passes",
			JSON::array (
			    { { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold", this->getScene ().camera.bloom.threshold->value->getFloat () },
				    { "bloomtint", bloomTintValue } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold", this->getScene ().camera.bloom.threshold->value->getFloat () },
				    { "bloomtint", bloomTintValue } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold", this->getScene ().camera.bloom.threshold->value->getFloat () },
				    { "bloomtint", bloomTintValue } } } } }
			) } } }
	      ) } };

    if (scene->camera.bloom.enabled->value->getBool () && !this->m_hdr) {
	this->m_bloomObjectData = ObjectParser::parse (bloom, scene->project);
	this->m_bloomObject = this->createObject (*this->m_bloomObjectData);

	this->m_objectsByRenderOrder.push_back (this->m_bloomObject);
    }
}

CScene::~CScene () {
    // bloom object is in the objects list, so no need to explicitly delete it
    this->m_bloomObject = nullptr;

    for (const auto& val : this->m_objects | std::views::values) {
	delete val;
    }

    this->m_objectsByRenderOrder.clear ();
    this->m_objects.clear ();

    if (this->m_fadeProgram != GL_NONE) {
	glDeleteProgram (this->m_fadeProgram);
    }

    this->releaseHDRBloom ();
    this->releaseMultisample ();

    for (const GLuint program : { this->m_bloomDownsample, this->m_bloomDownsampleThreshold, this->m_bloomUpsample,
				  this->m_bloomUpsampleCubic, this->m_bloomCombine }) {
	if (program != GL_NONE) {
	    glDeleteProgram (program);
	}
    }
}

Render::CObject* CScene::createObject (const Object& object) {
    Render::CObject* renderObject = nullptr;

    if (const auto current = this->m_objects.find (object.id); current != this->m_objects.end ()) {
	return current->second;
    }

    // dependency/parent loops spanning several objects exist in the wild, the object further
    // up the stack finishes creating itself once this call unwinds
    if (!this->m_objectsInCreation.insert (object.id).second) {
	sLog.error ("Dependency or parent cycle through object ", object.id, ", ignoring the back reference");
	return nullptr;
    }

    try {
	this->createObjectDependencies (object);
    } catch (...) {
	this->m_objectsInCreation.erase (object.id);
	throw;
    }

    this->m_objectsInCreation.erase (object.id);

    renderObject = this->dispatchObjectType (object);

    if (renderObject != nullptr) {
	this->m_objects.emplace (renderObject->getId (), renderObject);
    }

    return renderObject;
}

void CScene::createObjectDependencies (const Object& object) {
    for (const auto& cur : object.dependencies) {
	// self-dependency is a possibility...
	if (cur == object.id) {
	    continue;
	}

	const auto dep
	    = std::ranges::find_if (this->getScene ().objects, [&cur] (const auto& o) { return o->id == cur; });

	if (dep != this->getScene ().objects.end ()) {
	    this->createObject (**dep);
	}
    }

    if (object.parent.has_value ()) {
	int parentId = object.parent.value ();

	const auto dep = std::ranges::find_if (this->getScene ().objects, [&parentId] (const auto& o) {
	    return o->id == parentId;
	});

	if (dep == this->getScene ().objects.end ()) {
	    sLog.exception ("Cannot find parent ", parentId, " for object ", object.id);
	}

	this->createObject (**dep);
    }
}

Render::CObject* CScene::dispatchObjectType (const Object& object) {
    Render::CObject* renderObject = nullptr;
    if (object.is<Image> ()) {
	renderObject = new Objects::CImage (*this, *object.as<Image> ());
    } else if (object.is<Sound> ()) {
	renderObject = new Objects::CSound (*this, *object.as<Sound> ());
    } else if (object.is<Text> ()) {
	renderObject = new Objects::CText (*this, *object.as<Text> ());
    } else if (object.is<Light> ()) {
	renderObject = new Objects::CLight (*this, *object.as<Light> (), this->nextFreeLightSlot ());
    } else if (object.is<SceneCamera> ()) {
	auto* camera = new Objects::CCamera (*this, *object.as<SceneCamera> ());
	this->m_sceneCameras.push_back (camera);
	renderObject = camera;
    } else if (object.is<Mesh> ()) {
	renderObject = new Objects::CMesh (*this, *object.as<Mesh> ());
    } else if (object.is<Particle> ()) {
	const auto& particleData = *object.as<Particle> ();

	if (this->getContext ().getApp ().getContext ().settings.general.disableParticles == true) {
	    sLog.debug ("Ignoring particle system (disabled in settings): ", particleData.name);
	    return nullptr;
	}

	if (!particleData.material || !particleData.material->material) {
	    sLog.error ("Ignoring particle system with no usable material: ", particleData.name);
	    return nullptr;
	}

	renderObject = new Objects::CParticle (*this, particleData);
    } else {
	// group/locator nodes can still carry scripts, and the shared-state bootstrap script often lives on one
	sLog.debug ("Creating group object without renderable: ", object.id);
	renderObject = new Scripting::ScriptableObject (*this, object);
    }

    try {
	renderObject->setup ();
    } catch (const std::exception& e) {
	sLog.error ("Failed to setup object ", object.id, ": ", e.what ());
	if (auto* scriptable = dynamic_cast<Scripting::ScriptableObject*> (renderObject); scriptable != nullptr) {
	    this->getScriptEngine ().dropObjectScripts (*scriptable);
	}
	delete renderObject;
	renderObject = nullptr;
    }

    return renderObject;
}

void CScene::addObjectToRenderOrder (const Object& object) {
    const auto obj = this->m_objects.find (object.id);

    // ignores not created objects like particle systems
    if (obj == this->m_objects.end ()) {
	return;
    }

    if (!this->m_objectsInRenderOrderWalk.insert (object.id).second) {
	return;
    }

    for (const auto& dep : object.dependencies) {
	// self-dependency is possible
	if (dep == object.id) {
	    continue;
	}

	auto depIt = std::ranges::find_if (this->getScene ().objects, [&dep] (const auto& o) { return o->id == dep; });

	if (depIt != this->getScene ().objects.end ()) {
	    this->addObjectToRenderOrder (**depIt);

	    if (const auto created = this->m_objects.find (dep);
		created != this->m_objects.end () && created->second->is<Objects::CImage> ()) {
		created->second->as<Objects::CImage> ()->markAsDependency ();
	    }
	} else {
	    sLog.error ("Cannot find dependency ", dep, " for object ", object.id);
	}
    }

    // avoid adding the same object twice if several others depend on it
    const auto renderIt = std::ranges::find_if (this->m_objectsByRenderOrder, [&object] (const auto& o) {
	return o->getId () == object.id;
    });

    if (renderIt == this->m_objectsByRenderOrder.end ()) {
	this->m_objectsByRenderOrder.emplace_back (obj->second);
    }

    this->m_objectsInRenderOrderWalk.erase (object.id);
}

ScriptEngine& CScene::getScriptEngine () const { return *this->m_scriptEngine; }
Camera& CScene::getCamera () const { return *this->m_camera; }

namespace {
// with LWE_FRAME_STATS set, any single step of a scene frame slower than this is logged by name
constexpr double STALL_THRESHOLD_MS = 15.0;

template <typename Step> void timeStep (const std::string& label, Step&& step) {
    static const bool enabled = std::getenv ("LWE_FRAME_STATS") != nullptr;

    if (!enabled) {
	step ();
	return;
    }

    glFinish ();
    const auto start = std::chrono::steady_clock::now ();
    step ();
    glFinish ();
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();

    if (ms >= STALL_THRESHOLD_MS) {
	sLog.out ("FRAME-STALL ", label, ": ", ms, "ms");
    }
}
}

void CScene::renderFrame (const glm::ivec4& viewport) {
    timeStep ("renderFrame total", [&] { this->renderFrameSteps (viewport); });
}

void CScene::renderFrameSteps (const glm::ivec4& viewport) {
    this->m_outputSize = { viewport.z, viewport.w };
    this->resizeOutputBuffers (this->m_outputSize);
    // wallpaper64.exe sub_14017FA70 counts every frame (scene render object +340, zeroed by sub_14017C6D0) before
    // anything is simulated, boids pick their sample block with it
    this->m_frameCounter++;
    timeStep ("updateMouse", [&] { this->updateMouse (viewport); });

    // WE updates every object before cursor events and scripts (sub_1401891A0 from sub_14017FA70), so scripts read
    // and change this frame's puppet bones and the render draws what they left
    for (const auto& cur : this->m_objectsByRenderOrder) {
	if (cur->is<Objects::CImage> ()) {
	    cur->as<Objects::CImage> ()->updatePuppetPose ();
	    cur->as<Objects::CImage> ()->updateTextureAnimation (this->getDeltaTime ());
	} else if (cur->is<Objects::CMesh> ()) {
	    cur->as<Objects::CMesh> ()->updateAnimation ();
	} else if (cur->is<Objects::CSound> ()) {
	    // every object, hidden ones too, and sounds nothing draws
	    cur->as<Objects::CSound> ()->update (this->getDeltaTime ());
	}
    }

    // after the tick, so a layer a script moves this frame (e.g. onto input.cursorWorldPosition) is hit tested where it
    // is now
    timeStep ("script tick", [&] { this->getScriptEngine ().tick (); });
    // the first tick runs the objects' init (), which WE does before its loader sets the static camera
    this->loadStaticCamera ();
    // WE runs the object updates first, then the camera, then the parallax camera
    this->updateCamera ();
    this->updateParallax ();
    timeStep ("cursor events", [&] { this->dispatchCursorEvents (); });
    this->updateLights ();
    this->updateLightingV1 ();
    timeStep ("shadows", [&] { this->m_shadowMapping->render (this->m_shadowEntries); });

    // only image objects need their texture (e.g. video/gif frame) refreshed before drawing
    for (const auto& cur : this->m_objectsByRenderOrder) {
	if (!cur->is<Objects::CImage> ()) {
	    continue;
	}

	const Objects::CImage* image = cur->as<Objects::CImage> ();

#if !NDEBUG
	const std::string message = "Updating texture " + image->getImage ().model->filename;

	glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, message.c_str ());
#endif

	timeStep ("updateTextures " + image->getObject ().name, [&] { image->updateTextures (); });

#if !NDEBUG
	glPopDebugGroup ();
#endif
    }

    glBindVertexArray (this->m_vaoBuffer);
    this->renderReflection ();
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    glViewport (0, 0, this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight ());

    // sub_140183550: the objects draw into _rt_FullFrameBufferMultiSampled, which resolves into the scene buffer
    // once they are done
    this->m_msaaActive = this->m_msaaSamples > 1 && this->prepareMultisample ();

    if (this->m_msaaActive) {
	glBindFramebuffer (GL_FRAMEBUFFER, this->m_msaaFramebuffer);
    }

    // passes leave their own depthwrite and color mask behind, and glClear skips masked channels. Layer
    // composites write rgb only, so the alpha would otherwise never get reset to 1 and fullscreen layers,
    // which copy the scene and blend back with its alpha, would come out invisible
    glDepthMask (GL_TRUE);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const glm::vec3 clearColor = this->getScene ().colors.clear->value->getVec3 ();
    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    this->paintLetterbox ();

    // transparentsorting only does something in 3D scenes (render flags & 0x1008 == 0x1000, sub_14018AAC0): the rest
    // goes first in list order, then the transparent objects back to front
    if (this->getCamera ().isPerspective () && this->getScene ().transparentSorting->value->getBool ()) {
	std::vector<CObject*> transparent;

	for (auto* cur : std::vector (this->m_objectsByRenderOrder)) {
	    if (this->isTransparentSorted (*cur)) {
		transparent.push_back (cur);
	    } else {
		this->renderSceneObject (cur);
	    }
	}

	for (auto* cur : this->sortedByDepth (std::move (transparent))) {
	    this->renderSceneObject (cur);
	}
    } else {
	for (const auto& cur : this->m_objectsByRenderOrder) {
	    this->renderSceneObject (cur);
	}
    }

    this->m_volumetrics->composite ();

    if (this->m_msaaActive) {
	this->resolveMultisample ();
	this->m_msaaActive = false;
	glBindFramebuffer (GL_FRAMEBUFFER, this->m_sceneFBO->getFramebuffer ());
    }

    if (this->m_bloomObject == nullptr) {
	this->updateMipMappedFrameBuffer ();
    }

    if (this->m_hdr) {
	this->renderHDRBloom ();
    }

    if (this->m_cameraFade > 0.0f) {
	this->renderCameraFade ();
    }
}

void CScene::paintLetterbox () const {
    // the bars a fit/center/free alignment leaves are part of WE's scene buffer (sub_140183A70 widens the projection
    // over them) and only get the clear color, whatever is drawn there shows (text screen anchors move into them).
    // --corner-color (with the border clamp) takes the clear color's place there, right after the clear
    if (this->getState ().getClampingMode () != TextureFlags_ClampUVsBorder || this->m_camera->isPerspective ()) {
	return;
    }

    glm::vec4 margins = this->getVisibleMargins ();

    if (margins.x >= 0.0f && margins.y >= 0.0f && margins.z >= 0.0f && margins.w >= 0.0f) {
	return;
    }

    // alignmentfliph mirrors the projection, the left bar ends up on the right
    if (this->isFlippedHorizontally ()) {
	std::swap (margins.x, margins.y);
    }

    const float width = static_cast<float> (this->getCanvasWidth ());
    const float height = static_cast<float> (this->getCanvasHeight ());
    const float visibleWidth = width - margins.x - margins.y;
    const float visibleHeight = height - margins.z - margins.w;
    const auto bufferWidth = static_cast<float> (this->m_sceneFBO->getRealWidth ());
    const auto bufferHeight = static_cast<float> (this->m_sceneFBO->getRealHeight ());
    // scene units to buffer pixels, the buffer's rows run from the bottom of the scene up
    const auto columnOf = [&] (const float x) {
	return static_cast<GLint> (std::lround ((x - margins.x) / visibleWidth * bufferWidth));
    };
    const auto rowOf = [&] (const float y) {
	return static_cast<GLint> (std::lround ((y - margins.z) / visibleHeight * bufferHeight));
    };
    const GLint left = std::clamp (columnOf (0.0f), 0, static_cast<GLint> (bufferWidth));
    const GLint right = std::clamp (columnOf (width), 0, static_cast<GLint> (bufferWidth));
    const GLint bottom = std::clamp (rowOf (0.0f), 0, static_cast<GLint> (bufferHeight));
    const GLint top = std::clamp (rowOf (height), 0, static_cast<GLint> (bufferHeight));
    const glm::vec4& color = this->getCornerColor ();

    glBindFramebuffer (GL_FRAMEBUFFER, this->getSceneDrawFramebuffer ());
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor (color.r, color.g, color.b, color.a);
    glEnable (GL_SCISSOR_TEST);

    const auto fill = [] (const GLint x, const GLint y, const GLint w, const GLint h) {
	if (w > 0 && h > 0) {
	    glScissor (x, y, w, h);
	    glClear (GL_COLOR_BUFFER_BIT);
	}
    };

    fill (0, 0, left, static_cast<GLint> (bufferHeight));
    fill (right, 0, static_cast<GLint> (bufferWidth) - right, static_cast<GLint> (bufferHeight));
    fill (left, 0, right - left, bottom);
    fill (left, top, right - left, static_cast<GLint> (bufferHeight) - top);

    glDisable (GL_SCISSOR_TEST);
}

bool CScene::isShadowCasterVisible (const CObject& object) const {
    // the object's visible check (vtable +104) and flag 2 (under a passthrough layer), like renderSceneObject
    const auto visibility = this->getContext ().getApp ().getContext ().resolveObjectVisibility (
	object.getId (), object.getObject ().name
    );

    if (visibility.has_value () && !visibility.value ()) {
	return false;
    }

    if (this->isHiddenByAncestor (object) || this->isDrawnByPassthroughLayer (object)) {
	return false;
    }

    if (!object.is<Objects::CMesh> ()) {
	return true;
    }

    // casters have object flags 0xC00: "castshadow" (0x800) and a normal or alphatocoverage material (0x400)
    const auto& mesh = object.as<Objects::CMesh> ()->getMesh ();
    return mesh.groupVisible->value->getBool () && mesh.castShadow->value->getBool ();
}

void CScene::renderSceneObject (CObject* cur) {
    const auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
    if (debug.objectFilter.has_value () && cur->getId () != debug.objectFilter.value ()) {
	return;
    }
    if (std::ranges::find (debug.skipObjects, cur->getId ()) != debug.skipObjects.end ()) {
	return;
    }

    const auto visibility
	= this->getContext ().getApp ().getContext ().resolveObjectVisibility (cur->getId (), cur->getObject ().name);
    if (visibility.has_value () && !visibility.value ()) {
	return;
    }

    if (this->isHiddenByAncestor (*cur)) {
	return;
    }

    // a passthrough layer above it draws it into its own buffer (object flag 2, skipped by sub_14018AAC0)
    if (this->isDrawnByPassthroughLayer (*cur)) {
	return;
    }

    // lights draw their volumes into the light buffer, which goes onto the scene before the next other object
    if (cur->is<Objects::CLight> ()) {
	this->renderLightVolume (*cur->as<Objects::CLight> (), this->getWorldViewProjection ());
	return;
    }

    this->m_volumetrics->composite ();

    if (cur == this->m_bloomObject && !this->m_renderingReflection) {
	this->updateMipMappedFrameBuffer ();
    }

    timeStep ("render " + cur->getObject ().name, [&] { cur->render (); });
}

namespace {
GLuint buildProgram (const std::string& defines, const char* fragmentBody) {
    static const char* vertex = "#version 330\n"
				"out vec2 v_TexCoord;\n"
				"void main () {\n"
				"vec2 corner = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
				"v_TexCoord = corner;\n"
				"gl_Position = vec4 (corner * 2.0 - 1.0, 0.0, 1.0);\n"
				"}";
    const std::string fragment = "#version 330\n" + defines + fragmentBody;
    const char* fragmentSource = fragment.c_str ();
    const GLuint vertexShader = glCreateShader (GL_VERTEX_SHADER);
    const GLuint fragmentShader = glCreateShader (GL_FRAGMENT_SHADER);
    glShaderSource (vertexShader, 1, &vertex, nullptr);
    glShaderSource (fragmentShader, 1, &fragmentSource, nullptr);
    glCompileShader (vertexShader);
    glCompileShader (fragmentShader);

    GLint compiled = GL_FALSE;
    glGetShaderiv (fragmentShader, GL_COMPILE_STATUS, &compiled);

    if (compiled == GL_FALSE) {
	char log[2048] = {};
	glGetShaderInfoLog (fragmentShader, sizeof (log) - 1, nullptr, log);
	sLog.error ("HDR bloom shader failed to compile: ", log);
    }

    const GLuint program = glCreateProgram ();
    glAttachShader (program, vertexShader);
    glAttachShader (program, fragmentShader);
    glLinkProgram (program);
    glDeleteShader (vertexShader);
    glDeleteShader (fragmentShader);
    return program;
}

// assets/shaders/hdr_downsample.frag
const char* kDownsample = R"(
in vec2 v_TexCoord;
out vec4 fragColor;
uniform sampler2D g_Texture0;
uniform vec4 g_RenderVar0;
uniform float g_BloomStrength;
uniform vec4 g_BloomBlendParams;
uniform vec3 g_BloomTint;
uniform float g_BloomScatter;

#if BICUBIC
vec4 cubic (float v) {
    vec4 n = vec4 (1.0, 2.0, 3.0, 4.0) - v;
    vec4 s = n * n * n;
    float x = s.x;
    float y = s.y - 4.0 * s.x;
    float z = s.z - 4.0 * s.y + 6.0 * s.x;
    float w = 6.0 - x - y - z;
    return vec4 (x, y, z, w) * (1.0 / 6.0);
}

vec4 textureBicubic (vec2 texCoords) {
    float sc = 0.5;
    vec2 texSize = sc / g_RenderVar0.xy;
    vec2 invTexSize = g_RenderVar0.xy / sc;
    texCoords = texCoords * texSize - 0.5;
    vec2 fxy = fract (texCoords);
    texCoords -= fxy;
    vec4 xcubic = cubic (fxy.x);
    vec4 ycubic = cubic (fxy.y);
    vec4 c = texCoords.xxyy + vec2 (-0.5, +1.5).xyxy;
    vec4 s = vec4 (xcubic.xz + xcubic.yw, ycubic.xz + ycubic.yw);
    vec4 offset = c + vec4 (xcubic.yw, ycubic.yw) / s;
    offset *= invTexSize.xxyy;
    vec4 sample0 = texture (g_Texture0, offset.xz);
    vec4 sample1 = texture (g_Texture0, offset.yz);
    vec4 sample2 = texture (g_Texture0, offset.xw);
    vec4 sample3 = texture (g_Texture0, offset.yw);
    float sx = s.x / (s.x + s.y);
    float sy = s.z / (s.z + s.w);
    return mix (mix (sample3, sample2, sx), mix (sample1, sample0, sx), sy);
}
#define SAMPLE(uv) textureBicubic (uv)
#else
#define SAMPLE(uv) texture (g_Texture0, uv)
#endif

void main () {
    vec3 albedo = SAMPLE (v_TexCoord + g_RenderVar0.xy).rgb + SAMPLE (v_TexCoord + g_RenderVar0.zy).rgb
	+ SAMPLE (v_TexCoord + g_RenderVar0.xw).rgb + SAMPLE (v_TexCoord + g_RenderVar0.zw).rgb;
#if UPSAMPLE
    albedo *= 0.25 * g_BloomScatter;
#else
    albedo *= 0.25;
#endif
#if BLOOM
    albedo = max (vec3 (0.0), albedo);
    float brightness = max (albedo.r, max (albedo.g, albedo.b));
    float soft = brightness - g_BloomBlendParams.y;
    soft = clamp (soft, 0.0, g_BloomBlendParams.z);
    soft = soft * soft * g_BloomBlendParams.w;
    float contribution = max (soft, brightness - g_BloomBlendParams.x);
    contribution /= max (brightness, 0.00001);
    albedo *= contribution * g_BloomStrength * g_BloomTint;
#endif
    fragColor = vec4 (albedo, 1.0);
}
)";

// assets/shaders/combine_hdr.frag (combine_hdr_upsample): the output is the linear color on an sRGB back buffer,
// which comes back as the clamped sum. DISPLAYHDR is combine_dhdr_upsample: the unclamped linear color brightened by
// g_RenderVar0.y towards the output's peak where the luma passes 1, linear BT.2020 for an HDR output (CWallpaper's
// linear input) and clipped like the swapchain on an SDR one
const char* kCombine = R"(
in vec2 v_TexCoord;
out vec4 fragColor;
uniform sampler2D g_Texture0;
uniform sampler2D g_Texture1;
uniform vec2 g_TexelSize;
#if DISPLAYHDR
uniform vec4 g_RenderVar0;
uniform bool u_OutputLinear;
const mat3 bt709to2020 = mat3 (0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);

vec3 lin (vec3 v) {
    vec3 c = step (0.04045, v);
    return c * (pow ((v + 0.055) / 1.055, vec3 (2.4))) + (1.0 - c) * (v / 12.92);
}

vec3 srgb (vec3 c) {
    return mix (c * 12.92, 1.055 * pow (c, vec3 (1.0 / 2.4)) - 0.055, step (0.0031308, c));
}
#endif

void main () {
    vec3 albedo = texture (g_Texture0, v_TexCoord).rgb;
    vec3 bloom = texture (g_Texture1, v_TexCoord + g_TexelSize).rgb + texture (g_Texture1, v_TexCoord - g_TexelSize).rgb
	+ texture (g_Texture1, v_TexCoord + vec2 (g_TexelSize.x, -g_TexelSize.y)).rgb
	+ texture (g_Texture1, v_TexCoord + vec2 (-g_TexelSize.x, g_TexelSize.y)).rgb;
#if DISPLAYHDR
    albedo = clamp (albedo, 0.0, 1.0) + bloom * 0.25;
    float hdrFactors = g_RenderVar0.y * smoothstep (1.0, 5.0, dot (vec3 (0.299, 0.587, 0.114), albedo)) + g_RenderVar0.x;
    vec3 light = lin (max (vec3 (0.0), albedo)) * hdrFactors;
    fragColor = vec4 (u_OutputLinear ? bt709to2020 * light : srgb (clamp (light, 0.0, 1.0)), 1.0);
#else
    fragColor = vec4 (clamp (albedo + bloom * 0.25, 0.0, 1.0), 1.0);
#endif
}
)";

void allocateLevel (auto& level, const glm::ivec2 size) {
    level.size = size;
    glGenTextures (1, &level.texture);
    glBindTexture (GL_TEXTURE_2D, level.texture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, size.x, size.y, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers (1, &level.framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, level.framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level.texture, 0);
}

void releaseLevel (auto& level) {
    glDeleteFramebuffers (1, &level.framebuffer);
    glDeleteTextures (1, &level.texture);
    level = {};
}
} // namespace

namespace {
// sub_1400D2C60 with flag 0x10: max (1, min (log2 (np2 (w) / 2), log2 (np2 (h) / 2)) - 2) levels, np2 = the next power
// of two, 8 for a 1920x1058 buffer
uint32_t mipMappedLevels (const uint32_t width, const uint32_t height) {
    const auto halfPowerOfTwo
	= [] (const uint32_t value) { return std::bit_width (std::bit_ceil (std::max (value, 2u)) >> 1) - 1; };

    return static_cast<uint32_t> (
	std::max (1, static_cast<int> (std::min (halfPowerOfTwo (width), halfPowerOfTwo (height))) - 2)
    );
}
} // namespace

void CScene::followOutputSize (const std::shared_ptr<CFBO>& fbo, const float divisor) {
    if (fbo == nullptr) {
	return;
    }

    this->m_outputSizedBuffers.push_back ({ fbo, divisor });

    if (this->m_outputBufferSize.x > 0) {
	fbo->resize (
	    static_cast<uint32_t> (this->m_outputBufferSize.x / divisor),
	    static_cast<uint32_t> (this->m_outputBufferSize.y / divisor)
	);
    }
}

void CScene::resizeOutputBuffers (const glm::ivec2 size) {
    if (size == this->m_outputBufferSize || size.x <= 0 || size.y <= 0) {
	return;
    }

    this->m_outputBufferSize = size;
    std::erase_if (this->m_outputSizedBuffers, [] (const auto& buffer) { return buffer.fbo.expired (); });

    for (const auto& [weak, divisor] : this->m_outputSizedBuffers) {
	weak.lock ()->resize (static_cast<uint32_t> (size.x / divisor), static_cast<uint32_t> (size.y / divisor));
    }

    if (this->_rt_MipMappedFrameBuffer != nullptr) {
	const auto width = static_cast<uint32_t> (size.x);
	const auto height = static_cast<uint32_t> (size.y);
	this->find ("_rt_MipMappedFrameBuffer")->resize (width, height, mipMappedLevels (width, height));
    }
}

std::shared_ptr<const CFBO> CScene::requireMipMappedFrameBuffer () {
    if (this->_rt_MipMappedFrameBuffer != nullptr) {
	return this->_rt_MipMappedFrameBuffer;
    }

    const uint32_t width = this->m_sceneFBO->getRealWidth ();
    const uint32_t height = this->m_sceneFBO->getRealHeight ();

    this->_rt_MipMappedFrameBuffer = this->create (
	"_rt_MipMappedFrameBuffer", this->m_sceneFBO->getFormat (), TextureFlags_ClampUVs, 1.0, { width, height },
	{ width, height }, { 0.0f, 0.0f, 0.0f, 1.0f }, mipMappedLevels (width, height)
    );

    return this->_rt_MipMappedFrameBuffer;
}

std::shared_ptr<const CFBO> CScene::requireReflectionFrameBuffer () {
    if (this->_rt_Reflection != nullptr) {
	return this->_rt_Reflection;
    }

    // output size, format 1 (8 bit even in HDR scenes), a depth buffer, no mips (sub_140181AF0)
    const uint32_t width = this->m_sceneFBO->getRealWidth ();
    const uint32_t height = this->m_sceneFBO->getRealHeight ();

    this->_rt_Reflection = this->create (
	"_rt_Reflection", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { width, height }, { width, height }
    );
    this->_rt_Reflection->attachDepthBuffer ();
    this->followOutputSize (this->_rt_Reflection, 1);

    return this->_rt_Reflection;
}

std::shared_ptr<const CFBO> CScene::requireAlphaMaskFrameBuffer (const bool intermediate) {
    auto& buffer = intermediate ? this->_rt_FullAlphaMaskIntermediate : this->_rt_FullAlphaMask;

    if (buffer != nullptr) {
	return buffer;
    }

    // sub_1401AADB0 with divisor 2, no depth
    const uint32_t width = std::max (this->m_sceneFBO->getRealWidth () / 2, 2u);
    const uint32_t height = std::max (this->m_sceneFBO->getRealHeight () / 2, 2u);

    buffer = this->create (
	intermediate ? "_rt_FullAlphaMaskIntermediate" : "_rt_FullAlphaMask", TextureFormat_R8, TextureFlags_ClampUVs,
	1.0, { width, height }, { width, height }
    );
    this->followOutputSize (buffer, 2);

    return buffer;
}

void CScene::addReflectionReceiver (const int id) { this->m_reflectionReceivers.insert (id); }

void CScene::renderReflection () {
    // sub_14017FA70, when something samples _rt_Reflection (scene flag 1) and the reflection setting (renderer flag
    // 0x80, on by default) is on: before the main pass the scene is drawn again into it, mirrored on the world's y = 0
    // plane (view * diag (1, -1, 1, 1), eye and camera axes mirrored with it). WE also negates the projection's y
    // there (renderer flag 1, always set on its Direct3D renderer), which only makes up for Direct3D's top down
    // render target rows; here the target already lines up with the shader's clip space lookup, and the mirror
    // alone flips the winding, so culling is inverted instead
    if (this->m_reflectionReceivers.empty () || this->_rt_Reflection == nullptr) {
	return;
    }

    auto& camera = this->getCamera ();
    const glm::mat4 view = camera.getWorldView ();
    const glm::mat4 mirrored = view * glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));

    if (camera.isPerspective ()) {
	camera.setPerspectiveView (mirrored, camera.getViewFov ());
    } else {
	camera.setWorldView (mirrored);
	camera.updatePerspectiveLayers ();
    }

    glBindFramebuffer (GL_FRAMEBUFFER, this->_rt_Reflection->getFramebuffer ());
    glViewport (0, 0, this->_rt_Reflection->getRealWidth (), this->_rt_Reflection->getRealHeight ());
    glDepthMask (GL_TRUE);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const glm::vec3 clearColor = this->getScene ().colors.clear->value->getVec3 ();
    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // everything the scene draws goes into the reflection target, like a passthrough layer's buffer
    this->m_layerTargets.push_back (
	{
	    .fbo = this->_rt_Reflection,
	    .transform = glm::mat4 (1.0f),
	    .alphaMax = false,
	    .viewProjection = this->getWorldViewProjection (),
	    .viewProjectionApplied = true,
	}
    );
    this->m_renderingReflection = true;

    // scene +456 (sub_14018FF60): images, models, text and particles in creation order, minus the ones that sample the
    // reflection themselves and the ones with "reflected" false; no lights, sounds, cameras or groups
    for (auto* cur : this->m_objectsByRenderOrder) {
	// the bloom layer stands in for WE's bloom post processing, it's no scene object there
	if (cur == this->m_bloomObject || cur->is<Objects::CLight> () || cur->is<Objects::CSound> ()
	    || cur->is<Objects::CCamera> ()) {
	    continue;
	}

	if (!cur->getObject ().reflected || this->m_reflectionReceivers.contains (cur->getId ())) {
	    continue;
	}

	this->renderSceneObject (cur);
    }

    this->m_renderingReflection = false;
    this->m_layerTargets.pop_back ();

    if (camera.isPerspective ()) {
	camera.setPerspectiveView (view, camera.getViewFov ());
    } else {
	camera.setWorldView (view);
	camera.updatePerspectiveLayers ();
    }
}

void CScene::executeEffectFunction (const ImageEffect& effect, const std::string& name) const {
    for (const auto* object : this->m_objects | std::views::values) {
	if (object->is<Objects::CRenderable> ()
	    && object->as<Objects::CRenderable> ()->executeEffectFunction (effect, name)) {
	    return;
	}
    }
}

GLuint CScene::getSceneDrawFramebuffer () const {
    return this->m_msaaActive ? this->m_msaaFramebuffer : this->m_sceneFBO->getFramebuffer ();
}

bool CScene::prepareMultisample () {
    const glm::ivec2 size (this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight ());

    if (this->m_msaaFramebuffer != GL_NONE && size == this->m_msaaSize) {
	return true;
    }

    this->releaseMultisample ();
    this->m_msaaSize = size;

    // same color format as the scene buffer (sub_140181AF0: RGBA8, RGBA16F in HDR), a resolve needs it
    GLint colorFormat = GL_RGBA8;
    GLint maxSamples = 0;
    glBindTexture (GL_TEXTURE_2D, this->m_sceneFBO->getTextureID (0));
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &colorFormat);
    glGetIntegerv (GL_MAX_SAMPLES, &maxSamples);

    // the device check (CheckMultisampleQualityLevels in sub_14012AC60) falls back to one sample, no MSAA
    if (this->m_msaaSamples > maxSamples) {
	sLog.error (
	    "MSAA x", this->m_msaaSamples, " is not supported here (at most ", maxSamples, "), drawing without"
	);
	this->m_msaaSamples = 0;
	return false;
    }

    glGenRenderbuffers (1, &this->m_msaaColor);
    glBindRenderbuffer (GL_RENDERBUFFER, this->m_msaaColor);
    glRenderbufferStorageMultisample (GL_RENDERBUFFER, this->m_msaaSamples, colorFormat, size.x, size.y);

    if (this->m_sceneFBO->getDepthbuffer () != GL_NONE) {
	glGenRenderbuffers (1, &this->m_msaaDepth);
	glBindRenderbuffer (GL_RENDERBUFFER, this->m_msaaDepth);
	glRenderbufferStorageMultisample (GL_RENDERBUFFER, this->m_msaaSamples, GL_DEPTH_COMPONENT24, size.x, size.y);
    }

    glBindRenderbuffer (GL_RENDERBUFFER, 0);
    glGenFramebuffers (1, &this->m_msaaFramebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_msaaFramebuffer);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, this->m_msaaColor);

    if (this->m_msaaDepth != GL_NONE) {
	glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, this->m_msaaDepth);
    }

    const bool complete = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_sceneFBO->getFramebuffer ());

    if (!complete) {
	sLog.error ("Cannot create the MSAA x", this->m_msaaSamples, " scene buffer, drawing without");
	this->releaseMultisample ();
	this->m_msaaSamples = 0;
	return false;
    }

#if !NDEBUG
    glObjectLabel (GL_FRAMEBUFFER, this->m_msaaFramebuffer, -1, "_rt_FullFrameBufferMultiSampled");
#endif

    return true;
}

void CScene::releaseMultisample () {
    if (this->m_msaaFramebuffer != GL_NONE) {
	glDeleteFramebuffers (1, &this->m_msaaFramebuffer);
    }

    if (this->m_msaaColor != GL_NONE) {
	glDeleteRenderbuffers (1, &this->m_msaaColor);
    }

    if (this->m_msaaDepth != GL_NONE) {
	glDeleteRenderbuffers (1, &this->m_msaaDepth);
    }

    this->m_msaaFramebuffer = this->m_msaaColor = this->m_msaaDepth = GL_NONE;
}

void CScene::resolveMultisample () const {
    if (!this->m_msaaActive) {
	return;
    }

    GLint readFramebuffer = 0;
    GLint drawFramebuffer = 0;
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);

    const GLbitfield mask = GL_COLOR_BUFFER_BIT | (this->m_msaaDepth != GL_NONE ? GL_DEPTH_BUFFER_BIT : 0);

    glBindFramebuffer (GL_READ_FRAMEBUFFER, this->m_msaaFramebuffer);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_sceneFBO->getFramebuffer ());
    glBlitFramebuffer (
	0, 0, this->m_msaaSize.x, this->m_msaaSize.y, 0, 0, this->m_msaaSize.x, this->m_msaaSize.y, mask, GL_NEAREST
    );
    glBindFramebuffer (GL_READ_FRAMEBUFFER, readFramebuffer);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, drawFramebuffer);
}

void CScene::updateMipMappedFrameBuffer () const {
    if (this->_rt_MipMappedFrameBuffer == nullptr) {
	return;
    }

    const auto width = static_cast<GLint> (this->m_sceneFBO->getRealWidth ());
    const auto height = static_cast<GLint> (this->m_sceneFBO->getRealHeight ());

    this->resolveMultisample ();
    glBindFramebuffer (GL_READ_FRAMEBUFFER, this->m_sceneFBO->getFramebuffer ());
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->_rt_MipMappedFrameBuffer->getFramebuffer ());
    glBlitFramebuffer (0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    this->_rt_MipMappedFrameBuffer->generateMipmaps ();
    glBindFramebuffer (GL_FRAMEBUFFER, this->getSceneDrawFramebuffer ());
}

void CScene::releaseHDRBloom () {
    for (auto& level : this->m_bloomLevels) {
	releaseLevel (level);
    }

    this->m_bloomLevels.clear ();

    if (this->m_hdrCopy.texture != GL_NONE) {
	releaseLevel (this->m_hdrCopy);
    }
}

glm::ivec2 CScene::getOutputResolution () const {
    return { std::max (this->getFramebufferWidth (), 1), std::max (this->getFramebufferHeight (), 1) };
}

glm::mat4 CScene::getWorldViewProjection (const bool perspectiveLayer) const {
    const auto& camera = this->getCamera ();

    if (!camera.isOrthogonal ()) {
	return camera.getPerspective () * camera.getView ();
    }

    const glm::vec3 center (this->getWidth () * 0.5f, this->getHeight () * 0.5f, 0.0f);
    // "perspective" objects get the perspective layer camera (sub_1401E5B60), models too (sub_1402222A0)
    const glm::mat4 viewProjection = perspectiveLayer ? camera.getPerspectiveLayerViewProjection ()
						      : camera.getProjection () * camera.getLookAt ();

    return viewProjection * glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f))
	* glm::translate (glm::mat4 (1.0f), -center);
}

void CScene::renderHDRBloom () {
    if (this->m_bloomDownsample == GL_NONE) {
	this->m_bloomDownsample = buildProgram ("", kDownsample);
	this->m_bloomDownsampleThreshold = buildProgram ("#define BLOOM 1\n", kDownsample);
	this->m_bloomUpsample = buildProgram ("#define UPSAMPLE 1\n", kDownsample);
	this->m_bloomUpsampleCubic = buildProgram ("#define UPSAMPLE 1\n#define BICUBIC 1\n", kDownsample);
	this->m_bloomCombine = buildProgram (this->m_displayHDR ? "#define DISPLAYHDR 1\n" : "", kCombine);
    }

    // the chain starts at half the output's resolution
    const glm::ivec2 resolution = this->getOutputResolution ();

    // levels: halvings of the output's smaller side, at most "bloomhdriterations" and the 8 buffers WE creates
    int levels = 0;

    for (int side = std::min (resolution.x, resolution.y) / 2; side > 0; side /= 2) {
	levels++;
    }

    const auto& bloom = this->getScene ().camera.bloom;
    levels = std::clamp (std::min (levels, bloom.hdrIterations->value->getInt ()), 1, 8);

    if (resolution != this->m_bloomResolution || static_cast<int> (this->m_bloomLevels.size ()) != levels) {
	this->releaseHDRBloom ();
	this->m_bloomResolution = resolution;

	for (int level = 0; level < levels; level++) {
	    auto& entry = this->m_bloomLevels.emplace_back ();
	    allocateLevel (entry, glm::max (resolution / (2 << level), glm::ivec2 (1)));
	}

	allocateLevel (this->m_hdrCopy, { this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight () });
    }

    const float strength = bloom.hdrStrength->value->getFloat ();
    const float threshold = bloom.hdrThreshold->value->getFloat ();
    const float feather = bloom.hdrFeather->value->getFloat ();
    const float scatter = bloom.hdrScatter->value->getFloat ();
    const glm::vec3 tint = bloom.tint->value->getVec3 ();
    // sub_140184020
    const float scaledStrength = strength / (std::pow (scatter, static_cast<float> (std::max (levels, 2) - 2)) + 1.0f);
    const float knee = threshold * feather;
    const glm::vec4 blend (threshold, threshold - knee, knee + knee, 0.25f / (knee + 0.0000099999997f));
    const glm::vec2 texel (1.0f / static_cast<float> (resolution.x), 1.0f / static_cast<float> (resolution.y));

    const auto draw = [&] (GLuint program, const BloomLevel& target, GLuint source, float step) {
	glBindFramebuffer (GL_FRAMEBUFFER, target.framebuffer);
	glViewport (0, 0, target.size.x, target.size.y);
	glUseProgram (program);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, source);
	glUniform1i (glGetUniformLocation (program, "g_Texture0"), 0);
	glUniform4f (
	    glGetUniformLocation (program, "g_RenderVar0"), texel.x * step, texel.y * step, -texel.x * step,
	    -texel.y * step
	);
	glUniform1f (glGetUniformLocation (program, "g_BloomStrength"), scaledStrength);
	glUniform4fv (glGetUniformLocation (program, "g_BloomBlendParams"), 1, &blend.x);
	glUniform3fv (glGetUniformLocation (program, "g_BloomTint"), 1, &tint.x);
	glUniform1f (glGetUniformLocation (program, "g_BloomScatter"), scatter);
	glDrawArrays (GL_TRIANGLES, 0, 3);
    };

    glBindVertexArray (this->m_vaoBuffer);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glDisable (GL_BLEND);
    glColorMask (true, true, true, true);

    // sub_140183610: threshold into the first level, halve down the chain, then add each level back onto the one
    // above it, the last two of those with the bicubic filter
    draw (this->m_bloomDownsampleThreshold, this->m_bloomLevels[0], this->m_sceneFBO->getTextureID (0), 1.0f);

    for (int level = 1; level < levels; level++) {
	draw (
	    this->m_bloomDownsample, this->m_bloomLevels[level], this->m_bloomLevels[level - 1].texture,
	    static_cast<float> (1 << level)
	);
    }

    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE);

    for (int level = levels - 1; level > 0; level--) {
	draw (
	    level < levels - 2 ? this->m_bloomUpsample : this->m_bloomUpsampleCubic, this->m_bloomLevels[level - 1],
	    this->m_bloomLevels[level].texture, static_cast<float> (2 << (level - 1))
	);
    }

    glDisable (GL_BLEND);

    // combine_hdr_upsample reads the scene, so it gets a copy of it and writes back into the scene buffer
    const GLuint sceneFramebuffer = this->m_sceneFBO->getFramebuffer ();
    const int width = this->m_sceneFBO->getRealWidth ();
    const int height = this->m_sceneFBO->getRealHeight ();
    glBindFramebuffer (GL_READ_FRAMEBUFFER, sceneFramebuffer);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_hdrCopy.framebuffer);
    glBlitFramebuffer (0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer (GL_FRAMEBUFFER, sceneFramebuffer);
    glViewport (0, 0, width, height);
    glUseProgram (this->m_bloomCombine);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->m_hdrCopy.texture);
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, this->m_bloomLevels[0].texture);
    glActiveTexture (GL_TEXTURE0);
    glUniform1i (glGetUniformLocation (this->m_bloomCombine, "g_Texture0"), 0);
    glUniform1i (glGetUniformLocation (this->m_bloomCombine, "g_Texture1"), 1);
    glUniform2f (glGetUniformLocation (this->m_bloomCombine, "g_TexelSize"), texel.x, texel.y);

    if (this->m_displayHDR) {
	// g_RenderVar0 = (white, peak - white) / 80 in WE's scRGB (sub_14012AC60): peak at least 80 nits, white the SDR
	// white level within [80, peak], (1, 1) on an SDR monitor. Here 1.0 is the output's reference white, which the
	// PQ encoding anchors to the compositor's SDR white, so both are taken relative to that
	glm::vec2 renderVar (1.0f, 1.0f);
	const glm::vec2& luminance = this->getOutputLuminance ();

	if (this->isOutputHDR () && luminance.x > 0.0f) {
	    const float peak = std::max (luminance.y, 80.0f);
	    const float white = std::max (std::min (luminance.x, peak), 80.0f);
	    renderVar = glm::vec2 (white, peak - white) / luminance.x;
	}

	glUniform4f (glGetUniformLocation (this->m_bloomCombine, "g_RenderVar0"), renderVar.x, renderVar.y, 0.0f, 0.0f);
	glUniform1i (glGetUniformLocation (this->m_bloomCombine, "u_OutputLinear"), this->isOutputHDR () ? 1 : 0);
	this->setLinearInput (this->isOutputHDR ());
    }

    glDrawArrays (GL_TRIANGLES, 0, 3);
}

void CScene::renderCameraFade () {
    if (this->m_fadeProgram == GL_NONE) {
	// materials/util/fade.json: flat color * 0.7 at the fade's alpha over the whole frame
	const char* vertex = "#version 330\n"
			     "void main () {\n"
			     "vec2 corner = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
			     "gl_Position = vec4 (corner * 2.0 - 1.0, 0.0, 1.0);\n"
			     "}";
	const char* fragment = "#version 330\n"
			       "uniform vec3 color;\n"
			       "uniform float g_Alpha;\n"
			       "out vec4 fragColor;\n"
			       "void main () { fragColor = vec4 (color * 0.7, g_Alpha); }";
	const GLuint vertexShader = glCreateShader (GL_VERTEX_SHADER);
	const GLuint fragmentShader = glCreateShader (GL_FRAGMENT_SHADER);
	glShaderSource (vertexShader, 1, &vertex, nullptr);
	glShaderSource (fragmentShader, 1, &fragment, nullptr);
	glCompileShader (vertexShader);
	glCompileShader (fragmentShader);
	this->m_fadeProgram = glCreateProgram ();
	glAttachShader (this->m_fadeProgram, vertexShader);
	glAttachShader (this->m_fadeProgram, fragmentShader);
	glLinkProgram (this->m_fadeProgram);
	glDeleteShader (vertexShader);
	glDeleteShader (fragmentShader);
    }

    // the shader's tint comes from the project's schemecolor ("usershadervalues"), its default otherwise
    glm::vec3 color (0.315f, 0.135f, 0.1125f);
    const auto& properties = this->getUserProperties ();

    if (const auto scheme = properties.find ("schemecolor"); scheme != properties.end () && scheme->second != nullptr) {
	color = scheme->second->getVec3 ();
    }

    glBindVertexArray (this->m_vaoBuffer);
    glUseProgram (this->m_fadeProgram);
    glUniform3fv (glGetUniformLocation (this->m_fadeProgram, "color"), 1, &color.x);
    glUniform1f (glGetUniformLocation (this->m_fadeProgram, "g_Alpha"), std::min (this->m_cameraFade, 1.0f));
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glEnable (GL_BLEND);
    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays (GL_TRIANGLES, 0, 3);
}

void CScene::updateParallax () {
    if (this->getScene ().camera.parallax.enabled->value->getBool ()) {
	// the wallpaper's own position rides through the same per-layer depth/clamp mechanism as mouse parallax, halved
	// to match its range X keeps WallpaperState's sign flip, Y does not because this displacement is applied after
	// the Y-up conversion
	const glm::vec2 positionBias
	    = { -this->getState ().getOffsetX () * 0.5f, this->getState ().getOffsetY () * 0.5f };

	if (this->getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	    // no mouse contribution left to smooth toward, apply position directly
	    this->m_parallaxBias = positionBias;
	    this->m_cameraParallax = { 0.0f, 0.0f };
	} else {
	    const float influence = this->getScene ().camera.parallax.mouseInfluence->value->getFloat ();
	    const float amount = this->getScene ().camera.parallax.amount->value->getFloat ();
	    const float delay = this->getScene ().camera.parallax.delay->value->getFloat ();

	    this->m_parallaxBias = positionBias * amount * influence;

	    // same easing as the real engine: no delay snaps to the mouse, otherwise a fast exponential follow
	    // a camera object's eye moves the parallax camera too, before the smoothing (sub_1401891A0). All of it
	    // is y up like WE's world: the cursor's y is flipped, the eye isn't
	    const glm::vec2 eye = { this->m_cameraEye.x / static_cast<float> (this->getWidth ()),
				    this->m_cameraEye.y / static_cast<float> (this->getHeight ()) };
	    const glm::vec2 target = (this->m_mousePosition - glm::vec2 (0.5f, 0.5f)) * influence + eye;
	    if (delay <= 0.0f) {
		this->m_cameraParallax = target;
	    } else {
		const float dt = std::max (g_Time - g_TimeLast, 0.0f);
		const float follow = std::min (1.0f, (1.0f - delay / 3.0f) * 10.0f * dt);
		this->m_cameraParallax += (target - this->m_cameraParallax) * follow;
	    }
	}

	this->m_parallaxPosition = glm::clamp (glm::vec2 (0.5f) + this->m_cameraParallax, 0.0f, 1.0f);
    }
}

glm::mat4 CScene::objectLocalMatrix (const Object& object) {
    // images and particles keep their own (scriptable) scale and angles next to the base object's copy
    glm::vec3 scale = object.groupScale->value->getVec3 ();
    glm::vec3 angles = object.groupAngles->value->getVec3 ();

    if (object.is<Image> ()) {
	scale = object.as<Image> ()->scale->value->getVec3 ();
	angles = object.as<Image> ()->angles->value->getVec3 ();
    } else if (object.is<Particle> ()) {
	scale = object.as<Particle> ()->scale->value->getVec3 ();
	angles = object.as<Particle> ()->angles->value->getVec3 ();
    } else if (object.is<Text> ()) {
	scale = object.as<Text> ()->scale->value->getVec3 ();
    }

    glm::mat4 transform = glm::translate (glm::mat4 (1.0f), object.origin->value->getVec3 ());
    transform = glm::rotate (transform, angles.z, glm::vec3 (0.0f, 0.0f, 1.0f));
    transform = glm::rotate (transform, angles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
    transform = glm::rotate (transform, angles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
    return glm::scale (transform, scale);
}

glm::mat4 CScene::objectWorldMatrix (const Object& object) const {
    glm::mat4 world = objectLocalMatrix (object);
    const Object* current = &object;

    for (int depth = 0; current->parent.has_value () && depth < 64; depth++) {
	const CObject* parent = this->getObject (current->parent.value ());

	if (parent == nullptr) {
	    break;
	}

	// sub_1401850A0: a parent model moves its child's local matrix onto the child's "attachment" point first
	// (vtable slot 15, sub_140224970)
	if (current->attachment.has_value () && parent->is<Objects::CMesh> ()) {
	    if (const auto attachment = parent->as<Objects::CMesh> ()->getAttachmentMatrix (*current->attachment)) {
		world = *attachment * world;
	    }
	}

	current = &parent->getObject ();
	world = objectLocalMatrix (*current) * world;
    }

    return world;
}

void CScene::updateCameraPath (const float dt, glm::vec3& eye, glm::vec3& center, glm::vec3& up, float& zoom) {
    // sub_1401891A0: the keys of one path at a time, cubic Hermite between two keys with tangents of half their
    // difference, then the next path once the last key is done
    const auto& paths = this->getScene ().camera.paths;
    this->m_pathIndex %= static_cast<int> (paths.size ());
    const auto& path = paths[this->m_pathIndex];

    if (path.keys.empty ()) {
	this->m_pathIndex = (this->m_pathIndex + 1) % static_cast<int> (paths.size ());
	return;
    }

    const int count = static_cast<int> (path.keys.size ());
    this->m_pathKey = std::min (this->m_pathKey, count - 1);
    const auto& key = path.keys[this->m_pathKey];
    const float time = this->m_pathTime;
    float until;

    eye = key.eye;
    center = key.center;
    up = key.up;
    zoom = key.zoom;

    if (time < key.time) {
	until = (this->m_pathKey + 1 < count ? path.keys[this->m_pathKey + 1].time : 0.0f) + key.time;
    } else if (this->m_pathKey + 1 < count) {
	const auto& next = path.keys[this->m_pathKey + 1];
	const float s = (time - key.time) / (next.time - key.time);
	const float s2 = s * s;
	const float s3 = s2 * s;
	const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
	const float h10 = s3 - 2.0f * s2 + s;
	const float h01 = 3.0f * s2 - 2.0f * s3;
	const float h11 = s3 - s2;
	const auto hermite = [&] (const auto& a, const auto& b) {
	    const auto tangent = (b - a) * 0.5f;
	    return a * h00 + tangent * h10 + b * h01 + tangent * h11;
	};

	eye = hermite (key.eye, next.eye);
	center = hermite (key.center, next.center);
	up = hermite (key.up, next.up);
	zoom = hermite (key.zoom, next.zoom);
	until = next.time;
    } else {
	until = path.duration - key.time;
    }

    this->m_pathTime = time + dt;

    if (this->m_pathTime > until) {
	if (this->m_pathKey + 1 >= count || path.duration <= path.keys[this->m_pathKey + 1].time) {
	    this->m_pathIndex = (this->m_pathIndex + 1) % static_cast<int> (paths.size ());
	    this->m_pathKey = 0;
	    this->m_pathTime = 0.0f;
	} else {
	    this->m_pathKey++;
	}
    }

    // camerafade darkens the first and last half second of every path (sub_14017FA70)
    if (this->getScene ().camera.fade->value->getBool ()) {
	const auto& current = paths[this->m_pathIndex];
	const float left = current.duration - this->m_pathTime;

	if (left < 0.5f) {
	    this->m_cameraFade = 1.0f - 2.0f * left;
	} else if (left > current.duration - 0.5f) {
	    this->m_cameraFade = 1.0f - 2.0f * (current.duration - left);
	}
    }
}

void CScene::updateFog (const glm::vec3& eye) {
    const auto& fog = this->getScene ().fog;
    const auto params = [] (const auto& start, const auto& end, const auto& startDensity, const auto& endDensity) {
	const float from = start->value->getFloat ();
	const float density = startDensity->value->getFloat ();
	return glm::vec4 (from, end->value->getFloat () - from, density, endDensity->value->getFloat () - density);
    };

    this->m_fog.distanceColor = fog.distanceColor->value->getVec3 ();
    this->m_fog.distanceParams
	= params (fog.distanceStart, fog.distanceEnd, fog.distanceStartDensity, fog.distanceEndDensity);
    this->m_fog.heightColor = fog.heightColor->value->getVec3 ();
    this->m_fog.heightParamsWorld
	= params (fog.heightStart, fog.heightEnd, fog.heightStartDensity, fog.heightEndDensity);
    this->m_fog.heightParamsLocal = this->m_fog.heightParamsWorld;
    this->m_fog.eyeWorld = eye;
    this->m_fog.eyeLocal = eye;

    if (!this->m_camera->isPerspective ()) {
	// 2D scenes: WE's eye sits 2000 units out over the scene center plus the camera (end of sub_1401891A0), and
	// a height of y here is H/2 - y there
	const float height = static_cast<float> (this->getHeight ());
	this->m_fog.eyeWorld
	    = glm::vec3 (static_cast<float> (this->getWidth ()) * 0.5f + eye.x, height * 0.5f + eye.y, 2000.0f);
	this->m_fog.eyeLocal = glm::vec3 (eye.x, -eye.y, 2000.0f);
	this->m_fog.heightParamsLocal.x = height * 0.5f - this->m_fog.heightParamsWorld.x;
	this->m_fog.heightParamsLocal.y = -this->m_fog.heightParamsWorld.y;
    }
}

void CScene::updateCamera () {
    const auto& appContext = this->getContext ().getApp ().getContext ();
    Objects::CCamera* active = nullptr;

    for (CObject* object : this->m_sceneCameras) {
	const auto override = appContext.resolveObjectVisibility (object->getId (), object->getObject ().name);

	if (override.value_or (object->getObject ().groupVisible->value->getBool ())
	    && !this->isHiddenByAncestor (*object)) {
	    active = object->as<Objects::CCamera> ();
	}
    }

    const float dt = std::max (g_Time - g_TimeLast, 0.0f);

    // without a camera object or path the camera is the static one, which scripts can move
    const bool perspective = this->m_camera->isPerspective ();
    const StaticCamera& base = this->getStaticCamera ();
    glm::vec3 eye = base.eye;
    glm::vec3 center = base.center;
    glm::vec3 up = base.up;
    float fov = this->m_camera->getFov ();
    float zoom = base.zoom;
    this->m_cameraFade = 0.0f;

    if (active != nullptr) {
	glm::mat4 world = this->objectWorldMatrix (active->getObject ());
	zoom = active->getCamera ().zoom->value->getFloat ();
	fov = active->getCamera ().fov->value->getFloat ();

	// a camera path file drives the camera's transform, zoom and fov (sub_1401F2AD0)
	const auto& data = active->getObject ();
	const CObject* parent = data.parent.has_value () ? this->getObject (data.parent.value ()) : nullptr;
	const glm::mat4 parentWorld
	    = parent != nullptr ? this->objectWorldMatrix (parent->getObject ()) : glm::mat4 (1.0f);

	if (const auto pose = active->updateTimeline (dt, world, parentWorld); pose.has_value ()) {
	    world = pose->world;
	    zoom = pose->zoom;
	    fov = pose->fov;
	}

	// eye at the camera's position, looking down its -z axis, its y axis up
	eye = glm::vec3 (world[3]);
	center = eye - glm::vec3 (world[2]);
	up = glm::vec3 (world[1]);
    } else if (!this->getScene ().camera.paths.empty ()) {
	this->updateCameraPath (dt, eye, center, up, zoom);
    }

    if (this->getScene ().camera.shake.enabled->value->getBool ()) {
	// sub_140199580: speed and roughness shape a cos/sin wobble on the scene clock, it moves eye and center alike
	const auto& shake = this->getScene ().camera.shake;
	const float roughness = std::pow (shake.roughness->value->getFloat (), 3.0f);
	const float speed = shake.speed->value->getFloat ();
	const float time = speed * speed * this->getSceneClock ();
	glm::vec3 offset (std::cos (time), std::sin (time * 1.333f), perspective ? std::sin (time) : 0.0f);
	float amplitude = shake.amplitude->value->getFloat () * 0.1f;

	if (!perspective) {
	    amplitude *= static_cast<float> (this->getHeight ()) * 0.1f;
	}

	if (roughness > 0.001f && roughness != 1.0f) {
	    const float length = glm::length (offset);
	    offset *= std::pow (length, roughness) / length;
	}

	eye += offset * amplitude;
	center += offset * amplitude;
    }

    const glm::mat4 view = glm::lookAt (eye, center, up);
    this->m_cameraEye = perspective ? glm::vec2 (0.0f) : glm::vec2 (eye);
    this->updateFog (eye);

    if (perspective) {
	this->m_camera->setVisibleRegion ({}, this->getOutputAspect ());
	this->m_camera->setPerspectiveView (view, fov);
	return;
    }

    this->m_camera->setWorldView (view);
    this->m_camera->setZoom (this->getScene ().camera.projection.zoom->value->getFloat () * zoom);

    this->m_camera->setVisibleRegion (this->getVisibleMargins (), this->getOutputAspect ());
    this->m_camera->updatePerspectiveLayers ();
}

void CScene::dispatchCursorEvents () {
    if (!this->getContext ().getApp ().getContext ().settings.mouse.enabled) {
	return;
    }

    const bool down
	= this->getContext ().getInputContext ().getMouseInput ().leftClick () == Input::MouseClickStatus::Clicked;
    const bool pressed = down && !this->m_cursorLeftDown;
    const bool released = !down && this->m_cursorLeftDown;
    this->m_cursorLeftDown = down;

    const glm::vec2 scenePosition = { this->m_mousePositionNormalized.x * static_cast<float> (this->getWidth ()),
				      this->m_mousePositionNormalized.y * static_cast<float> (this->getHeight ()) };
    const bool moved = scenePosition != this->m_cursorLastScenePosition;
    this->m_cursorLastScenePosition = scenePosition;

    auto& engine = this->getScriptEngine ();
    // handlers are free to create layers, which would move things around under a live iteration. 3D scenes walk a
    // copy sorted by depth like the transparent pass (sub_1401865C0), 2D ones the creation order
    const auto objects = this->getCamera ().isPerspective () ? this->sortedByDepth (this->m_objectsByRenderOrder)
							     : this->m_objectsByRenderOrder;
    // WE (sub_140189E10) checks for a drag before the pass: while the button is held on something pressed,
    // only the pressed objects hear about it, and only through cursorMove
    const bool dragging = down && !this->m_cursorPressed.empty ();
    // scene buffer clip space, what the objects' matrices project to; the buffer is stored upside down, clip y grows
    // towards the bottom of the screen like the cursor's
    const glm::vec2 ndc = this->m_cursorScreen * 2.0f - 1.0f;

    // topmost first, like WE. Hidden objects are hit tested and get events too, they just can't stop propagation.
    // Images and text (object types 1 and 4) test their quad, models (type 5) their bounds
    for (auto it = objects.rbegin (); it != objects.rend (); ++it) {
	auto* cur = *it;

	if (!cur->getObject ().solid->value->getBool ()) {
	    continue;
	}

	Scripting::ScriptableObject* scriptable = nullptr;
	bool hit = false;
	bool visible = false;
	// the event's localPosition is whatever the object's hit test left behind (sub_14019DBB0 / sub_140185520),
	// quads fill it in off the quad too
	glm::vec3 local {};

	if (cur->is<Objects::CImage> ()) {
	    auto* image = cur->as<Objects::CImage> ();

	    scriptable = image;
	    hit = image->hitTest (ndc);
	    // fullscreen layers get the cursor in window pixels: WE stores ScreenToClient / client size and multiplies
	    // it back by the client size (sub_140110630, sub_14017F1B0)
	    local = image->getImage ().model->fullscreen ? glm::vec3 (this->m_mousePositionViewport, 0.0f)
							 : glm::vec3 (image->cursorLocalPosition (ndc), 0.0f);
	    visible = image->getImage ().visible->value->getBool ();
	} else if (cur->is<Objects::CText> ()) {
	    auto* text = cur->as<Objects::CText> ();

	    scriptable = text;
	    hit = text->hitTest (ndc);
	    local = glm::vec3 (text->cursorLocalPosition (ndc), 0.0f);
	    visible = text->getText ().visible->value->getBool ();
	} else if (cur->is<Objects::CMesh> ()) {
	    auto* mesh = cur->as<Objects::CMesh> ();

	    scriptable = mesh;
	    hit = mesh->hitTest (ndc);
	    local = mesh->cursorLocalPosition (ndc);
	    visible = mesh->getMesh ().groupVisible->value->getBool ();
	} else {
	    continue;
	}

	const int id = cur->getId ();
	const bool handlers = engine.hasCursorHandlers (*scriptable);
	const auto dispatch = [&] (const char* event) {
	    if (handlers) {
		engine.dispatchCursorEvent (event, *scriptable, scenePosition, local);
	    }
	};

	if (dragging) {
	    if (moved && this->m_cursorPressed.contains (id)) {
		dispatch ("cursorMove");
	    }

	    continue;
	}

	if (!hit) {
	    if (released && this->m_cursorPressed.contains (id)) {
		dispatch ("cursorUp");
	    }

	    if (this->m_cursorInside.erase (id) > 0) {
		dispatch ("cursorLeave");
	    }

	    continue;
	}

	if (this->m_cursorInside.insert (id).second) {
	    dispatch ("cursorEnter");
	}

	if (moved) {
	    dispatch ("cursorMove");
	}

	if (pressed) {
	    dispatch ("cursorDown");
	    this->m_cursorPressed.insert (id);
	} else if (released) {
	    dispatch ("cursorUp");

	    if (this->m_cursorPressed.contains (id)) {
		dispatch ("cursorClick");
	    }
	}

	if (!cur->getObject ().disablePropagation->value->getBool ()) {
	    continue;
	}

	const auto visibility
	    = this->getContext ().getApp ().getContext ().resolveObjectVisibility (id, cur->getObject ().name);

	if (visibility.value_or (visible) && !this->isHiddenByAncestor (*cur)) {
	    break;
	}
    }

    if (!down) {
	this->m_cursorPressed.clear ();
    }
}

void CScene::updateMouse (const glm::ivec4& viewport) {
    const glm::dvec2 position = this->getContext ().getInputContext ().getMouseInput ().position ();

    this->m_pointerPositionLast = this->m_pointerPosition;
    this->m_mousePositionViewport = glm::vec2 (position.x - viewport.x, position.y - viewport.y);

    double mouseX = glm::clamp ((position.x - viewport.x) / viewport.z, 0.0, 1.0);

    // WE flips by mirroring its projection, so the cursor lands on what is shown under it
    if (this->isFlippedHorizontally ()) {
	mouseX = 1.0 - mouseX;
    }

    // OpenGL convention (0=bottom, 1=top) - particle code expects 0=bottom as negative Y (down)
    double normalizedMouseY = glm::clamp ((position.y - viewport.y) / viewport.w, 0.0, 1.0);

    // the scene buffer holds what the output shows, the visible region maps the cursor onto the scene
    const glm::vec4 output = this->getOutputUVs ();
    const glm::vec4 visible = this->getVisibleUVs ();

    this->m_cursorScreen.x = output.x + mouseX * (output.y - output.x);
    this->m_cursorScreen.y = output.z + normalizedMouseY * (output.w - output.z);
    this->m_mousePositionNormalized.x = visible.x + mouseX * (visible.y - visible.x);
    this->m_mousePositionNormalized.y = visible.z + normalizedMouseY * (visible.w - visible.z);

    // invert the Y normalization above to match what the shader expects
    double mouseY = 1.0 - normalizedMouseY;

    this->m_mousePosition.x = this->m_cursorScreen.x;
    this->m_mousePosition.y = output.z + mouseY * (output.w - output.z);
    // g_PointerPosition is ScreenToClient over the client size, never mirrored by alignmentfliph (sub_140110630,
    // uniform 105 reads renderer +140 in sub_1400D8300)
    const float pointerX = this->isFlippedHorizontally () ? 1.0f - this->m_mousePosition.x : this->m_mousePosition.x;
    this->m_pointerPosition = { pointerX, this->m_mousePosition.y };
}

glm::vec4 CScene::getVisibleUVs () const {
    const auto& state = this->getState ();

    // 3D scenes keep the whole view, the perspective takes the output's aspect instead
    if (this->m_camera->isPerspective () || state.getViewportWidth () <= 0 || state.getViewportHeight () <= 0) {
	return this->getOutputUVs ();
    }

    const auto uvs = state.getTextureUVs ();
    return { uvs.ustart, uvs.uend, uvs.vstart, uvs.vend };
}

glm::vec4 CScene::getVisibleMargins () const {
    const glm::vec4 uvs = this->getVisibleUVs ();
    const float width = this->getCanvasWidth ();
    const float height = this->getCanvasHeight ();
    const bool vflip = this->getState ().isVFlipped ();

    // v runs top down in a flipped state, bottom up otherwise
    return { uvs.x * width, (1.0f - uvs.y) * width, (vflip ? 1.0f - uvs.w : uvs.w) * height,
	     (vflip ? uvs.z : 1.0f - uvs.z) * height };
}

glm::vec4 CScene::getVisibleCanvasRegion () const {
    glm::vec4 margins = glm::max (this->getVisibleMargins (), glm::vec4 (0.0f));
    const float halfWidth = static_cast<float> (this->getCanvasWidth ()) / 2.0f;
    const float halfHeight = static_cast<float> (this->getCanvasHeight ()) / 2.0f;

    // the margins are on screen, a mirrored projection puts the left one on the canvas' right
    if (this->isFlippedHorizontally ()) {
	std::swap (margins.x, margins.y);
    }

    return { -halfWidth + margins.x, halfWidth - margins.y, -halfHeight + margins.w, halfHeight - margins.z };
}

const Data::Model::Properties& CScene::getUserProperties () const {
    return this->getWallpaperData ().project.properties;
}

const Scene& CScene::getScene () const { return *this->getWallpaperData ().as<Scene> (); }

void CScene::expandCanvasToContent (
    const Scene& scene, const float width, const float height, float& canvasWidth, float& canvasHeight
) const {
    // keeps the expanded canvas within what the GPU is guaranteed to be able to allocate for a framebuffer
    constexpr float maxCanvasSize = 8192.0f;

    float extentX = width / 2.0f;
    float extentY = height / 2.0f;

    for (const auto& object : scene.objects) {
	if (!object->is<Image> ()) {
	    continue;
	}

	const auto* image = object->as<Image> ();

	// children are positioned relative to their parent, and a layer with no declared size takes its
	// texture's, which isn't known this early
	if (image->parent.has_value () || image->size.x <= 0.0f || image->size.y <= 0.0f || image->origin == nullptr
	    || image->scale == nullptr) {
	    continue;
	}

	const auto visibility
	    = this->getContext ().getApp ().getContext ().resolveObjectVisibility (image->id, image->name);

	if (visibility.has_value () && !visibility.value ()) {
	    continue;
	}

	if (image->visible != nullptr && !image->visible->value->getBool ()) {
	    continue;
	}

	const glm::vec3 origin = image->origin->value->getVec3 ();
	const glm::vec3 scale = image->scale->value->getVec3 ();

	// rotation is ignored on purpose, a rotated layer just keeps the extent its unrotated box has
	extentX = std::max (extentX, std::abs (origin.x - width / 2.0f) + std::abs (scale.x) * image->size.x / 2.0f);
	extentY = std::max (extentY, std::abs (origin.y - height / 2.0f) + std::abs (scale.y) * image->size.y / 2.0f);
    }

    canvasWidth = std::min (std::ceil (extentX * 2.0f), std::max (width, maxCanvasSize));
    canvasHeight = std::min (std::ceil (extentY * 2.0f), std::max (height, maxCanvasSize));

    if (canvasWidth != width || canvasHeight != height) {
	sLog.out ("Expanded scene canvas from ", width, "x", height, " to ", canvasWidth, "x", canvasHeight);
    }
}

int CScene::getWidth () const { return this->m_camera->getWidth (); }

int CScene::getHeight () const { return this->m_camera->getHeight (); }

int CScene::getCanvasWidth () const { return this->m_camera->getCanvasWidth (); }

int CScene::getCanvasHeight () const { return this->m_camera->getCanvasHeight (); }

float CScene::getTime () const { return g_Time; }

float CScene::getDeltaTime () const { return g_Time - g_TimeLast; }

float CScene::getSceneClock () const {
    // wallpaper64.exe sub_14017FA70 starts over at 0 once it passes 432000 (the frame crossing it is dropped there)
    return std::fmod (std::max (g_Time - this->m_startTime, 0.0f), 432000.0f);
}

uint32_t CScene::getFrameCounter () const { return this->m_frameCounter; }

float CScene::getFps () const {
    const float dt = g_Time - g_TimeLast;
    // avoids a division by zero / bogus fps on the first frame, where g_TimeLast is still 0
    if (dt <= 1e-6f) {
	return 60.0f;
    }
    return 1.0f / dt;
}

const glm::vec2* CScene::getPointerPosition () const { return &this->m_pointerPosition; }

const glm::vec2* CScene::getPointerPositionLast () const { return &this->m_pointerPositionLast; }

const glm::vec2* CScene::getMousePositionNormalized () const { return &this->m_mousePositionNormalized; }

const glm::vec2* CScene::getCursorScreenPosition () const { return &this->m_cursorScreen; }

glm::vec3 CScene::unprojectCursor () const {
    // WE unprojects the cursor (x, 1 - y) through the view and projection on top of the renderer's stacks, with glm's
    // unProject (depth 0.5 -> NDC 0) for scripts (sub_14018E070 -> sub_14019DF90) and NDC depth 0 for particle control
    // points (sub_14022E3E0). Its projections are Direct3D ones, so NDC depth 0 is the near plane
    const glm::vec2 ndc { this->m_cursorScreen.x * 2.0f - 1.0f, (1.0f - this->m_cursorScreen.y) * 2.0f - 1.0f };
    const auto& camera = this->getCamera ();

    if (camera.isPerspective ()) {
	const glm::vec4 world
	    = glm::inverse (camera.getPerspective () * camera.getView ()) * glm::vec4 (ndc, -1.0f, 1.0f);

	return glm::vec3 (world) / world.w;
    }

    return glm::vec3 (glm::inverse (camera.getProjection () * camera.getLookAt ()) * glm::vec4 (ndc, 0.0f, 1.0f));
}

glm::vec3 CScene::getCursorWorldPosition () const {
    const glm::vec3 position = this->unprojectCursor ();

    if (this->getCamera ().isPerspective ()) {
	return position;
    }

    // the 2D layout is WE's world moved to the scene center and flipped vertically (Camera::setWorldView); WE zeroes z
    // in orthographic scenes (renderer flag 0x400)
    return { position.x + static_cast<float> (this->getWidth ()) / 2.0f,
	     static_cast<float> (this->getHeight ()) / 2.0f - position.y, 0.0f };
}

glm::vec2 CScene::getCursorPixelPosition () const {
    // sub_14018E070 with its screen flag: g_PointerPosition times the window client size, x mirrored on renderer flag
    // 0x800 (horizontal flip). m_mousePosition is already mirrored
    const auto& state = this->getState ();

    return { this->m_mousePosition.x * static_cast<float> (state.getViewportWidth ()),
	     this->m_mousePosition.y * static_cast<float> (state.getViewportHeight ()) };
}

float CScene::getOutputAspect () const {
    const auto& state = this->getState ();

    if (state.getViewportWidth () <= 0 || state.getViewportHeight () <= 0) {
	return static_cast<float> (this->getCanvasWidth ()) / static_cast<float> (this->getCanvasHeight ());
    }

    return static_cast<float> (state.getViewportWidth ()) / static_cast<float> (state.getViewportHeight ());
}

const glm::vec2* CScene::getParallaxPosition () const { return &this->m_parallaxPosition; }

glm::vec2 CScene::getParallaxOffset (const Object& object) const {
    if (!this->getScene ().camera.parallax.enabled->value->getBool ()) {
	return { 0.0f, 0.0f };
    }

    // every object has a parallaxDepth, (1, 1) unless set (base object constructor sub_14016BE90)
    const auto depthOf = [] (const Object& candidate) -> glm::vec2 {
	if (candidate.is<Image> ()) {
	    return candidate.as<Image> ()->parallaxDepth->value->getVec2 ();
	}
	if (candidate.is<Text> ()) {
	    return candidate.as<Text> ()->parallaxDepth->value->getVec2 ();
	}
	if (candidate.is<Particle> ()) {
	    return candidate.as<Particle> ()->parallaxDepth->value->getVec2 ();
	}
	return candidate.groupParallaxDepth->value->getVec2 ();
    };

    // the topmost ancestor moves its whole subtree, guarded against a malformed parent loop
    constexpr int maxParentDepth = 32;
    const Object* anchor = &object;
    for (int i = 0; i < maxParentDepth && anchor->parent.has_value (); ++i) {
	const CObject* parent = this->getObject (anchor->parent.value ());
	if (parent == nullptr) {
	    break;
	}
	anchor = &parent->getObject ();
    }

    const std::optional<glm::vec2> depth = depthOf (*anchor);

    const float amount = this->getScene ().camera.parallax.amount->value->getFloat ();
    const float width = static_cast<float> (this->getWidth ());
    const float height = static_cast<float> (this->getHeight ());
    glm::vec2 shift = (*depth + amount) * this->m_parallaxBias * width;

    if (!this->getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	// real engine: (origin - cameraPosition) * amount * depth in its y up world (sub_14018AAC0), the camera
	// sitting at the mouse-driven point of the scene. This space is y down, hence the negated y
	const glm::vec3 origin = anchor->origin->value->getVec3 ();
	shift.x += (origin.x - width * 0.5f - this->m_cameraParallax.x * width) * amount * depth->x;
	shift.y -= (origin.y - height * 0.5f - this->m_cameraParallax.y * height) * amount * depth->y;
    }

    return shift;
}

const std::vector<CObject*>& CScene::getObjectsByRenderOrder () const { return this->m_objectsByRenderOrder; }

int CScene::nextFreeLightSlot () const {
    int used = 0;

    for (const auto* object : this->m_objects | std::views::values) {
	if (object->is<Objects::CLight> ()) {
	    used |= 1 << object->as<Objects::CLight> ()->getSlot ();
	}
    }

    for (int slot = 0; slot < 4; slot++) {
	if ((used & (1 << slot)) == 0) {
	    return slot;
	}
    }

    return 0;
}

void CScene::updateLights () {
    // an empty slot is black with radius 1, WE parks it at (0, 100, 0)
    glm::vec4 colors[4];
    glm::vec3 positions[4];

    std::ranges::fill (colors, glm::vec4 (0.0f, 0.0f, 0.0f, 1.0f));
    std::ranges::fill (positions, glm::vec3 (0.0f, 100.0f, 0.0f));

    for (const auto* object : this->m_objects | std::views::values) {
	if (!object->is<Objects::CLight> ()) {
	    continue;
	}

	const auto* light = object->as<Objects::CLight> ();
	const auto& data = light->getLight ();

	if (data.type != LightType::Legacy) {
	    continue;
	}

	const auto override = this->getContext ().getApp ().getContext ().resolveObjectVisibility (data.id, data.name);
	const bool visible = override.has_value () ? override.value () : data.visible->value->getBool ();

	if (!visible || this->isHiddenByAncestor (*light)) {
	    continue;
	}

	const int slot = light->getSlot ();
	const float intensity = data.intensity->value->getFloat ();

	colors[slot]
	    = glm::vec4 (glm::vec3 (data.color->value->getVec3 ()) * intensity, data.radius->value->getFloat ());
	positions[slot] = data.origin->value->getVec3 ();
    }

    std::ranges::copy (colors, this->m_lightsColorRadius);

    // radiance is color / distance^2 in the shader, so the color is scaled by radius^2; the fourth light's
    // color rides in the .w of the other three
    for (int i = 0; i < 3; i++) {
	this->m_lightsColorPremultiplied[i] = glm::vec4 (glm::vec3 (colors[i]) * colors[i].w * colors[i].w, 0.0f);
	this->m_lightsColorPremultiplied[i].w = colors[3][i] * colors[3].w * colors[3].w;
    }

    std::ranges::copy (positions, this->m_lightsPosition);
}

std::vector<const Light*> CScene::sortedLightingV1Lights () const {
    // sub_140186990 orders the visible lights by type, then shadow/cookie flags (set ones first), then by their own
    // origin along the camera's forward axis
    const auto& camera = this->getCamera ();
    const glm::vec3 forward = camera.isOrthogonal ()
	? glm::vec3 (0.0f, 0.0f, -1.0f)
	: -glm::vec3 (camera.getView ()[0][2], camera.getView ()[1][2], camera.getView ()[2][2]);
    struct Entry {
	int type;
	int flags;
	float depth;
	const Light* light;
    };
    std::vector<Entry> entries;

    for (const auto* object : this->m_objects | std::views::values) {
	if (!object->is<Objects::CLight> ()) {
	    continue;
	}

	const auto& data = object->as<Objects::CLight> ()->getLight ();

	if (data.type == LightType::Legacy) {
	    continue;
	}

	const auto override = this->getContext ().getApp ().getContext ().resolveObjectVisibility (data.id, data.name);
	const bool visible = override.has_value () ? override.value () : data.visible->value->getBool ();

	if (!visible || this->isHiddenByAncestor (*object)) {
	    continue;
	}

	entries.push_back (
	    { static_cast<int> (data.type), (data.castShadow ? 1 : 0) | (data.useCookie ? 2 : 0),
	      glm::dot (data.origin->value->getVec3 (), forward), &data }
	);
    }

    std::ranges::stable_sort (entries, [] (const Entry& a, const Entry& b) {
	if (a.type != b.type) {
	    return a.type < b.type;
	}
	if (a.flags != b.flags) {
	    return a.flags > b.flags;
	}
	return a.depth < b.depth;
    });

    std::vector<const Light*> lights;

    for (const auto& entry : entries) {
	lights.push_back (entry.light);
    }

    return lights;
}

std::shared_ptr<const TextureProvider> CScene::getLightCookie () const {
    // one cookie texture for every lit material: the last cookie spot the uniform packing takes (sub_140190C80),
    // its "cookie" or cookie/flashlight1 (sub_14025D080). Passes are built while the scene's objects are still being
    // created, so this goes by the scene data: visible spots in the packing's order (type is equal, cookie spots
    // sort before the others, then by depth)
    const auto& camera = this->getCamera ();
    const glm::vec3 forward = camera.isOrthogonal ()
	? glm::vec3 (0.0f, 0.0f, -1.0f)
	: -glm::vec3 (camera.getView ()[0][2], camera.getView ()[1][2], camera.getView ()[2][2]);
    std::vector<const Light*> spots;

    for (const auto& object : this->getScene ().objects) {
	if (!object->is<Light> ()) {
	    continue;
	}

	const auto* light = object->as<Light> ();
	const auto override
	    = this->getContext ().getApp ().getContext ().resolveObjectVisibility (light->id, light->name);

	if (light->type == LightType::Spot
	    && (override.has_value () ? override.value () : light->visible->value->getBool ())) {
	    spots.push_back (light);
	}
    }

    std::ranges::stable_sort (spots, [&forward] (const Light* a, const Light* b) {
	const int flagsA = (a->castShadow ? 1 : 0) | (a->useCookie ? 2 : 0);
	const int flagsB = (b->castShadow ? 1 : 0) | (b->useCookie ? 2 : 0);

	if (flagsA != flagsB) {
	    return flagsA > flagsB;
	}
	return glm::dot (a->origin->value->getVec3 (), forward) < glm::dot (b->origin->value->getVec3 (), forward);
    });

    const Light* last = nullptr;

    for (int i = 0; i < static_cast<int> (spots.size ()) && i < this->m_lightingV1.spots; i++) {
	if (spots[i]->useCookie) {
	    last = spots[i];
	}
    }

    // without a cookie spot nothing samples it
    return this->getContext ().resolveTexture (
	last == nullptr || last->cookie.empty () ? "cookie/flashlight1" : last->cookie, this->getScene ().project
    );
}

namespace {
// point light cube faces as assets/shaders/common_pbr_2.h picks them (CalculateProjectedCoordsPoint), the same
// matrices sub_140190C80 renders the six viewports with: +x, -x, +y, -y, +z, -z
glm::mat4 pointFaceView (const int face, const glm::vec3& o) {
    switch (face) {
	case 0:
	    return { { 0, 0, -1, 0 }, { 0, 1, 0, 0 }, { 1, 0, 0, 0 }, { -o.z, -o.y, o.x, 1 } };
	case 1:
	    return { { 0, 0, 1, 0 }, { 0, 1, 0, 0 }, { -1, 0, 0, 0 }, { o.z, -o.y, -o.x, 1 } };
	case 2:
	    return { { 1, 0, 0, 0 }, { 0, 0, -1, 0 }, { 0, 1, 0, 0 }, { -o.x, -o.z, o.y, 1 } };
	case 3:
	    return { { 1, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, -1, 0, 0 }, { -o.x, o.z, -o.y, 1 } };
	case 4:
	    return { { -1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, -1, 0 }, { o.x, -o.y, o.z, 1 } };
	default:
	    return { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { -o.x, -o.y, -o.z, 1 } };
    }
}

// WE's device projections have 1 at the near plane (sub_14009A360); Volumetrics::spotViewProjection keeps the usual
// 0 there, this turns one into the other (depth' = 1 - depth)
glm::mat4 reverseDepth (const glm::mat4& projection) {
    glm::mat4 reverse (1.0f);
    reverse[2][2] = -1.0f;
    reverse[3][2] = 1.0f;
    return reverse * projection;
}
} // namespace

void CScene::updateLightingV1 () {
    auto& lighting = this->m_lightingV1;
    const LightingV1 counts = lighting;

    lighting = LightingV1 {};
    lighting.points = counts.points;
    lighting.spots = counts.spots;
    lighting.tubes = counts.tubes;
    lighting.directionals = counts.directionals;
    lighting.spotCookies = counts.spotCookies;
    lighting.spotShadowCookies = counts.spotShadowCookies;
    lighting.spotShadows = counts.spotShadows;
    lighting.directionalShadows = counts.directionalShadows;
    lighting.pointShadows = counts.pointShadows;
    this->m_shadowEntries.clear ();
    this->m_volumeShadows.clear ();

    if (lighting.points + lighting.spots + lighting.tubes + lighting.directionals == 0) {
	return;
    }

    const auto& camera = this->getCamera ();
    const bool orthographic = camera.isOrthogonal ();
    const std::vector<const Light*> lights = this->sortedLightingV1Lights ();
    const bool shadows = this->m_shadowQuality > 0;
    const int tile = ShadowMapping::tileSize (this->m_shadowQuality);
    // renderer +104 (eye) and +352 (camera forward) as sub_14017FA70 leaves them
    const glm::vec3 eye = this->m_fog.eyeWorld;
    const glm::vec3 forward = orthographic
	? glm::vec3 (0.0f, 0.0f, -1.0f)
	: -glm::vec3 (camera.getView ()[0][2], camera.getView ()[1][2], camera.getView ()[2][2]);
    const int ssc = lighting.spotShadowCookies;
    const int sc = lighting.spotCookies;
    const int ss = lighting.spotShadows;

    // where each class of a light type writes next (sub_140190C80 keeps a pointer per class): points with a
    // shadow first, spots shadow+cookie / cookie / shadow / plain indexed by castshadow | usecookie << 1,
    // directional lights with a shadow first. The ShadowProjection matrices go by the spot classes too and the
    // cascades continue after them. Every light with a shadow or cookie takes one of the features, a directional
    // light one for its three cascades
    int pointsLeft = lighting.points;
    int pointSlot[2] = { lighting.pointShadows, 0 };
    int pointShadowsLeft = lighting.pointShadows;
    int pointShadow = 0;
    int spotsLeft = lighting.spots;
    int spotSlot[4] = { ssc + sc + ss, ssc + sc, ssc, 0 };
    int featureSlot[4] = { ssc + sc + ss, ssc + sc, ssc, 0 };
    int featuresLeft = lighting.features ();
    int tube = 0;
    int directionalsLeft = lighting.directionals;
    int directionalSlot[2] = { lighting.directionalShadows, 0 };
    const int featureCapacity = static_cast<int> (std::size (lighting.featureProjection));

    for (const Light* entry : lights) {
	const Light& light = *entry;
	const glm::mat4 world = this->objectWorldMatrix (light);
	const glm::vec3 color = light.color->value->getVec3 () * light.intensity->value->getFloat ();
	const float radius = light.radius->value->getFloat ();
	const float exponent = light.exponent->value->getFloat ();
	const glm::vec3 origin (world[3]);
	const bool shadowed = shadows && light.castShadow;

	switch (light.type) {
	    case LightType::Point:
		{
		    if (pointsLeft == 0) {
			break;
		    }

		    pointsLeft--;

		    if (const int slot = pointSlot[shadowed ? 1 : 0]++; slot < lighting.points) {
			lighting.pointColor[slot] = glm::vec4 (color, radius);
			lighting.pointOrigin[slot] = glm::vec4 (origin, exponent);
		    }

		    if (!shadowed || pointShadowsLeft == 0) {
			break;
		    }

		    // sub_14025D420: a fov a bit over 90 degrees per quality, aspect 1, near the light source size (1
		    // in orthographic scenes), far the radius
		    pointShadowsLeft--;

		    const int quality = this->m_shadowQuality;
		    const float fov = quality <= 2 ? 94.0f : quality == 3 ? 92.0f : 91.2f;
		    const float near
			= orthographic ? 1.0f : std::max (0.05f, light.lightSourceSize->value->getFloat ());
		    const float far = std::max (radius, near + 0.01f);
		    const glm::mat4 projection = ShadowMapping::perspective (fov * 0.017453292f, 1.0f, near, far);
		    ShadowMapping::Entry shadow { .point = true, .size = tile };

		    lighting.pointShadowProjection[pointShadow]
			= glm::vec4 (projection[2][2], projection[3][2], projection[2][3], projection[3][3]);
		    shadow.transform = &lighting.pointShadowProjectionTransform[pointShadow];
		    pointShadow++;

		    for (int face = 0; face < 6; face++) {
			shadow.viewProjection[face] = projection * pointFaceView (face, origin);
		    }

		    this->m_volumeShadows[&light]
			= { .atlasTransform = shadow.transform,
			    .pointProjection = lighting.pointShadowProjection[pointShadow - 1] };
		    this->m_shadowEntries.push_back (shadow);
		    break;
		}
	    case LightType::Spot:
		{
		    if (spotsLeft == 0) {
			break;
		    }

		    spotsLeft--;

		    const int type = (light.castShadow ? 1 : 0) | (light.useCookie ? 2 : 0);
		    const int kind = type & (shadows ? 3 : 2);

		    // cosines of the cone angles in the .w, the direction is the world matrix's x column as it is
		    if (const int slot = spotSlot[kind]++; slot < lighting.spots) {
			lighting.spotColor[slot] = glm::vec4 (color, radius);
			lighting.spotOrigin[slot]
			    = glm::vec4 (origin, std::cos (light.innerCone->value->getFloat () * 0.017453292f));
			lighting.spotDirection[slot] = glm::vec4 (
			    glm::vec3 (world[0]), std::cos (light.outerCone->value->getFloat () * 0.017453292f)
			);
			lighting.spotExponent[slot] = glm::vec4 (exponent, 0.0f, 0.0f, 0.0f);
		    }

		    if ((!shadowed && !light.useCookie) || featuresLeft == 0) {
			break;
		    }

		    featuresLeft--;

		    const int feature = featureSlot[kind]++;

		    if (feature >= featureCapacity) {
			break;
		    }

		    lighting.featureProjection[feature]
			= reverseDepth (Volumetrics::spotViewProjection (light, world, orthographic));

		    if (shadowed) {
			ShadowMapping::Entry shadow { .size = tile,
						      .transform = &lighting.featureProjectionTransform[feature] };
			shadow.viewProjection[0] = lighting.featureProjection[feature];
			this->m_volumeShadows[&light]
			    = { .matrix = lighting.featureProjection[feature], .atlasTransform = shadow.transform };
			this->m_shadowEntries.push_back (shadow);
		    }
		    break;
		}
	    case LightType::Tube:
		if (tube < lighting.tubes) {
		    lighting.tubeColor[tube] = glm::vec4 (color, radius);
		    lighting.tubeOriginA[tube] = glm::vec4 (origin, exponent);
		    lighting.tubeOriginB[tube]
			= glm::vec4 (glm::vec3 (world * glm::vec4 (light.controlPoint->value->getVec3 (), 1.0f)), 0.0f);
		    tube++;
		}
		break;
	    case LightType::Directional:
		{
		    if (directionalsLeft == 0) {
			break;
		    }

		    directionalsLeft--;

		    if (const int slot = directionalSlot[shadowed ? 1 : 0]++; slot < lighting.directionals) {
			lighting.directionalColor[slot] = glm::vec4 (color, 1.0f);
			lighting.directionalDirection[slot] = glm::vec4 (-glm::vec3 (world[0]), 0.0f);
		    }

		    if (!shadowed || featuresLeft == 0) {
			break;
		    }

		    featuresLeft--;

		    // sub_14025D370: each cascade covers a square of its distance and a depth range around the view,
		    // sub_140190C80 0x14019128b: centered half the distance ahead of the eye (the forward's part along
		    // the light only half counted, z 0 in 2D), snapped to shadow texels along the light's y and z axes.
		    // The view looks down the light's x axis (rows z, y, -x like a spot), the projection is an
		    // orthographic box (sub_14009A630) of +-distance/2 and depth +-range/2
		    const float cascade0 = light.cascadeDistance[0]->value->getFloat ();
		    const float cascade1 = light.cascadeDistance[1]->value->getFloat ();
		    const float cascade2 = light.cascadeDistance[2]->value->getFloat ();
		    const glm::vec2 ranges[3] = {
			{ cascade0, cascade1 * 4.0f },
			{ cascade1, cascade1 * 4.0f },
			{ cascade2, std::max (cascade2 * 1.5f, cascade1 * 4.0f) },
		    };
		    const glm::vec3 direction = glm::normalize (glm::vec3 (world[0]));
		    const glm::vec3 y (world[1]);
		    const glm::vec3 z (world[2]);
		    const glm::vec3 rows[3]
			= { Volumetrics::approximateNormalize (z), Volumetrics::approximateNormalize (y),
			    Volumetrics::approximateNormalize (-glm::vec3 (world[0])) };
		    const glm::vec3 ahead = forward - direction * (glm::dot (forward, direction) * 0.5f);

		    for (const glm::vec2& range : ranges) {
			const float half = range.x * 0.5f;
			glm::vec3 center = eye + ahead * half;

			if (orthographic) {
			    center.z = 0.0f;
			}

			const float texel = range.x / static_cast<float> (tile);
			const float alongZ = glm::dot (z, center);
			const float alongY = glm::dot (y, center);

			center -= y * std::fmod (alongY, texel);
			center -= z * std::fmod (alongZ, texel);

			glm::mat4 view (1.0f);

			for (int row = 0; row < 3; row++) {
			    for (int column = 0; column < 3; column++) {
				view[column][row] = rows[row][column];
			    }

			    view[3][row] = -glm::dot (rows[row], center);
			}

			glm::mat4 projection (1.0f);
			projection[0][0] = 1.0f / half;
			projection[1][1] = 1.0f / half;
			projection[2][2] = 1.0f / range.y;
			projection[3][2] = 0.5f;

			const int feature = featureSlot[0]++;

			if (feature >= featureCapacity) {
			    continue;
			}

			lighting.featureProjection[feature] = projection * view;

			ShadowMapping::Entry shadow { .size = tile,
						      .transform = &lighting.featureProjectionTransform[feature] };
			shadow.viewProjection[0] = lighting.featureProjection[feature];
			this->m_shadowEntries.push_back (shadow);
		    }
		    break;
		}
	    default:
		break;
	}
    }
}

const CScene::LayerTarget* CScene::getLayerTarget () const {
    return this->m_layerTargets.empty () ? nullptr : &this->m_layerTargets.back ();
}

namespace {
bool isPassthroughLayer (const CObject& object) {
    return object.is<Objects::CImage> () && object.as<Objects::CImage> ()->getImage ().model->passthrough;
}
} // namespace

bool CScene::isDrawnByPassthroughLayer (const CObject& object) const {
    const Object* current = &object.getObject ();

    // set when the parent has flag 2 or 4 (sub_1401DD9A0), so any passthrough ancestor
    for (int depth = 0; current->parent.has_value () && depth < 64; depth++) {
	const auto* parent = this->getObject (current->parent.value ());

	if (parent == nullptr) {
	    return false;
	}

	if (isPassthroughLayer (*parent)) {
	    return true;
	}

	current = &parent->getObject ();
    }

    return false;
}

std::vector<CObject*> CScene::childrenOf (int id) const {
    std::vector<CObject*> children;

    for (auto* object : this->m_objectsByRenderOrder) {
	if (object->getObject ().parent == id) {
	    children.push_back (object);
	}
    }

    return children;
}

void CScene::renderPassthroughChildren (int layerId, const LayerTarget& target) {
    this->m_layerTargets.push_back (target);

    // direct children only when visible (slot 13), their subtrees unless they are passthrough layers themselves,
    // which draw their own (sub_1401ECB20)
    for (auto* child : this->childrenOf (layerId)) {
	this->renderPassthroughChild (child);

	if (!isPassthroughLayer (*child)) {
	    this->renderPassthroughSubtree (child->getId (), 1);
	}
    }

    this->m_layerTargets.pop_back ();
}

void CScene::renderPassthroughSubtree (int parentId, int depth) {
    if (depth >= 64) {
	return;
    }

    for (auto* child : this->childrenOf (parentId)) {
	this->renderPassthroughChild (child);

	if (!isPassthroughLayer (*child)) {
	    this->renderPassthroughSubtree (child->getId (), depth + 1);
	}
    }
}

void CScene::renderPassthroughChild (CObject* object) {
    const auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
    if (debug.objectFilter.has_value () && object->getId () != debug.objectFilter.value ()) {
	return;
    }
    if (std::ranges::find (debug.skipObjects, object->getId ()) != debug.skipObjects.end ()) {
	return;
    }

    const auto visibility = this->getContext ().getApp ().getContext ().resolveObjectVisibility (
	object->getId (), object->getObject ().name
    );
    if ((visibility.has_value () && !visibility.value ()) || this->isHiddenByAncestor (*object)) {
	return;
    }

    // lights go into the shared light buffer (sub_140196CE0 called from sub_1401ECB20), the main loop adds it onto
    // the scene before the next top level object that isn't a light. sub_1401ECB20 swaps the matrices without
    // marking the renderer dirty, so a light that comes before any other drawn child still sees the scene's view
    // projection (checked against live WE)
    if (object->is<Objects::CLight> ()) {
	const auto& target = this->m_layerTargets.back ();

	this->renderLightVolume (
	    *object->as<Objects::CLight> (),
	    target.viewProjectionApplied ? target.viewProjection : this->getWorldViewProjection ()
	);
	return;
    }

    timeStep ("render " + object->getObject ().name, [&] { object->render (); });
    // nested layers push and pop their own targets while rendering, look it up again
    this->m_layerTargets.back ().viewProjectionApplied = true;
}

void CScene::renderLightVolume (const Objects::CLight& object, const glm::mat4& viewProjection) {
    const auto& light = object.getLight ();

    // WE sends every light through the same code, but only point and spot lights get a volume matrix
    // (sub_14025D420). Tubes and directional lights would draw the cone through a matrix nothing sets, left out
    if ((light.type != LightType::Point && light.type != LightType::Spot) || !light.castVolumetrics
	|| !light.visible->value->getBool ()) {
	return;
    }

    timeStep ("volumetrics " + light.name, [&] {
	const auto shadow = this->m_volumeShadows.find (&light);

	this->m_volumetrics->renderLight (
	    light, this->objectWorldMatrix (light), viewProjection,
	    shadow != this->m_volumeShadows.end () ? &shadow->second : nullptr
	);
    });
}

bool CScene::isTransparentSorted (const CObject& object) {
    const auto cached = this->m_transparentSorted.find (&object);

    if (cached != this->m_transparentSorted.end ()) {
	return cached->second;
    }

    bool sorted = false;

    // particles, lights and text always, images by their material's blending or as passthrough layers without
    // copybackground (sub_1401FAC50), models never. WE's bloom is no scene object
    if (object.is<Objects::CParticle> () || object.is<Objects::CLight> () || object.is<Objects::CText> ()) {
	sorted = true;
    } else if (object.is<Objects::CImage> () && &object != this->m_bloomObject) {
	const auto& image = object.as<Objects::CImage> ()->getImage ();
	const auto& passes = image.model->material->passes;
	const BlendingMode blending = passes.empty () ? BlendingMode_Normal : passes.front ()->blending;

	sorted = blending == BlendingMode_Translucent || blending == BlendingMode_Additive
	    || (image.model->passthrough && !image.copyBackground->value->getBool ());
    }

    this->m_transparentSorted.emplace (&object, sorted);
    return sorted;
}

std::vector<CObject*> CScene::sortedByDepth (std::vector<CObject*> objects) const {
    // key = origin . view direction, the renderer keeps -row 2 of the view matrix for it (sub_14017FA70); fullscreen
    // and project layers (object flag 0x200) get -inf. Stable, largest key first
    const glm::mat4& view = this->getCamera ().getView ();
    const glm::vec3 forward = -glm::vec3 (view[0][2], view[1][2], view[2][2]);
    std::vector<std::pair<float, CObject*>> keyed;

    keyed.reserve (objects.size ());

    for (auto* object : objects) {
	float key = glm::dot (object->getObject ().origin->value->getVec3 (), forward);

	if (object->is<Objects::CImage> ()) {
	    const auto& model = *object->as<Objects::CImage> ()->getImage ().model;

	    if (model.fullscreen || model.projectlayer) {
		key = -std::numeric_limits<float>::infinity ();
	    }
	}

	keyed.emplace_back (key, object);
    }

    std::ranges::stable_sort (keyed, [] (const auto& a, const auto& b) { return a.first > b.first; });

    for (size_t i = 0; i < keyed.size (); i++) {
	objects[i] = keyed[i].second;
    }

    return objects;
}

bool CScene::quadContainsPoint (const glm::mat4& mvp, const glm::vec2& half, const glm::vec2& ndc) {
    const glm::vec2 corners[] = { { -half.x, -half.y }, { half.x, -half.y }, { half.x, half.y }, { -half.x, half.y } };
    glm::vec2 projected[4];

    for (int i = 0; i < 4; i++) {
	const glm::vec4 clip = mvp * glm::vec4 (corners[i], 0.0f, 1.0f);

	if (clip.w <= 0.0f) {
	    return false;
	}

	projected[i] = glm::vec2 (clip) / clip.w;
    }

    bool positive = false;
    bool negative = false;

    for (int i = 0; i < 4; i++) {
	const glm::vec2 edge = projected[(i + 1) % 4] - projected[i];
	const glm::vec2 toPoint = ndc - projected[i];
	const float cross = edge.x * toPoint.y - edge.y * toPoint.x;

	positive |= cross > 0.0f;
	negative |= cross < 0.0f;
    }

    // a quad seen edge on has no area and nothing to hit
    const glm::vec2 diagonalA = projected[2] - projected[0];
    const glm::vec2 diagonalB = projected[3] - projected[1];

    return !(positive && negative) && (diagonalA.x * diagonalB.y - diagonalA.y * diagonalB.x) != 0.0f;
}

std::optional<glm::vec2> CScene::quadPlanePoint (const glm::mat4& mvp, const glm::vec2& ndc) {
    // clip = x * col0 + y * col1 + col3, and the point lands on ndc when clip.xy = ndc * clip.w
    const glm::vec4& ax = mvp[0];
    const glm::vec4& ay = mvp[1];
    const glm::vec4& at = mvp[3];
    const float a = ax.x - ndc.x * ax.w;
    const float b = ay.x - ndc.x * ay.w;
    const float c = ax.y - ndc.y * ax.w;
    const float d = ay.y - ndc.y * ay.w;
    const float det = a * d - b * c;

    if (std::abs (det) <= 1.1920929e-7f) {
	return std::nullopt;
    }

    const float ex = ndc.x * at.w - at.x;
    const float ey = ndc.y * at.w - at.y;
    const glm::vec2 point = { (ex * d - b * ey) / det, (a * ey - ex * c) / det };

    if (point.x * ax.w + point.y * ay.w + at.w <= 0.0f) {
	return std::nullopt;
    }

    return point;
}

bool CScene::isHiddenByAncestor (const CObject& object) const {
    const auto& appContext = this->getContext ().getApp ().getContext ();
    const Object* current = &object.getObject ();

    // objects nest through "parent"; a child is only shown while every group above it is, whatever
    // its own visible property says (the guard keeps a malformed parent loop from spinning)
    for (int depth = 0; current->parent.has_value () && depth < 64; depth++) {
	const auto* parent = this->getObject (current->parent.value ());

	if (parent == nullptr) {
	    return false;
	}

	const Object& data = parent->getObject ();
	const auto override = appContext.resolveObjectVisibility (parent->getId (), data.name);
	bool visible;

	if (override.has_value ()) {
	    visible = override.value ();
	} else if (data.is<Image> ()) {
	    visible = data.as<Image> ()->visible->value->getBool ();
	} else if (data.is<Text> ()) {
	    visible = data.as<Text> ()->visible->value->getBool ();
	} else if (data.is<Particle> ()) {
	    visible = data.as<Particle> ()->visible->value->getBool ();
	} else {
	    visible = data.groupVisible->value->getBool ();
	}

	if (!visible) {
	    return true;
	}

	current = &data;
    }

    return false;
}

const CObject* CScene::getObject (int id) const {
    const auto object = this->m_objects.find (id);
    return object == this->m_objects.end () ? nullptr : object->second;
}

CObject* CScene::getObject (int id) {
    const auto object = this->m_objects.find (id);
    return object == this->m_objects.end () ? nullptr : object->second;
}

void CScene::setSoundPlaying (int id, bool playing) {
    this->m_soundPlayRequests[id] = playing;

    const auto object = this->m_objects.find (id);

    if (object == this->m_objects.end () || !object->second->is<Objects::CSound> ()) {
	return;
    }

    auto* sound = object->second->as<Objects::CSound> ();

    if (playing) {
	sound->play ();
    } else {
	sound->stop ();
    }
}

void CScene::pauseSound (int id) {
    if (auto* sound = this->getObject (id); sound != nullptr && sound->is<Objects::CSound> ()) {
	sound->as<Objects::CSound> ()->pause ();
    }
}

bool CScene::isSoundPlaying (int id, bool startSilent) const {
    if (const auto* sound = this->getObject (id); sound != nullptr && sound->is<Objects::CSound> ()) {
	return sound->as<Objects::CSound> ()->isPlaying ();
    }

    return this->getSoundPlayRequest (id).value_or (!startSilent);
}

std::optional<bool> CScene::getSoundPlayRequest (int id) const {
    const auto request = this->m_soundPlayRequests.find (id);

    return request == this->m_soundPlayRequests.end () ? std::nullopt : std::optional<bool> (request->second);
}

std::vector<CObject*> CScene::getLayers () const {
    std::vector<CObject*> layers;

    for (auto* object : this->m_objectsByRenderOrder) {
	if (object != this->m_bloomObject) {
	    layers.push_back (object);
	}
    }

    return layers;
}

int CScene::getObjectIndex (const CObject* object) const {
    const auto layers = this->getLayers ();
    const auto it = std::ranges::find (layers, object);

    if (it == layers.end ()) {
	return -1;
    }

    return static_cast<int> (std::distance (layers.begin (), it));
}

// bloom post-processes everything drawn before it, so it has to stay last
void CScene::appendLayer (CObject* object) {
    this->m_objectsByRenderOrder.insert (std::ranges::find (this->m_objectsByRenderOrder, this->m_bloomObject), object);
}

Render::CObject* CScene::buildLayer (JSON layerJson) {
    const int id = this->m_nextDynamicLayerId++;

    layerJson["id"] = id;
    if (!layerJson.contains ("name") || !layerJson["name"].is_string ()) {
	layerJson["name"] = "scriptlayer_" + std::to_string (id);
    }
    if (!layerJson.contains ("visible")) {
	layerJson["visible"] = true;
    }

    auto objectData = ObjectParser::parse (layerJson, this->getScene ().project);
    Render::CObject* renderObject = this->createObject (*objectData);

    if (renderObject == nullptr) {
	return nullptr;
    }

    this->m_dynamicObjectData.emplace_back (std::move (objectData));
    this->appendLayer (renderObject);

    return renderObject;
}

// scripts write into createLayer()'s result without a null check, so failures get a scriptable, non-rendering
// placeholder
Render::CObject* CScene::createPlaceholderLayer () {
    try {
	const int id = this->m_nextDynamicLayerId++;
	const JSON placeholderJson
	    = { { "id", id }, { "name", "scriptlayer_" + std::to_string (id) }, { "visible", true } };
	auto objectData = ObjectParser::parse (placeholderJson, this->getScene ().project);
	auto* renderObject = new Scripting::ScriptableObject (*this, *objectData);

	this->m_objects.emplace (renderObject->getId (), renderObject);
	this->m_dynamicObjectData.emplace_back (std::move (objectData));
	this->appendLayer (renderObject);

	return renderObject;
    } catch (const std::exception& placeholderError) {
	sLog.error ("createLayer placeholder fallback also failed: ", placeholderError.what ());
	return nullptr;
    }
}

Render::CObject* CScene::createLayer (const std::string& imagePath) {
    // createObject() throws on a missing asset, and unwinding a C++ exception through the V8 callback is undefined
    // behavior
    try {
	if (const auto cached = this->m_createLayerAliases.find (imagePath);
	    cached != this->m_createLayerAliases.end ()) {
	    return this->buildLayer ({ { "image", cached->second } });
	}

	return this->buildLayer ({ { "image", imagePath } });
    } catch (const std::exception& e) {
	// scripts written against a workshop dependency's original layout use the un-prefixed name, try the prefixed
	// copy first
	if (const auto alias = this->getScene ().project.assetLocator->resolveWorkshopDependencyAlias (imagePath);
	    alias.has_value () && !this->m_createLayerAliases.contains (imagePath)) {
	    try {
		sLog.out (
		    "createLayer: '", imagePath, "' not found, found and using workshop-dependency copy '",
		    alias->string (), "' instead"
		);
		auto* layer = this->buildLayer ({ { "image", alias->string () } });
		this->m_createLayerAliases.emplace (imagePath, alias->string ());
		return layer;
	    } catch (const std::exception& aliasError) {
		sLog.error (
		    "createLayer: workshop-dependency copy '", alias->string (), "' also failed: ", aliasError.what ()
		);
	    }
	} else {
	    sLog.error (
		"createLayer failed for '", imagePath, "', falling back to an invisible placeholder: ", e.what ()
	    );
	}

	return this->createPlaceholderLayer ();
    }
}

Render::CObject* CScene::createLayerFromConfig (JSON config) {
    // WE (scenescript64 sub_180011340): a config with "color" and no layer type key becomes a solid layer
    static constexpr std::array layerTypes
	= { "model", "particle", "sprite", "image", "text", "light", "sound", "camera" };

    if (config.contains ("color")
	&& std::ranges::none_of (layerTypes, [&config] (const char* key) { return config.contains (key); })) {
	config["image"] = "models/util/solidlayer.json";
    }

    try {
	return this->buildLayer (config);
    } catch (const std::exception& e) {
	sLog.error (
	    "createLayer failed for ", config.dump (), ", falling back to an invisible placeholder: ", e.what ()
	);
	return this->createPlaceholderLayer ();
    }
}

void CScene::sortLayer (CObject* object, int index) {
    const auto current = std::ranges::find (this->m_objectsByRenderOrder, object);

    if (current == this->m_objectsByRenderOrder.end ()) {
	return;
    }

    this->m_objectsByRenderOrder.erase (current);

    // index counts script-visible layers, so place it before whichever layer currently holds that slot
    const auto layers = this->getLayers ();
    const int clampedIndex = std::clamp (index, 0, static_cast<int> (layers.size ()));
    const auto before = clampedIndex < static_cast<int> (layers.size ())
	? std::ranges::find (this->m_objectsByRenderOrder, layers[clampedIndex])
	: std::ranges::find (this->m_objectsByRenderOrder, this->m_bloomObject);

    this->m_objectsByRenderOrder.insert (before, object);
}

void CScene::loadStaticCamera () {
    if (this->m_staticCameraLoaded) {
	return;
    }

    this->m_staticCameraLoaded = true;

    // 2D scenes without camera paths get the reset camera, the others scene.json's (sub_140186C90); zoom stays
    if (!this->m_camera->isPerspective () && this->getScene ().camera.paths.empty ()) {
	this->m_staticCamera.eye = glm::vec3 (0.0f);
	this->m_staticCamera.center = glm::vec3 (0.0f, 0.0f, -1.0f);
	this->m_staticCamera.up = glm::vec3 (0.0f, 1.0f, 0.0f);
    } else {
	this->m_staticCamera.eye = this->m_camera->getConfiguredEye ();
	this->m_staticCamera.center = this->m_camera->getCenter ();
	this->m_staticCamera.up = this->m_camera->getUp ();
    }
}

uint32_t CScene::createModelData (const ModelData::Config& config, ModelData::Error& error) {
    return this->m_modelData.create (
	config,
	[this] (const std::string& material, uint32_t format) {
	    return this->checkModelDataMaterial (material, format);
	},
	error
    );
}

void CScene::applyModelData (uint32_t token, const ModelData::Config& config, bool replace, ModelData::Error& error) {
    this->m_modelData.apply (
	token, config, replace,
	[this] (const std::string& material, uint32_t format) {
	    return this->checkModelDataMaterial (material, format);
	},
	error
    );
}

void CScene::destroyModelData (uint32_t token) { this->m_modelData.release (token); }

std::shared_ptr<ModelData::Model> CScene::acquireModelData (int token) { return this->m_modelData.acquire (token); }

void CScene::releaseModelData (uint32_t token) { this->m_modelData.release (token); }

ModelData::Error CScene::checkModelDataMaterial (const std::string& material, uint32_t format) {
    const auto key = std::make_pair (material, format);

    if (const auto cached = this->m_modelDataMaterialChecks.find (key);
	cached != this->m_modelDataMaterialChecks.end ()) {
	return cached->second;
    }

    // semantic of every vertex format bit and of every attribute name WE's shader translation knows (0x140482AF0,
    // 0x140484A90), sub_1400D7B60 only compares semantic names
    static const std::pair<uint32_t, const char*> formatSemantics[] = {
	{ 0x1, "POSITION" },      { 0x10000, "POSITION" },      { 0x2000000, "POSITION" },    { 0x2, "NORMAL" },
	{ 0x4, "TANGENT" },       { 0x800000, "BLENDINDICES" }, { 0x1000000, "BLENDWEIGHT" }, { 0x8, "TEXCOORD" },
	{ 0x10, "TEXCOORD" },     { 0x20, "TEXCOORD" },         { 0x40, "TEXCOORD" },         { 0x80, "TEXCOORD" },
	{ 0x100, "TEXCOORD" },    { 0x200, "TEXCOORD" },        { 0x400, "TEXCOORD" },        { 0x800, "TEXCOORD" },
	{ 0x1000, "TEXCOORD" },   { 0x2000, "TEXCOORD" },       { 0x4000, "TEXCOORD" },       { 0x20000, "TEXCOORD" },
	{ 0x40000, "TEXCOORD" },  { 0x80000, "TEXCOORD" },      { 0x100000, "TEXCOORD" },     { 0x200000, "TEXCOORD" },
	{ 0x400000, "TEXCOORD" }, { 0x8000, "COLOR" },
    };
    static const std::map<std::string, std::string> attributeSemantics = {
	{ "a_Position", "POSITION" },        { "a_PositionVec4", "POSITION" },
	{ "a_PositionC1", "POSITION" },      { "a_Normal", "NORMAL" },
	{ "a_Tangent4", "TANGENT" },         { "a_BlendIndices", "BLENDINDICES" },
	{ "a_BlendWeights", "BLENDWEIGHT" }, { "a_TexCoord", "TEXCOORD" },
	{ "a_TexCoordVec3", "TEXCOORD" },    { "a_TexCoordVec4", "TEXCOORD" },
	{ "a_TexCoordC1", "TEXCOORD" },      { "a_TexCoordVec3C1", "TEXCOORD" },
	{ "a_TexCoordVec4C1", "TEXCOORD" },  { "a_TexCoordC2", "TEXCOORD" },
	{ "a_TexCoordVec3C2", "TEXCOORD" },  { "a_TexCoordVec4C2", "TEXCOORD" },
	{ "a_TexCoordC3", "TEXCOORD" },      { "a_TexCoordVec3C3", "TEXCOORD" },
	{ "a_TexCoordVec4C3", "TEXCOORD" },  { "a_TexCoordC4", "TEXCOORD" },
	{ "a_TexCoordVec3C4", "TEXCOORD" },  { "a_TexCoordVec4C4", "TEXCOORD" },
	{ "a_TexCoordC5", "TEXCOORD" },      { "a_TexCoordVec3C5", "TEXCOORD" },
	{ "a_TexCoordVec4C5", "TEXCOORD" },  { "a_Color", "COLOR" },
    };
    static const std::regex input (R"(\bin\s+\w+\s+(a_\w+)\s*;)");

    ModelData::Error result = ModelData::Error::None;
    const auto& project = this->getScene ().project;
    const auto loaded = Data::Parsers::MaterialParser::load (project, material);

    if (loaded == nullptr || loaded->passes.empty ()) {
	result = ModelData::Error::InvalidMaterial;
    } else {
	// the program WE compiles for the material by itself (sub_1401A5C40): the pass's combos plus the scene wide
	// defines every pass gets
	const auto& pass = *loaded->passes.front ();
	ComboMap combos = pass.combos;
	const ComboMap overrideCombos;
	const TextureMap overrideTextures;
	static const ShaderConstantMap noConstants;

	if (this->getCamera ().isOrthogonal ()) {
	    combos.insert_or_assign ("SCENE_ORTHO", 1);
	}

	if (this->isHDR ()) {
	    combos.insert_or_assign ("HDR", 1);
	}

	try {
	    Shaders::Shader shader (
		*project.assetLocator, pass.shader, combos, overrideCombos, pass.textures, overrideTextures, noConstants
	    );
	    const auto sources
		= Shaders::GLSLContext::get ().toGlsl (shader.vertex (), shader.fragment (), pass.shader);
	    std::set<std::string> provided;

	    for (const auto& [bit, semantic] : formatSemantics) {
		if (format & bit) {
		    provided.insert (semantic);
		}
	    }

	    // the translation keeps the original source in an "#if 0" block at the end, that doesn't count
	    const std::string code = sources.vertex.substr (0, sources.vertex.rfind ("\n#if 0"));

	    for (std::sregex_iterator it (code.begin (), code.end (), input), end; it != end; ++it) {
		const std::string name = (*it)[1].str ();
		const auto semantic = attributeSemantics.find (name);
		const std::regex use ("\\b" + name + "\\b");
		const auto uses
		    = std::distance (std::sregex_iterator (code.begin (), code.end (), use), std::sregex_iterator ());

		// the D3D input signature only has the inputs the shader reads: live WE takes a position-only shape
		// with util/flat.json, whose flat.vert declares a_Color but only reads it with VERTEXCOLOR
		if (uses < 2) {
		    continue;
		}

		if (semantic != attributeSemantics.end () && !provided.contains (semantic->second)) {
		    result = ModelData::Error::ShaderExpectingMoreVertexData;
		    break;
		}
	    }
	} catch (const std::exception& e) {
	    sLog.error ("Model data material ", material, " has no usable shader: ", e.what ());
	    result = ModelData::Error::InvalidMaterial;
	}
    }

    this->m_modelDataMaterialChecks.emplace (key, result);
    return result;
}

void CScene::setAudioPolicy (bool muted, std::optional<int> ambientVolume) {
    const std::optional<int> volume = muted ? std::optional<int> (0) : ambientVolume;

    for (const auto& entry : this->m_objects) {
	if (entry.second->is<Objects::CSound> ()) {
	    entry.second->as<Objects::CSound> ()->setVolumeOverride (volume);
	}
    }
}
