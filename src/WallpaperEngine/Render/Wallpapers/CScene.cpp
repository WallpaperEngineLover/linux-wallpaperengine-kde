#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CLight.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CText.h"

#include "WallpaperEngine/Render/WallpaperState.h"

#include "CScene.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ranges>

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

    this->m_scriptEngine = std::make_unique<Scripting::ScriptEngine> (*this, context.getMediaSource ());
    this->m_camera = std::make_unique<Camera> (*this, scene->camera);

    float width = scene->camera.projection.width;
    float height = scene->camera.projection.height;

    // "auto" always matches the output resolution, same as real Wallpaper Engine. Used to be guessed
    // from the bounding box of every Image object's origin+size, but one object with a declared
    // "size" much bigger than what ends up on screen (e.g. an audio-bar visualizer sized for its
    // theoretical max) was enough to inflate the canvas and shrink everything else into a corner.
    if (scene->camera.projection.isAuto) {
	width = this->getContext ().getOutput ().getFullWidth ();
	height = this->getContext ().getOutput ().getFullHeight ();
    }

    this->m_cameraParallax = { 0, 0 };
    this->m_parallaxBias = { 0, 0 };
    this->m_parallaxPosition = { 0.5f, 0.5f };

    float canvasWidth = width;
    float canvasHeight = height;

    if (this->getContext ().getApp ().getContext ().settings.general.expandCanvas) {
	this->expandCanvasToContent (*scene, width, height, canvasWidth, canvasHeight);
    }

    this->m_camera->setOrthogonalProjection (width, height, canvasWidth, canvasHeight);

    // needed before scene setup below, which creates FBOs
    this->setupFramebuffers ();

    const uint32_t sceneWidth = this->m_camera->getCanvasWidth ();
    const uint32_t sceneHeight = this->m_camera->getCanvasHeight ();

    this->_rt_shadowAtlas = this->create (
	"_rt_shadowAtlas", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth, sceneHeight },
	{ sceneWidth, sceneHeight }
    );
    this->alias ("_alias_lightCookie", "_rt_shadowAtlas");

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

    // for the bloom effect below
    this->_rt_4FrameBuffer = this->create (
	"_rt_4FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 4, sceneHeight / 4 },
	{ sceneWidth / 4, sceneHeight / 4 }
    );
    this->_rt_8FrameBuffer = this->create (
	"_rt_8FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );
    this->_rt_Bloom = this->create (
	"_rt_Bloom", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );

    // Bloom is achieved without any custom code by synthesizing a fake image object that loads
    // effect files from the virtual container - this costs two extra draw calls versus official WPE,
    // which renders bloom directly to the screen, something a scene here never does.
    const auto bloomOrigin = glm::vec3 { sceneWidth / 2, sceneHeight / 2, 0.0f };
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
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } } }
			) } } }
	      ) } };

    if (scene->camera.bloom.enabled->value->getBool ()) {
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
    timeStep ("updateMouse", [&] { this->updateMouse (viewport); });

    if (this->getScene ().camera.parallax.enabled->value->getBool ()) {
	// the wallpaper's own position rides through the same per-layer depth/clamp mechanism as mouse parallax, halved to match its range
	// X keeps WallpaperState's sign flip, Y does not because this displacement is applied after the Y-up conversion
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
	    const glm::vec2 target = (this->m_mousePosition - glm::vec2 (0.5f, 0.5f)) * influence;
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

    // after the tick, so a layer a script moves this frame (e.g. onto input.cursorWorldPosition) is hit tested where it is now
    timeStep ("script tick", [&] { this->getScriptEngine ().tick (); });
    timeStep ("cursor events", [&] { this->dispatchCursorEvents (); });
    this->updateLights ();

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
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    glViewport (0, 0, this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight ());

    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const auto& cur : this->m_objectsByRenderOrder) {
	const auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
	if (debug.objectFilter.has_value () && cur->getId () != debug.objectFilter.value ()) {
	    continue;
	}
	if (std::ranges::find (debug.skipObjects, cur->getId ()) != debug.skipObjects.end ()) {
	    continue;
	}

	const auto visibility
	    = this->getContext ().getApp ().getContext ().resolveObjectVisibility (cur->getId (), cur->getObject ().name);
	if (visibility.has_value () && !visibility.value ()) {
	    continue;
	}

	if (this->isHiddenByAncestor (*cur)) {
	    continue;
	}

	timeStep ("render " + cur->getObject ().name, [&] { cur->render (); });
    }
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
    // handlers are free to create layers, which would move things around under a live iteration
    const auto objects = this->m_objectsByRenderOrder;

    for (auto* cur : objects) {
	if (!cur->is<Objects::CImage> ()) {
	    continue;
	}

	auto* image = cur->as<Objects::CImage> ();

	if (!engine.hasCursorHandlers (*image)) {
	    continue;
	}

	image->refreshScenePosition ();

	const int id = image->getImage ().id;
	const bool inside = image->getImage ().visible->value->getBool () && !this->isHiddenByAncestor (*cur)
	    && image->containsScenePoint (scenePosition);
	const bool wasInside = this->m_cursorInside.contains (id);
	const glm::vec2 local = scenePosition - image->getSceneCenter ();

	if (inside && !wasInside) {
	    this->m_cursorInside.insert (id);
	    engine.dispatchCursorEvent ("cursorEnter", *image, scenePosition, local);
	} else if (!inside && wasInside) {
	    this->m_cursorInside.erase (id);
	    engine.dispatchCursorEvent ("cursorLeave", *image, scenePosition, local);
	}

	if (inside && moved) {
	    engine.dispatchCursorEvent ("cursorMove", *image, scenePosition, local);
	}

	if (pressed && inside) {
	    this->m_cursorPressed.insert (id);
	    engine.dispatchCursorEvent ("cursorDown", *image, scenePosition, local);
	}

	if (released) {
	    const bool clicked = this->m_cursorPressed.erase (id) > 0 && inside;

	    if (inside) {
		engine.dispatchCursorEvent ("cursorUp", *image, scenePosition, local);
	    }

	    if (clicked) {
		engine.dispatchCursorEvent ("cursorClick", *image, scenePosition, local);
	    }
	}
    }
}

void CScene::updateMouse (const glm::ivec4& viewport) {
    const glm::dvec2 position = this->getContext ().getInputContext ().getMouseInput ().position ();

    this->m_mousePositionLast = this->m_mousePosition;

    double mouseX = glm::clamp ((position.x - viewport.x) / viewport.z, 0.0, 1.0);
    // OpenGL convention (0=bottom, 1=top) - particle code expects 0=bottom as negative Y (down)
    double normalizedMouseY = glm::clamp ((position.y - viewport.y) / viewport.w, 0.0, 1.0);

    // fill/fit scaling modes can render the scene larger than the viewport and crop via UVs
    const auto uvs = this->getState ().getTextureUVs ();

    this->m_mousePositionNormalized.x = uvs.ustart + mouseX * (uvs.uend - uvs.ustart);
    this->m_mousePositionNormalized.y = uvs.vstart + normalizedMouseY * (uvs.vend - uvs.vstart);

    // invert the Y normalization above to match what the shader expects
    double mouseY = 1.0 - normalizedMouseY;

    this->m_mousePosition.x = this->m_mousePositionNormalized.x;
    this->m_mousePosition.y = uvs.vstart + mouseY * (uvs.vend - uvs.vstart);
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
	if (image->parent.has_value () || image->size.x <= 0.0f || image->size.y <= 0.0f
	    || image->origin == nullptr || image->scale == nullptr) {
	    continue;
	}

	const auto visibility = this->getContext ().getApp ().getContext ().resolveObjectVisibility (image->id, image->name);

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

float CScene::getFps () const {
    const float dt = g_Time - g_TimeLast;
    // avoids a division by zero / bogus fps on the first frame, where g_TimeLast is still 0
    if (dt <= 1e-6f) {
	return 60.0f;
    }
    return 1.0f / dt;
}

const glm::vec2* CScene::getMousePosition () const { return &this->m_mousePosition; }

const glm::vec2* CScene::getMousePositionLast () const { return &this->m_mousePositionLast; }

const glm::vec2* CScene::getMousePositionNormalized () const { return &this->m_mousePositionNormalized; }

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
	// real engine: (origin - cameraPosition) * amount * depth, camera sitting at the mouse-driven point of the
	// scene. Origins are y-up and this space is y-down, hence the flipped Y terms
	const glm::vec3 origin = anchor->origin->value->getVec3 ();
	shift.x += (origin.x - width * 0.5f - this->m_cameraParallax.x * width) * amount * depth->x;
	shift.y -= (origin.y - height * 0.5f + this->m_cameraParallax.y * height) * amount * depth->y;
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

	const auto override
	    = this->getContext ().getApp ().getContext ().resolveObjectVisibility (data.id, data.name);
	const bool visible = override.has_value () ? override.value () : data.visible->value->getBool ();

	if (!visible || this->isHiddenByAncestor (*light)) {
	    continue;
	}

	const int slot = light->getSlot ();
	const float intensity = data.intensity->value->getFloat ();

	colors[slot] = glm::vec4 (glm::vec3 (data.color->value->getVec3 ()) * intensity, data.radius->value->getFloat ());
	positions[slot] = data.origin->value->getVec3 ();
    }

    // radiance is color / distance^2 in the shader, so the color is scaled by radius^2; the fourth light's
    // color rides in the .w of the other three
    for (int i = 0; i < 3; i++) {
	this->m_lightsColorPremultiplied[i] = glm::vec4 (glm::vec3 (colors[i]) * colors[i].w * colors[i].w, 0.0f);
	this->m_lightsColorPremultiplied[i].w = colors[3][i] * colors[3].w * colors[3].w;
    }

    std::ranges::copy (positions, this->m_lightsPosition);
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

Render::CObject* CScene::createLayer (const std::string& imagePath) {
    const int id = this->m_nextDynamicLayerId++;
    const std::string name = "scriptlayer_" + std::to_string (id);

    // same minimal-object-JSON approach the constructor uses for the bloom layer, so this gets the
    // same defaults a real scene.json image object would
    const auto tryBuild = [&] (const std::string& path) -> Render::CObject* {
	const JSON layerJson = {
	    { "id", id },
	    { "name", name },
	    { "image", path },
	    { "visible", true },
	};

	auto objectData = ObjectParser::parse (layerJson, this->getScene ().project);
	Render::CObject* renderObject = this->createObject (*objectData);

	if (renderObject == nullptr) {
	    return nullptr;
	}

	this->m_dynamicObjectData.emplace_back (std::move (objectData));
	this->appendLayer (renderObject);

	return renderObject;
    };

    // createObject() throws on a missing asset, and unwinding a C++ exception through the QuickJS callback is undefined behavior
    try {
	if (const auto cached = this->m_createLayerAliases.find (imagePath); cached != this->m_createLayerAliases.end ()) {
	    return tryBuild (cached->second);
	}

	return tryBuild (imagePath);
    } catch (const std::exception& e) {
	// scripts written against a workshop dependency's original layout use the un-prefixed name, try the prefixed copy first
	if (const auto alias = this->getScene ().project.assetLocator->resolveWorkshopDependencyAlias (imagePath);
	    alias.has_value () && !this->m_createLayerAliases.contains (imagePath)) {
	    try {
		sLog.out (
		    "createLayer: '", imagePath, "' not found, found and using workshop-dependency copy '",
		    alias->string (), "' instead"
		);
		auto* layer = tryBuild (alias->string ());
		this->m_createLayerAliases.emplace (imagePath, alias->string ());
		return layer;
	    } catch (const std::exception& aliasError) {
		sLog.error ("createLayer: workshop-dependency copy '", alias->string (), "' also failed: ", aliasError.what ());
	    }
	} else {
	    sLog.error ("createLayer failed for '", imagePath, "', falling back to an invisible placeholder: ", e.what ());
	}

	// scripts write into createLayer()'s result without a null check, so fall back to a scriptable, non-rendering placeholder
	try {
	    const JSON placeholderJson = { { "id", id }, { "name", name }, { "visible", true } };
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

void CScene::setAudioPolicy (bool muted, std::optional<int> ambientVolume) {
    const std::optional<int> volume = muted ? std::optional<int> (0) : ambientVolume;

    for (const auto& entry : this->m_objects) {
	if (entry.second->is<Objects::CSound> ()) {
	    entry.second->as<Objects::CSound> ()->setVolumeOverride (volume);
	}
    }
}
