#include "CParticle.h"

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Maths.h"
#include "WallpaperEngine/Render/Utils/NoiseUtils.h"

#include <GL/glew.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <numeric>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

extern float g_Time;
extern float g_RealTime;

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Utils;
using namespace WallpaperEngine::Data::Model;

namespace {
/** sub_1401D15A0 cases 0xB/0xC: a color moved in HSV by the override color's distance to the reference */
glm::vec3 shiftColor (const glm::vec3& rgb, const glm::vec3& shift) {
    const glm::vec3 hsv = WallpaperEngine::Maths::rgbToHsv (rgb);
    const float hue = shift.x + hsv.x;
    return WallpaperEngine::Maths::hsvToRgb (glm::vec3 (
	hue - std::floor (hue), std::clamp (shift.y + hsv.y, 0.0f, 1.0f), std::clamp (shift.z + hsv.z, 0.0f, 1.0f)
    ));
}
} // namespace

CParticle::CParticle (Wallpapers::CScene& scene, const Particle& particle) :
    CObject (scene, particle), CRenderable (scene, particle, *particle.material->material),
    ScriptableObject (scene, particle), m_particle (particle) {
    this->registerProperty ("scale", *particle.scale->value);
    this->registerProperty ("angles", *particle.angles->value);
    this->registerProperty ("visible", *particle.visible->value);
    this->registerProperty ("parallaxDepth", *particle.parallaxDepth->value);

    this->detectTexture ();
    m_worldSpace = (particle.flags & 1) != 0;
    if (std::getenv ("LWE_FIXED_TIMESTEP") != nullptr) {
	m_rng.seed (static_cast<std::mt19937::result_type> (this->getId ()));
    } else {
	std::random_device rd;
	m_rng.seed (rd ());
    }

    // Read renderer config early - buffer sizing below depends on it
    if (!m_particle.renderers.empty ()) {
	const auto& renderer = m_particle.renderers[0];
	if (renderer.name == "rope" || renderer.name == "ropetrail") {
	    // Both rope and ropetrail use genericropeparticle shader
	    m_useRopeRenderer = true;
	    m_ropeSubdivision = std::max (0, static_cast<int> (renderer.subdivision));
	    m_ropeUVScale = renderer.uvScale;
	    m_ropeUVScrolling = renderer.uvScrolling;
	    m_ropeUVSmoothing = renderer.uvSmoothing;

	    if (renderer.name == "ropetrail") {
		// clamps from wallpaper64.exe sub_1401C5490
		m_useTrailRenderer = true;
		m_trailLength = std::max (renderer.length, 0.001f);
		m_ropeSegments = std::clamp (static_cast<int> (renderer.segments), 2, 32);
		m_ropeSubdivision = std::clamp (static_cast<int> (renderer.subdivision), 0, 32);
		m_trailFadeAlpha = renderer.fadeAlpha;
		m_trailFadeSize = renderer.fadeSize;
	    }
	} else if (renderer.name == "spritetrail") {
	    // spritetrail uses genericparticle with TRAILRENDERER combo
	    m_useTrailRenderer = true;
	    m_trailLength = renderer.length;
	    m_trailMaxLength = renderer.maxLength;
	    m_trailMinLength = renderer.minLength;
	}
    }

    float countMultiplier = particle.instanceOverride.count->value->getFloat ();
    uint32_t adjustedMaxCount = static_cast<uint32_t> (particle.maxCount * countMultiplier);

    // Use wallpaper's specified count, or default if maxCount is 0
    m_maxParticles = (adjustedMaxCount > 0) ? adjustedMaxCount : DEFAULT_MAX_PARTICLES;

    m_particles.resize (m_maxParticles);
    m_slotUsed.assign (m_maxParticles, 0);

    if (m_useRopeRenderer && m_useTrailRenderer) {
	// ropetrail: a quad per history segment of every particle
	const size_t quads = static_cast<size_t> (m_maxParticles) * m_ropeSegments;
	m_vertices.resize (quads * 4 * ROPE_FLOATS_PER_VERTEX);
	m_indices.resize (quads * 6);
	m_trailHistory.resize (static_cast<size_t> (m_maxParticles) * m_ropeSegments);
	m_trailCount.resize (m_maxParticles);
	m_trailScroll.resize (m_maxParticles);
	m_trailInterval = m_trailLength / static_cast<float> (m_ropeSegments);
    } else if (m_useRopeRenderer) {
	// Rope: N particles connect via (N-1) segments, each subdivided into sub-segments
	const int subdivision = std::max (1, m_ropeSubdivision);
	const int maxSubSegments = std::max (1, static_cast<int> (m_maxParticles - 1)) * subdivision;
	m_vertices.resize (maxSubSegments * 4 * ROPE_FLOATS_PER_VERTEX);
	m_indices.resize (maxSubSegments * 6);
    } else {
	// 4 vertices, 6 indices per particle
	const int verticesPerParticle = 4;
	const int indicesPerParticle = 6;

	m_vertices.resize (m_maxParticles * verticesPerParticle * SPRITE_FLOATS_PER_VERTEX);
	m_indices.resize (m_maxParticles * indicesPerParticle);
    }
}

CParticle::CParticle (CParticle& parent, const ParticleChild& child) :
    CParticle (parent.getScene (), *child.particle) {
    m_parent = &parent;
    m_childDefinition = &child;
    m_placement = child.transform;
}

CParticle::~CParticle () {
    delete m_pass;

    if (m_vao != 0) {
	glDeleteVertexArrays (1, &m_vao);
    }
    if (m_vbo != 0) {
	glDeleteBuffers (1, &m_vbo);
    }
    if (m_ebo != 0) {
	glDeleteBuffers (1, &m_ebo);
    }

    m_vertices.clear ();
    m_indices.clear ();
}

void CParticle::setup () {
    if (m_initialized) {
	return;
    }

    m_lastScreenWidth = getScene ().getCamera ().getWidth ();
    m_lastScreenHeight = getScene ().getCamera ().getHeight ();
    if (m_parent == nullptr) {
	m_transformedOrigin = this->sceneOrigin ();
    }

    if (m_particle.material && m_particle.material->material && !m_particle.material->material->passes.empty ()) {
	auto& firstPass = *m_particle.material->material->passes.begin ();

	// Overbright: brightness multiplier for additive particles
	auto overbrightIt = firstPass->constants.find ("ui_editor_properties_overbright");
	if (overbrightIt != firstPass->constants.end ()) {
	    m_overbright = overbrightIt->second->value->getFloat ();
	}
    }

    // TextureParser computes the spritesheet grid from TEXS frame data (animated textures) or
    // .tex-json metadata (static textures). GIF-style animated textures (separate GL texture per
    // frame) get 0 cols/rows since a 1x1 grid can't hold all frames - no SPRITESHEET mode needed,
    // frame switching happens via texture ID instead.
    if (const auto texture = getTexture ()) {
	m_spritesheetCols = static_cast<int> (texture->getSpritesheetCols ());
	m_spritesheetRows = static_cast<int> (texture->getSpritesheetRows ());
	m_spritesheetFrames = static_cast<int> (texture->getSpritesheetFrames ());
	m_spritesheetDuration = texture->getSpritesheetDuration ();
    }

    // wallpaper64.exe system flag 2: only angularvelocityrandom and angularmovement make angular speed a thing, the
    // remap components ignore it otherwise
    for (const auto& initializer : m_particle.initializers) {
	if (initializer && initializer->is<AngularVelocityRandomInitializer> ()) {
	    m_hasAngularVelocity = true;
	}
    }
    for (const auto& op : m_particle.operators) {
	if (op && op->is<AngularMovementOperator> ()) {
	    m_hasAngularVelocity = true;
	}
    }

    setupEmitters ();
    setupInitializers ();
    setupOperators ();
    setupPass ();

    m_controlPoints.resize (8);
    for (const auto& cp : m_particle.controlPoints) {
	if (cp.id >= 0 && cp.id < 8) {
	    auto& point = m_controlPoints[cp.id];
	    // offsets are in WE's y-up particle space like emitter origins, particles here are mirrored on y
	    point.offset = glm::vec3 (cp.offset.x, -cp.offset.y, cp.offset.z);
	    point.linkMouse = (cp.flags & 1) != 0;
	    point.worldSpace = (cp.flags & 2) != 0;
	    point.followParent = (cp.flags & 4) != 0;
	    point.copyUntransformed = (cp.flags & 8) != 0;
	    point.parentIndex = cp.parentControlPoint;
	    // sub_14022C3C0 starts every control point as a plain translation to its offset
	    point.position = point.offset;

	    if (point.linkMouse) {
		m_hasMouseControlPoint = true;
	    }
	}
    }
    for (size_t i = 0; i < m_controlPoints.size (); i++) {
	m_controlPoints[i].remapOutput = (m_particle.remapOutputControlPoints & (1u << i)) != 0;
    }

    this->updateFrame ();
    this->updateControlPoints ();

    m_initialized = true;

    this->refreshColorOverride ();
    this->setupChildren ();
}

void CParticle::render () {
    if (!m_initialized) {
	return;
    }

    const auto& appContext = this->getScene ().getContext ().getApp ().getContext ();
    const auto visibility = appContext.resolveObjectVisibility (this->getId (), this->getObject ().name);
    if (!visibility.value_or (m_particle.visible->value->getBool ())) {
	// sub_140230650 starts the layer's time over while it is hidden
	m_layerTime = 0.0f;
	return;
    }

    syncTransformedOrigin ();

    // stop() drops every particle, and a later play() starts emitting from scratch
    const auto playback = this->getPlayback ();
    if (playback == Playback::Stopped) {
	m_particleCount = 0;
	std::fill (m_slotUsed.begin (), m_slotUsed.end (), 0);
	m_slotExtent = 0;
	std::fill (m_ghostUsed.begin (), m_ghostUsed.end (), 0);
	if (m_lastPlayback != Playback::Stopped) {
	    this->clearEventChildren ();
	    for (const auto& child : m_staticChildren) {
		child->restart ();
	    }
	}
    } else if (m_lastPlayback == Playback::Stopped) {
	m_emitters.clear ();
	setupEmitters ();
    }
    m_lastPlayback = playback;

    const float currentTime = m_hasMouseControlPoint ? g_RealTime : g_Time;

    // Initialize time on first render to avoid a huge dt spike, and skip the update
    // that frame to avoid an initial burst
    if (m_time == 0.0) {
	// "starttime" prewarms the system so it starts already populated instead of every
	// particle visibly leaving the emitter at once
	if (playback == Playback::Playing) {
	    this->prewarm (currentTime);
	}
	m_time = currentTime;
	this->draw (glm::mat4 (1.0f));
	return;
    }

    float dt = currentTime - static_cast<float> (m_time);
    m_time = currentTime;

    if (dt > 0.0f && playback != Playback::Stopped) {
	// Cap dt to prevent simulation instability across different FPS
	dt = std::min (dt, 0.1f);
	update (dt);
    }

    this->draw (glm::mat4 (1.0f));
}

void CParticle::draw (const glm::mat4& base) {
    // sub_140236600 / sub_1402366F0: world space systems draw with the stack's base, everything else multiplies
    // its +928 matrix onto what the parent left
    const glm::mat4 placement = m_parent == nullptr ? this->objectMatrix () : m_placement;
    const glm::mat4 top = m_worldSpace ? base : base * placement;
    m_modelMatrix = m_worldSpace ? glm::mat4 (1.0f) : top;

    if (m_particleCount > 0 && m_particle.material) {
	if (m_useRopeRenderer) {
	    renderRope ();
	} else {
	    renderSprites ();
	}
    }

    if (m_staticChildren.empty () && m_eventChildren.empty ()) {
	return;
    }

    // drawn right away, depth first: static children, then event children slot by slot. A world space system
    // hands down its raw +928 matrix, for a static child that is only the child transform, not its full frame
    const glm::mat4 childBase = m_worldSpace ? placement : top;
    for (const auto& child : m_staticChildren) {
	child->draw (childBase);
    }
    for (const auto& slot : m_eventChildren) {
	for (const auto& child : slot.active) {
	    child->draw (childBase);
	}
    }
}

void CParticle::prewarm (double now) {
    if (m_prewarmed || m_particle.startTime <= 0.0f) {
	return;
    }
    m_prewarmed = true;

    // wallpaper64.exe sub_14022EBE0: whole fixed steps, coarser ones for big systems, and no child events meanwhile
    const float step = m_particle.maxCount < 500 ? 0.05f : 0.2f;

    m_prewarming = true;
    m_time = now - m_particle.startTime;
    for (float done = 0.0f; done < m_particle.startTime; done += step) {
	m_time += step;
	update (step);
    }
    m_time = now;
    m_prewarming = false;
}

bool CParticle::isPlaying () const {
    const auto playback = this->getPlayback ();

    return playback == Playback::Playing || (playback == Playback::Paused && m_particleCount > 0);
}

glm::vec3 CParticle::sceneOrigin () const {
    glm::vec3 origin = m_particle.origin->value->getVec3 ();

    // 3D scenes place everything in world units as they are
    if (getScene ().getCamera ().isPerspective ()) {
	return origin;
    }

    // screen space (0,0 top-left) to the centered space of the ortho(-width/2, width/2, -height/2, height/2) projection
    const float screenWidth = static_cast<float> (getScene ().getWidth ());
    const float screenHeight = static_cast<float> (getScene ().getHeight ());

    origin.x -= screenWidth / 2.0f;
    origin.y = screenHeight / 2.0f - origin.y;
    return origin;
}

// scripts can move the system every frame (e.g. an origin that follows the cursor)
void CParticle::syncTransformedOrigin () {
    const float screenWidth = static_cast<float> (getScene ().getWidth ());
    const float screenHeight = static_cast<float> (getScene ().getHeight ());
    const glm::vec3 origin = this->sceneOrigin ();

    if (origin == m_transformedOrigin && screenWidth == m_lastScreenWidth && screenHeight == m_lastScreenHeight) {
	return;
    }

    m_transformedOrigin = origin;
    m_lastScreenWidth = screenWidth;
    m_lastScreenHeight = screenHeight;
}

void CParticle::update (float dt) {
    // children share the instance override and scale their own time
    const float childDt = dt;

    // instanceoverride "rate" scales the whole simulation's time, not only emission (wallpaper64.exe sub_1401B7FF0)
    dt *= std::max (0.01f, m_particle.instanceOverride.rate->value->getFloat ());

    // prewarming runs the simulation directly, the layer and system clocks only move in real updates
    if (!m_prewarming) {
	m_systemTime += dt;
	if (m_parent == nullptr) {
	    m_layerTime += dt;
	}
    }
    if (g_RealTime != m_lastRealTime) {
	if (m_lastRealTime > 0.0f) {
	    m_frameDelta = g_RealTime - m_lastRealTime;
	}
	m_lastRealTime = g_RealTime;
	m_frameCounter++;
    }

    this->refreshColorOverride ();

    // sub_140236CD0: ages first, so a new particle is drawn at age 0 on its first frame, then control points,
    // emission and operators
    for (uint32_t i = 0; i < m_particleCount; i++) {
	m_particles[i].age += dt;
    }

    // Order-preserving compaction: particles only die from lifetime expiry (never from
    // size, since size can oscillate), and index 0 must stay the oldest particle
    uint32_t writeIdx = 0;
    for (uint32_t readIdx = 0; readIdx < m_particleCount; readIdx++) {
	if (m_particles[readIdx].isAlive ()) {
	    if (writeIdx != readIdx) {
		m_particles[writeIdx] = m_particles[readIdx];
		if (!m_trailHistory.empty ()) {
		    std::copy_n (
			m_trailHistory.begin () + static_cast<size_t> (readIdx) * m_ropeSegments, m_ropeSegments,
			m_trailHistory.begin () + static_cast<size_t> (writeIdx) * m_ropeSegments
		    );
		    m_trailCount[writeIdx] = m_trailCount[readIdx];
		    m_trailScroll[writeIdx] = m_trailScroll[readIdx];
		}
	    }
	    writeIdx++;
	} else {
	    const uint32_t slot = m_particles[readIdx].slot;
	    if (slot < m_slotUsed.size ()) {
		m_slotUsed[slot] = 0;
	    }
	    if (slot < m_ghostUsed.size ()) {
		m_ghosts[slot] = m_particles[readIdx];
		m_ghostUsed[slot] = 1;
	    }
	    if (m_hasDeathEvents && !m_prewarming) {
		m_deaths.push_back (m_particles[readIdx]);
	    }
	}
    }
    m_particleCount = writeIdx;

    this->updateFrame ();
    this->updateControlPoints ();

    for (auto& cp : m_controlPoints) {
	cp.movement = cp.hasPreviousPosition ? cp.position - cp.previousPosition : glm::vec3 (0.0f);
	cp.velocity = cp.hasPreviousPosition && dt > 0.0f ? cp.movement / dt : glm::vec3 (0.0f);
	cp.previousPosition = cp.position;
	cp.hasPreviousPosition = true;
    }

    // pause() stops emission but keeps simulating what is already alive
    if (this->getPlayback () == Playback::Playing && !m_emissionStopped) {
	const uint32_t firstNew = m_particleCount;

	for (auto& emitter : m_emitters) {
	    emitter (m_particles, m_particleCount, dt);
	}
	m_emitterTime += dt;

	for (uint32_t i = firstNew; i < m_particleCount; i++) {
	    auto& p = m_particles[i];
	    p.id = m_nextParticleId++;

	    // sub_1402378A0 puts a new particle in the lowest free pool slot
	    const auto freeSlot = std::find (m_slotUsed.begin (), m_slotUsed.end (), 0);
	    p.slot = static_cast<uint32_t> (freeSlot - m_slotUsed.begin ());
	    if (freeSlot != m_slotUsed.end ()) {
		*freeSlot = 1;
	    }
	    m_slotExtent = std::max (m_slotExtent, p.slot + 1);
	    if (p.slot < m_ghostUsed.size ()) {
		m_ghostUsed[p.slot] = 0;
	    }

	    // sub_14023B340 end: a new particle's whole trail history starts where it spawned
	    if (!m_trailHistory.empty ()) {
		std::fill_n (m_trailHistory.begin () + static_cast<size_t> (i) * m_ropeSegments, m_ropeSegments, p.position);
		m_trailCount[i] = 1;
		m_trailScroll[i] = 0;
	    }

	    if (m_hasBirthEvents && !m_prewarming) {
		m_births.push_back (p.id);
	    }
	}
    }

    // sub_14023FBC0 starts from the spawn values of what a remapvalue writes, and remembers where particles are
    // for collisionquad
    if (m_resetSizeFromBase || m_resetAlphaFromBase || m_resetColorFromBase || m_tracksPreviousPosition) {
	for (uint32_t i = 0; i < m_particleCount; i++) {
	    auto& p = m_particles[i];
	    if (m_resetSizeFromBase) {
		p.size = p.initial.size;
	    }
	    if (m_resetAlphaFromBase) {
		p.alpha = p.initial.alpha;
	    }
	    if (m_resetColorFromBase) {
		p.color = p.initial.color;
	    }
	    if (m_tracksPreviousPosition) {
		p.previousPosition = p.position;
	    }
	}
    }

    for (auto& op : m_operators) {
	op (m_particles, m_particleCount, m_controlPoints, static_cast<float> (m_time), dt);
    }

    for (uint32_t i = 0; i < m_particleCount; i++) {
	auto& p = m_particles[i];

	if (m_spritesheetFrames > 0) {
	    float lifetimePos = p.getLifetimePos ();
	    float animSpeed = m_particle.sequenceMultiplier > 0.0f ? m_particle.sequenceMultiplier : 1.0f;

	    if (m_particle.animationMode == "randomframe") {
		if (p.frame < 0.0f) {
		    // per slot rather than per address, the address changes between runs
		    std::mt19937 particleRng (
			static_cast<std::mt19937::result_type> (i + this->getId () * 2654435761u)
		    );
		    std::uniform_int_distribution<int> dist (0, m_spritesheetFrames - 1);
		    p.frame = static_cast<float> (dist (particleRng));
		}
	    } else if (m_particle.animationMode == "once") {
		p.frame = std::min (
		    lifetimePos * m_spritesheetFrames * animSpeed, static_cast<float> (m_spritesheetFrames - 1)
		);
	    } else {
		if (m_spritesheetDuration > 0.0f) {
		    float timeInCycle = std::fmod (p.age * animSpeed, m_spritesheetDuration);
		    float cyclePos = timeInCycle / m_spritesheetDuration;
		    p.frame = std::fmod (cyclePos * m_spritesheetFrames, static_cast<float> (m_spritesheetFrames));
		} else {
		    p.frame = std::fmod (
			lifetimePos * m_spritesheetFrames * animSpeed, static_cast<float> (m_spritesheetFrames)
		    );
		}
	    }
	}
    }

    // sub_1402308A0: every interval each particle pushes its position to the front of its history. It runs with
    // the vertex build, which prewarming skips
    if (!m_trailHistory.empty () && !m_prewarming) {
	m_trailTimer -= dt;
	if (m_trailTimer <= 0.0f) {
	    m_trailTimer = m_trailInterval;
	    for (uint32_t i = 0; i < m_particleCount; i++) {
		const auto history = m_trailHistory.begin () + static_cast<size_t> (i) * m_ropeSegments;
		std::copy_backward (history, history + m_ropeSegments - 1, history + m_ropeSegments);
		*history = m_particles[i].position;
		m_trailCount[i] = static_cast<uint16_t> (std::min<int> (m_trailCount[i] + 1, m_ropeSegments));
		m_trailScroll[i]++;
	    }
	}
    }

    // prewarming leaves children alone, static ones prewarm on their own when created
    if (!m_prewarming) {
	this->updateChildren (childDt);
    }
}

void CParticle::refreshColorOverride () {
    const auto& instanceOverride = m_particle.instanceOverride;
    const glm::vec3 color = instanceOverride.colorn->value->getVec3 ();
    const glm::vec3 distance = glm::abs (m_particle.colorReference - color);

    // children only follow it while their parent does
    m_colorOverride.active = instanceOverride.hasColor && color.r >= 0.0f && (m_particle.flags & 8) == 0
	&& (distance.x >= 0.0035294117f || distance.y >= 0.0035294117f || distance.z >= 0.0035294117f)
	&& (m_parent == nullptr || m_parent->m_colorOverride.active);

    // a file without a color initializer, or any scene before version 5, multiplies by the color instead
    m_colorOverride.tint = m_colorOverride.active && m_particle.overrideColorTints ? color : glm::vec3 (1.0f);

    if (m_colorOverride.active) {
	m_colorOverride.hsv = WallpaperEngine::Maths::rgbToHsv (color);
	m_colorOverride.shift = m_colorOverride.hsv - WallpaperEngine::Maths::rgbToHsv (m_particle.colorReference);
    }
}

void CParticle::setupChildren () {
    for (const auto& child : m_particle.children) {
	if (child.particle == nullptr) {
	    continue;
	}

	if (child.type == ParticleChildType::Static) {
	    std::unique_ptr<CParticle> instance (new CParticle (*this, child));
	    instance->setup ();
	    instance->prewarm (m_time);
	    m_staticChildren.push_back (std::move (instance));
	    continue;
	}

	m_eventChildren.push_back (EventChildSlot { .child = &child, .active = {}, .pool = {} });
	m_hasBirthEvents |= child.type != ParticleChildType::EventDeath;
	m_hasDeathEvents |= child.type == ParticleChildType::EventDeath;
    }
}

const ParticleInstance* CParticle::findParticle (uint32_t id) const {
    // ids grow in spawn order and compaction keeps that order
    const auto end = m_particles.begin () + m_particleCount;
    const auto it = std::lower_bound (
	m_particles.begin (), end, id, [] (const ParticleInstance& p, uint32_t value) { return p.id < value; }
    );

    return it != end && it->id == id ? &*it : nullptr;
}

void CParticle::placeChild (const glm::mat4& placement) {
    m_placement = placement;
}

glm::mat4 CParticle::eventPlacement (const ParticleChild& child, const glm::vec3& position) const {
    // particle positions are in this system's frame, or the world when it is world space; the child's +928
    // matrix is relative to this frame, or the world when the child is world space
    const glm::mat4 placement = glm::translate (glm::mat4 (1.0f), position) * child.transform;
    const bool childWorldSpace = (child.particle->flags & 1) != 0;

    if (childWorldSpace == m_worldSpace) {
	return placement;
    }

    return childWorldSpace ? m_frame * placement : glm::inverse (m_frame) * placement;
}

glm::mat4 CParticle::objectMatrix () const {
    glm::vec3 scale = m_particle.scale->value->getVec3 ();
    glm::vec3 angles = m_particle.angles->value->getVec3 ();

    glm::mat4 matrix = glm::translate (glm::mat4 (1.0f), m_transformedOrigin);

    // CScene::renderFrame() already folds disableparallax into getParallaxDisplacement()
    if (getScene ().getScene ().camera.parallax.enabled->value->getBool ()) {
	const glm::vec2 offset = getScene ().getParallaxOffset (m_particle);
	matrix = glm::translate (matrix, glm::vec3 (offset.x, offset.y, 0.0f));
    }

    // Negate X and Z rotations to account for Y-flipped coordinate system, 3D scenes are not flipped
    const float flip = getScene ().getCamera ().isPerspective () ? 1.0f : -1.0f;
    matrix = glm::rotate (matrix, flip * angles.z, glm::vec3 (0, 0, 1));
    matrix = glm::rotate (matrix, angles.y, glm::vec3 (0, 1, 0));
    matrix = glm::rotate (matrix, flip * angles.x, glm::vec3 (1, 0, 0));
    return glm::scale (matrix, scale);
}

void CParticle::updateFrame () {
    if (m_parent == nullptr) {
	m_placement = this->objectMatrix ();
	m_frame = m_placement;
    } else if (m_worldSpace && m_childDefinition->type != ParticleChildType::Static) {
	// sub_140229760 replaces the stack top for world space systems
	m_frame = m_placement;
    } else {
	// static children go through sub_140229810, which always multiplies
	m_frame = m_parent->m_frame * m_placement;
    }
}

void CParticle::updateControlPoints () {
    const glm::mat4 toLocal = glm::inverse (m_frame);
    const glm::vec2* mousePos = getScene ().getMousePositionNormalized ();
    const bool takesParentParticles = m_childDefinition != nullptr && (m_childDefinition->flags & 1) != 0;
    const int firstParticlePoint = m_childDefinition != nullptr ? m_childDefinition->controlPointStartIndex : 0;

    for (size_t i = 0; i < m_controlPoints.size (); i++) {
	auto& cp = m_controlPoints[i];

	if (cp.remapOutput) {
	    continue;
	}

	if (cp.linkMouse) {
	    if (mousePos == nullptr) {
		continue;
	    }

	    // the cursor unprojected at NDC depth 0 replaces the point's translation, its offset plays no part
	    const glm::vec4 ndc { mousePos->x * 2.0f - 1.0f, (1.0f - mousePos->y) * 2.0f - 1.0f, 0.0f, 1.0f };
	    glm::vec3 position;
	    const auto& camera = getScene ().getCamera ();
	    if (camera.isPerspective ()) {
		const glm::vec4 world = glm::inverse (camera.getPerspective () * camera.getView ()) * ndc;
		position = glm::vec3 (world) / world.w;
	    } else {
		// the inverse of the centered orthographic projection
		const float screenWidth = static_cast<float> (getScene ().getWidth ());
		const float screenHeight = static_cast<float> (getScene ().getHeight ());
		position = glm::vec3 (ndc.x * screenWidth / 2.0f, ndc.y * screenHeight / 2.0f, 0.0f);
	    }

	    // world space systems keep x and y only
	    cp.position = m_worldSpace ? glm::vec3 (position.x, position.y, 0.0f)
				       : glm::vec3 (toLocal * glm::vec4 (position, 1.0f));
	    continue;
	}

	if (cp.followParent && m_parent != nullptr && cp.parentIndex >= 0
	    && cp.parentIndex < static_cast<int> (m_parent->m_controlPoints.size ())) {
	    const auto& source = m_parent->m_controlPoints[cp.parentIndex];
	    glm::mat4 matrix (source.orientation);
	    matrix[3] = glm::vec4 (source.position, 1.0f);

	    if (!cp.copyUntransformed && !(m_worldSpace && m_parent->m_worldSpace)) {
		if (m_worldSpace) {
		    matrix = m_parent->m_frame * matrix;
		} else if (m_parent->m_worldSpace) {
		    matrix = toLocal * matrix;
		} else {
		    matrix = toLocal * m_parent->m_frame * matrix;
		}
	    }

	    cp.orientation = glm::mat3 (matrix);
	    cp.position = glm::vec3 (matrix[3]);
	    continue;
	}

	// the parent's particles own these (sub_14022A580)
	if (!cp.followParent && takesParentParticles && static_cast<int> (i) >= firstParticlePoint) {
	    continue;
	}

	// sub_14022A070: offsets are local unless flag 2 says world, and the point lives in the system's space.
	// Control point 0 of a world space system always counts as local
	glm::mat4 matrix;
	if (m_worldSpace) {
	    if (cp.worldSpace && i != 0) {
		continue;
	    }
	    matrix = m_frame * glm::translate (glm::mat4 (1.0f), cp.offset);
	} else {
	    if (!cp.worldSpace) {
		continue;
	    }
	    matrix = toLocal * glm::translate (glm::mat4 (1.0f), cp.offset);
	}

	cp.orientation = glm::mat3 (matrix);
	cp.position = glm::vec3 (matrix[3]);
    }
}

void CParticle::spawnEventChildren (ParticleChildType type, const ParticleInstance& particle, bool alive) {
    for (auto& slot : m_eventChildren) {
	const auto& child = *slot.child;

	if (child.type != type || slot.active.size () >= static_cast<size_t> (std::max (0, child.maxCount))) {
	    continue;
	}
	if (WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) > child.probability) {
	    continue;
	}

	const glm::mat4 placement = this->eventPlacement (child, particle.position);

	// finished instances are kept around and started over instead of building a new pass every time
	std::unique_ptr<CParticle> instance;
	if (!slot.pool.empty ()) {
	    instance = std::move (slot.pool.back ());
	    slot.pool.pop_back ();
	    instance->placeChild (placement);
	    instance->restart ();
	} else {
	    instance.reset (new CParticle (*this, child));
	    instance->m_placement = placement;
	    instance->setup ();
	}

	// the inherit components read the event's particle from the first emission on, prewarm included
	instance->m_eventId = alive ? std::optional<uint32_t> (particle.id) : std::nullopt;
	instance->m_eventParticle = particle;
	instance->m_hasEventParticle = true;
	instance->m_following = type == ParticleChildType::EventFollow;
	instance->m_time = m_time;
	instance->prewarm (m_time);
	slot.active.push_back (std::move (instance));
    }
}

void CParticle::passControlPoints (CParticle& child, const ParticleChild& definition) const {
    // sub_14022A580: child flags bit 0 hands the parent's particles to the control points from the start index on,
    // one each in order. WE never gets past a control point that follows the cursor or the parent or that a remap
    // component owns (flags & 0x10005)
    if ((definition.flags & 1) == 0) {
	return;
    }

    // only the space switch through this system's frame, WE doesn't undo the child's own placement
    glm::mat4 transform (1.0f);
    if (m_worldSpace && !child.m_worldSpace) {
	transform = glm::inverse (m_frame);
    } else if (!m_worldSpace && child.m_worldSpace) {
	transform = m_frame;
    }

    auto& points = child.m_controlPoints;
    int index = std::max (0, definition.controlPointStartIndex);
    if (index >= static_cast<int> (points.size ()) || m_particleCount == 0) {
	return;
    }

    // WE walks its particle pool slot by slot, so the particles go out in slot order, not by age
    std::vector<uint32_t> order (m_particleCount);
    std::iota (order.begin (), order.end (), 0u);
    const size_t wanted = std::min<size_t> (order.size (), points.size () - index);
    std::partial_sort (order.begin (), order.begin () + wanted, order.end (), [this] (uint32_t a, uint32_t b) {
	return m_particles[a].slot < m_particles[b].slot;
    });

    for (size_t n = 0; n < wanted; n++) {
	auto& cp = points[index];
	if (cp.linkMouse || cp.followParent || cp.remapOutput) {
	    break;
	}
	cp.position = glm::vec3 (transform * glm::vec4 (m_particles[order[n]].position, 1.0f));
	index++;
    }
}

void CParticle::updateChildren (float dt) {
    for (const auto& child : m_staticChildren) {
	this->passControlPoints (*child, *child->m_childDefinition);
	child->m_time += dt;
	child->update (dt);
    }

    if (m_eventChildren.empty ()) {
	return;
    }

    // wallpaper64.exe sub_140236CD0 (deaths) and the end of sub_1402378A0 (births)
    for (const auto& particle : m_deaths) {
	this->spawnEventChildren (ParticleChildType::EventDeath, particle, false);
    }
    m_deaths.clear ();

    for (const uint32_t id : m_births) {
	if (const auto* p = this->findParticle (id)) {
	    const ParticleInstance particle = *p;
	    this->spawnEventChildren (ParticleChildType::EventFollow, particle, true);
	    this->spawnEventChildren (ParticleChildType::EventSpawn, particle, true);
	}
    }
    m_births.clear ();

    // sub_1402308A0: followers move with their particle, once it dies they stop emitting and fade out,
    // and finished instances go back to the pool
    for (auto& slot : m_eventChildren) {
	for (auto it = slot.active.begin (); it != slot.active.end ();) {
	    CParticle& child = **it;

	    if (child.m_eventId.has_value ()) {
		if (const auto* p = this->findParticle (*child.m_eventId)) {
		    child.m_eventParticle = *p;
		    if (child.m_following) {
			child.placeChild (this->eventPlacement (*slot.child, p->position));
		    }
		} else {
		    child.m_eventId.reset ();
		    if (child.m_following) {
			child.m_emissionStopped = true;
		    }
		}
	    }

	    this->passControlPoints (child, *slot.child);
	    child.m_time += dt;
	    child.update (dt);

	    if (child.isFinished ()) {
		slot.pool.push_back (std::move (*it));
		it = slot.active.erase (it);
	    } else {
		++it;
	    }
	}
    }

}

void CParticle::clearEventChildren () {
    for (auto& slot : m_eventChildren) {
	for (auto& child : slot.active) {
	    slot.pool.push_back (std::move (child));
	}
	slot.active.clear ();
    }
    m_births.clear ();
    m_deaths.clear ();
}

void CParticle::restart () {
    m_particleCount = 0;
    std::fill (m_slotUsed.begin (), m_slotUsed.end (), 0);
    std::fill (m_ghostUsed.begin (), m_ghostUsed.end (), 0);
    m_slotExtent = 0;
    m_systemTime = 0.0f;
    m_emitters.clear ();
    setupEmitters ();
    m_emitterTime = 0.0f;
    m_emissionStopped = false;
    m_prewarmed = false;
    m_eventId.reset ();
    m_hasEventParticle = false;
    m_following = false;
    for (auto& cp : m_controlPoints) {
	cp.hasPreviousPosition = false;
    }

    for (const auto& child : m_staticChildren) {
	child->restart ();
    }
    this->clearEventChildren ();
}

bool CParticle::isFinished () const {
    if (m_particleCount > 0 || (!m_emissionStopped && !this->emittersExhausted ())) {
	return false;
    }

    for (const auto& child : m_staticChildren) {
	if (!child->isFinished ()) {
	    return false;
	}
    }
    for (const auto& slot : m_eventChildren) {
	if (!slot.active.empty ()) {
	    return false;
	}
    }

    return true;
}

DynamicValue* CParticle::emitterCountOverride () const {
    // wallpaper64.exe sub_1401C5490 binds the instanceoverride count to every emitter's rate, speed to its
    // speedmin/speedmax, unless particle flag 0x20 / 0x10
    return (m_particle.flags & 0x20) == 0 ? m_particle.instanceOverride.count->value.get () : nullptr;
}

bool CParticle::emittersExhausted () const {
    // sub_1402378A0: past its delay an emitter without a rate switches off after its burst, one with a duration
    // once that runs out, and a rate without a duration keeps going forever
    for (const auto& emitter : m_particle.emitters) {
	if (emitter.rate <= 0.0f) {
	    if (m_emitterTime <= emitter.delay) {
		return false;
	    }
	} else if (emitter.duration <= 0.0f || m_emitterTime < emitter.delay + emitter.duration) {
	    return false;
	}
    }

    return true;
}

const Particle& CParticle::getParticle () const { return m_particle; }

const float& CParticle::getBrightness () const { return m_overbright; }

const float& CParticle::getUserAlpha () const { return m_particle.instanceOverride.alpha->value->getFloat (); }

const float& CParticle::getAlpha () const { return m_particle.instanceOverride.alpha->value->getFloat (); }

const glm::vec3& CParticle::getColor () const {
    static const glm::vec3 defaultColor (1.0f);
    if (m_particle.instanceOverride.color && m_particle.instanceOverride.color->value) {
	return m_particle.instanceOverride.color->value->getVec3 ();
    }
    return defaultColor;
}

const glm::vec4& CParticle::getColor4 () const {
    static const glm::vec4 defaultColor (1.0f);
    if (m_particle.instanceOverride.color && m_particle.instanceOverride.color->value) {
	return m_particle.instanceOverride.color->value->getVec4 ();
    }
    return defaultColor;
}

const glm::vec3& CParticle::getCompositeColor () const { return getColor (); }

// ========== EMITTERS ==========

void CParticle::setupEmitters () {
    for (const auto& emitter : m_particle.emitters) {
	EmitterFunc func;

	if (emitter.name == "boxrandom") {
	    func = createBoxEmitter (emitter);
	} else if (emitter.name == "sphererandom") {
	    func = createSphereEmitter (emitter);
	} else {
	    sLog.out ("Unknown emitter type: ", emitter.name);
	    continue;
	}

	if (func) {
	    m_emitters.push_back (std::move (func));
	}
    }
}

float CParticle::sampleAudio (
    int mode, const glm::vec2& bounds, float exponent, int frequencyStart, int frequencyEnd
) const {
    // same curve as wallpaper64.exe: modes 1/2/3 read left, right or (left + right) / 2 of the 16 band buffer
    if (mode == 0) {
	return 1.0f;
    }

    int first = std::clamp (frequencyStart, 0, 15);
    int last = std::clamp (frequencyEnd, 0, 15);

    if (last < first) {
	std::swap (first, last);
    }

    const auto& recorder = this->getScene ().getAudioContext ().getRecorder ();
    float peak = 0.0f;

    const float* left = recorder.audio16;
    const float* right = recorder.audio16 + 16;

    for (int i = first; i <= last; i++) {
	if (mode == 1) {
	    peak = std::max (peak, left[i]);
	} else if (mode == 2) {
	    peak = std::max (peak, right[i]);
	} else if (mode == 3) {
	    peak = std::max (peak, (left[i] + right[i]) * 0.5f);
	}
    }

    float t = (peak - bounds.x) / (bounds.y - bounds.x);
    // NaN from equal bounds ends up as 0 like the original
    t = t >= 1.0f ? 1.0f : (t >= 0.0f ? t : 0.0f);

    const float response = std::pow (t * t * (3.0f - 2.0f * t), exponent);

    return response >= 1.0f ? 1.0f : (response >= 0.0f ? response : 0.0f);
}

EmitterFunc CParticle::createBoxEmitter (const ParticleEmitter& emitter) {
    DynamicValue* countOverride = this->emitterCountOverride ();

    glm::vec3 transformedEmitterOrigin = emitter.origin;
    transformedEmitterOrigin.y = -transformedEmitterOrigin.y;

    int controlPointIndex = emitter.controlPoint;
    if (controlPointIndex == -1 && !m_particle.controlPoints.empty ()) {
	const auto& cp0 = m_particle.controlPoints[0];
	if ((cp0.flags & 1) != 0) {
	    controlPointIndex = 0;
	}
    }

    glm::vec3 flippedDirections = emitter.directions;
    flippedDirections.y = -flippedDirections.y;

    bool limitOnePerFrame = (emitter.flags & 2) != 0;
    bool randomPeriodicEmission = (emitter.flags & 4) != 0;

    return
	[this, emitter, transformedEmitterOrigin, controlPointIndex, countOverride, flippedDirections, limitOnePerFrame,
	 randomPeriodicEmission, emissionTimer = 0.0f, delayTimer = emitter.delay, durationTimer = 0.0f,
	 periodicTimer = 0.0f, periodicDuration = 0.0f, periodicDelay = 0.0f, emitting = false,
	 instantaneousEmitted = false] (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) mutable {
	    if (count >= particles.size ()) {
		return;
	    }

	    if (delayTimer > 0.0f) {
		delayTimer -= dt;
		return;
	    }

	    if (emitter.duration > 0.0f) {
		durationTimer += dt;
		if (durationTimer >= emitter.duration) {
		    return;
		}
	    }

	    if (randomPeriodicEmission) {
		periodicTimer += dt;

		if (!emitting) {
		    if (periodicTimer >= periodicDelay) {
			emitting = true;
			periodicTimer = 0.0f;
			periodicDuration = WallpaperEngine::Maths::randomFloat (
			    m_rng, emitter.minPeriodicDuration, emitter.maxPeriodicDuration
			);
		    } else {
			return;
		    }
		} else {
		    if (periodicTimer >= periodicDuration) {
			emitting = false;
			periodicTimer = 0.0f;
			periodicDelay = WallpaperEngine::Maths::randomFloat (
			    m_rng, emitter.minPeriodicDelay, emitter.maxPeriodicDelay
			);
			return;
		    }
		}
	    }

	    uint32_t toEmit = 0;
	    if (emitter.instantaneous > 0 && !instantaneousEmitted) {
		toEmit = emitter.instantaneous;
		instantaneousEmitted = true;
	    }

	    if (emitter.rate > 0.0f) {
		const float audio = sampleAudio (
		    emitter.audioProcessingMode, emitter.audioProcessingBounds, emitter.audioProcessingExponent,
		    emitter.audioProcessingFrequencyStart, emitter.audioProcessingFrequencyEnd
		);
		const float rate = emitter.rate * (countOverride != nullptr ? countOverride->getFloat () : 1.0f);
		emissionTimer += dt * rate * audio;
		uint32_t rateEmit = static_cast<uint32_t> (emissionTimer);
		emissionTimer -= static_cast<float> (rateEmit);
		// limitOnePerFrame (flags bit 1): cap at 1 to prevent rope artifacts
		if (limitOnePerFrame && rateEmit > 1) {
		    rateEmit = 1;
		}
		toEmit += rateEmit;
	    }

	    for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
		auto& p = particles[count];

		const ControlPointData* cp
		    = controlPointIndex >= 0 && controlPointIndex < static_cast<int> (m_controlPoints.size ())
		    ? &m_controlPoints[controlPointIndex]
		    : nullptr;

		// Random position within the box volume (hollow box if distanceMin > 0)
		glm::vec3 randomPos;
		for (int axis = 0; axis < 3; axis++) {
		    float minDist = emitter.distanceMin[axis];
		    float maxDist = emitter.distanceMax[axis];
		    float dist = WallpaperEngine::Maths::randomFloat (m_rng, minDist, maxDist);
		    // Randomly flip sign to center the distribution
		    if (WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) < 0.5f) {
			dist = -dist;
		    }
		    randomPos[axis] = dist;
		}
		randomPos *= flippedDirections;

		this->placeSpawn (p, cp, controlPointIndex, transformedEmitterOrigin, randomPos);

		// Emitter does not set velocity - initializers handle that
		p.velocity = glm::vec3 (0.0f);
		p.acceleration = glm::vec3 (0.0f);
		p.rotation = glm::vec3 (0.0f);
		p.angularVelocity = glm::vec3 (0.0f);
		p.angularAcceleration = glm::vec3 (0.0f);

		p.color = m_colorOverride.tint;
		p.alpha = 1.0f * m_particle.instanceOverride.alpha->value->getFloat ();
		p.size = 20.0f * m_particle.instanceOverride.size->value->getFloat ();
		p.lifetime = 1.0f * m_particle.instanceOverride.lifetime->value->getFloat ();
		p.age = 0.0f;
		p.alive = true;
		p.frame = -1.0f;
		p.seed = m_usesParticleSeed ? WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) : 0.0f;

		p.initial.color = p.color;
		p.initial.alpha = p.alpha;
		p.initial.size = p.size;
		p.initial.lifetime = p.lifetime;

		// Reset oscillator state for reused particles
		p.oscillateAlpha = {};
		p.oscillateSize = {};
		p.oscillatePosition = {};

		// sub_14023B340 starts the step collisionquad looks at where the emitter put the particle
		p.previousPosition = p.position;

		for (auto& init : m_initializers) {
		    init (p);
		}

		count++;
	    }
	};
}

EmitterFunc CParticle::createSphereEmitter (const ParticleEmitter& emitter) {
    DynamicValue* countOverride = this->emitterCountOverride ();
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;
    float lifetime = 1.0f * m_particle.instanceOverride.lifetime->value->getFloat ();

    // Convert emitter origin from screen space (Y down) to centered space (Y up)
    glm::vec3 transformedEmitterOrigin = emitter.origin;
    transformedEmitterOrigin.y = -transformedEmitterOrigin.y;

    int controlPointIndex = emitter.controlPoint;

    // Auto-detect control point 0 if not specified and CP0 has linkMouse
    if (controlPointIndex == -1 && !m_particle.controlPoints.empty ()) {
	const auto& cp0 = m_particle.controlPoints[0];
	if ((cp0.flags & 1) != 0) { // bit 0 = linkMouse
	    controlPointIndex = 0;
	}
    }

    bool limitOnePerFrame = (emitter.flags & 2) != 0;

    return [this, emitter, transformedEmitterOrigin, controlPointIndex, countOverride, speedOverride, lifetime,
	    limitOnePerFrame,
	    emissionTimer = 0.0f,
	    remaining
	    = emitter.instantaneous] (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) mutable {
	if (count >= particles.size ()) {
	    return;
	}

	const float audio = sampleAudio (
	    emitter.audioProcessingMode, emitter.audioProcessingBounds, emitter.audioProcessingExponent,
	    emitter.audioProcessingFrequencyStart, emitter.audioProcessingFrequencyEnd
	);
	const float rate = emitter.rate * (countOverride != nullptr ? countOverride->getFloat () : 1.0f);
	emissionTimer += dt * rate * audio;
	uint32_t toEmit = static_cast<uint32_t> (emissionTimer);
	emissionTimer -= static_cast<float> (toEmit);
	// limitOnePerFrame (flags bit 1): cap at 1 to prevent rope artifacts
	if (limitOnePerFrame && toEmit > 1) {
	    toEmit = 1;
	}

	if (remaining > 0) {
	    toEmit = remaining;
	    remaining = 0;
	}

	for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
	    auto& p = particles[count];

	    const ControlPointData* cp
		= controlPointIndex >= 0 && controlPointIndex < static_cast<int> (m_controlPoints.size ())
		? &m_controlPoints[controlPointIndex]
		: nullptr;

	    // sub_1402378A0 sphererandom, in WE's y up space: a point in the unit ball (cone around x) scaled by
	    // directions, whose length also picks the distance between distancemin and distancemax
	    const float phi = glm::two_pi<float> () * WallpaperEngine::Maths::randomFloat (m_rng, 0.00001f, 1.0f);
	    const float coneMin = -std::cos (emitter.cone * glm::pi<float> ());
	    const float c = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * (1.0f - coneMin) + coneMin;
	    const float r = std::cbrt (WallpaperEngine::Maths::randomFloat (m_rng, 0.00001f, 1.0f));
	    const float s = std::sqrt (std::max (0.0f, 1.0f - c * c));
	    const glm::vec3 point = glm::vec3 (r * c, r * s * std::sin (phi), r * s * std::cos (phi)) * emitter.directions;
	    const float pointLength = glm::length (point);
	    const float distance
		= emitter.distanceMin.x + (emitter.distanceMax.x - emitter.distanceMin.x) * pointLength;
	    glm::vec3 randomPos = pointLength > 0.0f ? point / pointLength : glm::vec3 (0.0f);

	    // a nonzero sign component forces that axis to its sign, zero leaves it alone
	    for (int i = 0; i < 3; i++) {
		if (emitter.sign[i] > 0.0f) {
		    randomPos[i] = std::abs (randomPos[i]);
		} else if (emitter.sign[i] < 0.0f) {
		    randomPos[i] = -std::abs (randomPos[i]);
		}
	    }
	    randomPos *= distance;
	    // particles live in y down
	    randomPos.y = -randomPos.y;
	    this->placeSpawn (p, cp, controlPointIndex, transformedEmitterOrigin, randomPos);

	    // the velocity points along the (control point transformed) offset, a zero offset gets a random direction
	    // inside directions instead
	    glm::vec3 direction = randomPos;
	    if (glm::dot (direction, direction) < 0.0001f) {
		direction = emitter.directions
		    * glm::vec3 (
			WallpaperEngine::Maths::randomFloat (m_rng, 0.00001f, 1.0f) * 2.0f - 1.0f,
			WallpaperEngine::Maths::randomFloat (m_rng, 0.00001f, 1.0f) * 2.0f - 1.0f,
			WallpaperEngine::Maths::randomFloat (m_rng, 0.00001f, 1.0f) * 2.0f - 1.0f
		    );
		direction.y = -direction.y;
		if (cp != nullptr && (controlPointIndex != 0 || m_worldSpace)) {
		    direction = cp->orientation * direction;
		}
	    }
	    const float directionLength = glm::length (direction);
	    const float speedScale = speedOverride != nullptr ? speedOverride->getFloat () : 1.0f;
	    const float speed = (emitter.speedMin
				 + (emitter.speedMax - emitter.speedMin) * WallpaperEngine::Maths::randomFloat (m_rng, 0.00001f, 1.0f))
		* speedScale;
	    p.velocity = directionLength > 0.0f ? direction * (speed / directionLength) : glm::vec3 (0.0f);

	    p.acceleration = glm::vec3 (0.0f);
	    p.rotation = glm::vec3 (0.0f);
	    p.angularVelocity = glm::vec3 (0.0f);
	    p.angularAcceleration = glm::vec3 (0.0f);

	    p.color = m_colorOverride.tint;
	    p.alpha = 1.0f * m_particle.instanceOverride.alpha->value->getFloat ();
	    p.size = 20.0f * m_particle.instanceOverride.size->value->getFloat ();
	    p.lifetime = lifetime;
	    p.age = 0.0f;
	    p.alive = true;
	    p.frame = -1.0f;
	    p.seed = m_usesParticleSeed ? WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) : 0.0f;

	    p.initial.color = p.color;
	    p.initial.alpha = p.alpha;
	    p.initial.size = p.size;
	    p.initial.lifetime = p.lifetime;

	    // Reset oscillator state for reused particles
	    p.oscillateAlpha = {};
	    p.oscillateSize = {};
	    p.oscillatePosition = {};

	    p.previousPosition = p.position;

	    for (auto& init : m_initializers) {
		init (p);
	    }

	    count++;
	}
    };
}

void CParticle::placeSpawn (
    ParticleInstance& p, const ControlPointData* cp, int controlPointIndex, const glm::vec3& origin, glm::vec3& offset
) {
    // sub_1402378A0 box/sphere emitters: the shape turns with the control point's matrix unless it is control
    // point 0 of a local system, the emitter origin is added untransformed, and the velocity initializers get
    // the control point's rotation and scale either way
    if (cp != nullptr && (controlPointIndex != 0 || m_worldSpace)) {
	offset = cp->orientation * offset;
    }

    p.position = origin + offset + (cp != nullptr ? cp->position : glm::vec3 (0.0f));
    m_emitOrientation = cp != nullptr ? cp->orientation : glm::mat3 (1.0f);
}

// ========== INITIALIZERS ==========

void CParticle::setupInitializers () {
    for (const auto& initializer : m_particle.initializers) {
	if (!initializer) {
	    continue;
	}

	InitializerFunc func;

	if (initializer->is<ColorRandomInitializer> ()) {
	    func = createColorRandomInitializer (*initializer->as<ColorRandomInitializer> ());
	} else if (initializer->is<SizeRandomInitializer> ()) {
	    func = createSizeRandomInitializer (*initializer->as<SizeRandomInitializer> ());
	} else if (initializer->is<AlphaRandomInitializer> ()) {
	    func = createAlphaRandomInitializer (*initializer->as<AlphaRandomInitializer> ());
	} else if (initializer->is<LifetimeRandomInitializer> ()) {
	    const auto& lifeInit = *initializer->as<LifetimeRandomInitializer> ();
	    m_uniformLifetimes = (lifeInit.min->value->getFloat () == lifeInit.max->value->getFloat ());
	    func = createLifetimeRandomInitializer (lifeInit);
	} else if (initializer->is<VelocityRandomInitializer> ()) {
	    func = createVelocityRandomInitializer (*initializer->as<VelocityRandomInitializer> ());
	} else if (initializer->is<RotationRandomInitializer> ()) {
	    func = createRotationRandomInitializer (*initializer->as<RotationRandomInitializer> ());
	} else if (initializer->is<AngularVelocityRandomInitializer> ()) {
	    func = createAngularVelocityRandomInitializer (*initializer->as<AngularVelocityRandomInitializer> ());
	} else if (initializer->is<TurbulentVelocityRandomInitializer> ()) {
	    func = createTurbulentVelocityRandomInitializer (*initializer->as<TurbulentVelocityRandomInitializer> ());
	} else if (initializer->is<MapSequenceAroundControlPointInitializer> ()) {
	    func = createMapSequenceAroundControlPointInitializer (
		*initializer->as<MapSequenceAroundControlPointInitializer> ()
	    );
	} else if (initializer->is<InheritInitialValueFromEventInitializer> ()) {
	    func = createInheritInitialValueFromEventInitializer (
		*initializer->as<InheritInitialValueFromEventInitializer> ()
	    );
	} else if (initializer->is<InheritControlPointVelocityInitializer> ()) {
	    func = createInheritControlPointVelocityInitializer (
		*initializer->as<InheritControlPointVelocityInitializer> ()
	    );
	} else if (initializer->is<HsvColorRandomInitializer> ()) {
	    func = createHsvColorRandomInitializer (*initializer->as<HsvColorRandomInitializer> ());
	} else if (initializer->is<ColorListInitializer> ()) {
	    func = createColorListInitializer (*initializer->as<ColorListInitializer> ());
	} else if (initializer->is<PositionOffsetRandomInitializer> ()) {
	    func = createPositionOffsetRandomInitializer (*initializer->as<PositionOffsetRandomInitializer> ());
	} else if (initializer->is<MapSequenceBetweenControlPointsInitializer> ()) {
	    func = createMapSequenceBetweenControlPointsInitializer (
		*initializer->as<MapSequenceBetweenControlPointsInitializer> ()
	    );
	} else if (initializer->is<RemapInitialValueInitializer> ()) {
	    func = createRemapInitialValueInitializer (*initializer->as<RemapInitialValueInitializer> ());
	} else {
	    sLog.out ("Unknown initializer type");
	}

	if (func) {
	    m_initializers.push_back (std::move (func));
	}
    }
}

InitializerFunc CParticle::createColorRandomInitializer (const ColorRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    // wallpaper64.exe sub_14023B340 case 3, one random for all three channels. An active override color moves
    // both ends in HSV first (sub_1401D15A0 case 0xB)
    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	glm::vec3 min = minValue->getVec3 ();
	glm::vec3 max = maxValue->getVec3 ();
	if (m_colorOverride.active) {
	    min = shiftColor (min, m_colorOverride.shift);
	    max = shiftColor (max, m_colorOverride.shift);
	}

	float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	if (const float exponent = exponentValue->getFloat (); exponent != 1.0f) {
	    t = std::pow (t, exponent);
	}
	p.color *= (max - min) * t + min;
	p.initial.color = p.color;
    };
}

InitializerFunc CParticle::createSizeRandomInitializer (const SizeRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();
    DynamicValue* sizeOverride = (m_particle.flags & 0x80) == 0 ? m_particle.instanceOverride.size->value.get () : nullptr;

    return [this, minValue, maxValue, exponentValue, sizeOverride] (ParticleInstance& p) {
	float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	float exponent = exponentValue->getFloat ();
	float min = minValue->getFloat ();
	float max = maxValue->getFloat ();

	// Apply exponent for non-linear distribution
	float adjustedT = std::pow (t, exponent);
	p.size = (min + adjustedT * (max - min)) * (sizeOverride != nullptr ? sizeOverride->getFloat () : 1.0f) / 2.0f;
	p.initial.size = p.size;
    };
}

InitializerFunc CParticle::createAlphaRandomInitializer (const AlphaRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* alphaOverride = m_particle.instanceOverride.alpha->value.get ();

    return [this, minValue, maxValue, alphaOverride] (ParticleInstance& p) {
	p.alpha = WallpaperEngine::Maths::randomFloat (m_rng, minValue->getFloat (), maxValue->getFloat ())
	    * alphaOverride->getFloat ();
	p.initial.alpha = p.alpha;
    };
}

InitializerFunc CParticle::createLifetimeRandomInitializer (const LifetimeRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* lifetimeOverride = m_particle.instanceOverride.lifetime->value.get ();

    return [this, minValue, maxValue, lifetimeOverride] (ParticleInstance& p) {
	p.lifetime = WallpaperEngine::Maths::randomFloat (m_rng, minValue->getFloat (), maxValue->getFloat ())
	    * lifetimeOverride->getFloat ();
	p.initial.lifetime = p.lifetime;
    };
}

InitializerFunc CParticle::createVelocityRandomInitializer (const VelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    return [this, minValue, maxValue, speedOverride] (ParticleInstance& p) {
	glm::vec3 vel = WallpaperEngine::Maths::randomVec3 (m_rng, minValue->getVec3 (), maxValue->getVec3 ())
	    * speedOverride->getFloat ();
	vel.y = -vel.y;
	p.velocity += m_emitOrientation * vel;
    };
}

InitializerFunc CParticle::createRotationRandomInitializer (const RotationRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    return [this, minValue, maxValue, speedOverride] (ParticleInstance& p) {
	p.rotation = WallpaperEngine::Maths::randomVec3 (m_rng, minValue->getVec3 (), maxValue->getVec3 ())
	    * speedOverride->getFloat ();
    };
}

InitializerFunc CParticle::createAngularVelocityRandomInitializer (const AngularVelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    return [this, minValue, maxValue, exponentValue, speedOverride] (ParticleInstance& p) {
	glm::vec3 minVec = minValue->getVec3 ();
	glm::vec3 maxVec = maxValue->getVec3 ();
	float exponent = exponentValue->getFloat ();

	// exponent = 1: uniform; exponent -> 0: bias towards max; exponent >= 2: bias towards min
	glm::vec3 result;
	for (int i = 0; i < 3; i++) {
	    float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	    t = std::pow (t, exponent);
	    result[i] = minVec[i] + t * (maxVec[i] - minVec[i]);
	}

	p.angularVelocity = result * speedOverride->getFloat ();
    };
}

InitializerFunc CParticle::createTurbulentVelocityRandomInitializer (const TurbulentVelocityRandomInitializer& init) {
    DynamicValue* speedMin = init.speedMin->value.get ();
    DynamicValue* speedMax = init.speedMax->value.get ();
    DynamicValue* offsetVal = init.offset->value.get ();
    DynamicValue* scaleVal = init.scale->value.get ();
    DynamicValue* forwardVal = init.forward->value.get ();
    DynamicValue* timeScaleVal = init.timeScale->value.get ();
    DynamicValue* phaseMinVal = init.phaseMin->value.get ();
    DynamicValue* phaseMaxVal = init.phaseMax->value.get ();
    DynamicValue* rightVal = init.right->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();
    DynamicValue* audioModeValue = init.audioProcessingMode->value.get ();
    DynamicValue* audioBoundsValue = init.audioProcessingBounds->value.get ();
    DynamicValue* audioExponentValue = init.audioProcessingExponent->value.get ();
    DynamicValue* audioStartValue = init.audioProcessingFrequencyStart->value.get ();
    DynamicValue* audioEndValue = init.audioProcessingFrequencyEnd->value.get ();

    // same formula as wallpaper64.exe (sub_1401C8AF0)
    return [this, speedMin, speedMax, offsetVal, scaleVal, forwardVal, timeScaleVal, phaseMinVal, phaseMaxVal, rightVal,
	    speedOverride, audioModeValue, audioBoundsValue, audioExponentValue, audioStartValue,
	    audioEndValue] (ParticleInstance& p) {
	const float audio = sampleAudio (
	    audioModeValue->getInt (), audioBoundsValue->getVec2 (), audioExponentValue->getFloat (),
	    audioStartValue->getInt (), audioEndValue->getInt ()
	);
	const float phaseMin = phaseMinVal->getFloat ();
	const float phaseRange = (phaseMaxVal->getFloat () - phaseMin) * audio;
	// WE adds the scene's camera path fade here, 0 outside of fades, so there is no time term
	const float phase = phaseMin + WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * phaseRange;
	const float timeScale = timeScaleVal->getFloat () * m_particle.instanceOverride.rate->value->getFloat ();

	const float angle = simplexNoise1D (phase * timeScale) * glm::pi<float> () * scaleVal->getFloat ()
	    + offsetVal->getFloat ();
	const float speed = WallpaperEngine::Maths::randomFloat (m_rng, speedMin->getFloat (), speedMax->getFloat ());

	glm::vec3 right = rightVal->getVec3 ();

	if (glm::length (right) < 0.0001f) {
	    right = glm::vec3 (0.0f, 0.0f, 1.0f);
	}

	glm::vec3 direction = glm::mat3 (glm::rotate (glm::mat4 (1.0f), angle, right)) * forwardVal->getVec3 ();

	direction.y = -direction.y;

	// z moves nothing on screen in 2D systems but pulls rope segments apart in depth
	if ((m_particle.flags & 4) == 0) {
	    direction.z = 0.0f;
	}

	p.velocity += m_emitOrientation * (direction * speed * speedOverride->getFloat ());
    };
}

InitializerFunc
CParticle::createMapSequenceAroundControlPointInitializer (const MapSequenceAroundControlPointInitializer& init) {
    DynamicValue* controlPointValue = init.controlPoint->value.get ();
    DynamicValue* countValue = init.count->value.get ();
    DynamicValue* speedMinValue = init.speedMin->value.get ();
    DynamicValue* speedMaxValue = init.speedMax->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    // Sequence counter is shared (closure state) across all particles spawned by this
    // initializer, giving each one a distinct angle around the circle
    int sequenceIndex = 0;

    return [this, controlPointValue, countValue, speedMinValue, speedMaxValue, sequenceIndex,
	    speedOverride] (ParticleInstance& p) mutable {
	int controlPoint = static_cast<int> (controlPointValue->getFloat ());
	int count = static_cast<int> (countValue->getFloat ());
	if (count < 1) {
	    count = 1;
	}

	float angle = (static_cast<float> (sequenceIndex) / static_cast<float> (count)) * glm::two_pi<float> ();
	sequenceIndex = (sequenceIndex + 1) % count;

	glm::vec3 centerPos = glm::vec3 (0.0f);
	if (controlPoint >= 0 && controlPoint < static_cast<int> (m_controlPoints.size ())) {
	    centerPos = m_controlPoints[controlPoint].position;
	}

	p.position = centerPos;

	glm::vec3 speedMin = speedMinValue->getVec3 ();
	glm::vec3 speedMax = speedMaxValue->getVec3 ();
	glm::vec3 speed = WallpaperEngine::Maths::randomVec3 (m_rng, speedMin, speedMax);

	// Flip Y before rotation to convert to centered space
	speed.y = -speed.y;

	// Rotating by the sequence angle gives the outward radial/circular pattern
	glm::mat3 rotationMatrix = glm::mat3 (
	    std::cos (angle), -std::sin (angle), 0.0f, std::sin (angle), std::cos (angle), 0.0f, 0.0f, 0.0f, 1.0f
	);
	glm::vec3 rotatedSpeed = rotationMatrix * speed * speedOverride->getFloat ();

	p.velocity = rotatedSpeed;
    };
}

// ========== OPERATORS ==========

namespace {
/** Applies an inherit input from the event's particle, the initializer also moves the base values operators start from */
void applyEventInput (ParticleInstance& p, const ParticleInstance& source, ParticleEventInput input, bool initial) {
    switch (input) {
	case ParticleEventInput::SetColor: p.color = source.color; break;
	case ParticleEventInput::MultiplyColor: p.color *= source.color; break;
	case ParticleEventInput::SetOpacity: p.alpha = source.alpha; break;
	case ParticleEventInput::MultiplyOpacity: p.alpha *= source.alpha; break;
	case ParticleEventInput::SetColorOpacity:
	    p.color = source.color;
	    p.alpha = source.alpha;
	    break;
	case ParticleEventInput::MultiplyColorOpacity:
	    p.color *= source.color;
	    p.alpha *= source.alpha;
	    break;
	case ParticleEventInput::SetVelocity: p.velocity = source.velocity; break;
	case ParticleEventInput::AddVelocity: p.velocity += source.velocity; break;
	case ParticleEventInput::SetSize: p.size = source.size; break;
	case ParticleEventInput::MultiplySize: p.size *= source.size; break;
	case ParticleEventInput::SetRotation: p.rotation = source.rotation; break;
	case ParticleEventInput::AddRotation: p.rotation += source.rotation; break;
	case ParticleEventInput::SetAngularVelocity: p.angularVelocity = source.angularVelocity; break;
	case ParticleEventInput::AddAngularVelocity: p.angularVelocity += source.angularVelocity; break;
    }

    if (initial) {
	p.initial.color = p.color;
	p.initial.alpha = p.alpha;
	p.initial.size = p.size;
    }
}
} // namespace

InitializerFunc
CParticle::createInheritInitialValueFromEventInitializer (const InheritInitialValueFromEventInitializer& init) {
    const ParticleEventInput input = init.input;

    // wallpaper64.exe sub_14023B340 case 16: the event's particle as it was, a dead one included
    return [this, input] (ParticleInstance& p) {
	if (m_hasEventParticle) {
	    applyEventInput (p, m_eventParticle, input, true);
	}
    };
}

InitializerFunc
CParticle::createInheritControlPointVelocityInitializer (const InheritControlPointVelocityInitializer& init) {
    const int controlPoint = init.controlPoint;
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();

    // sub_14023B340 case 8: a random share of how fast the control point moved over the last frame
    return [this, controlPoint, minValue, maxValue] (ParticleInstance& p) {
	if (controlPoint < 0 || controlPoint >= static_cast<int> (m_controlPoints.size ())) {
	    return;
	}
	const auto& cp = m_controlPoints[controlPoint];
	glm::vec3 velocity = cp.velocity;
	// a world space system's control points move in the world, back into its own space unless flag 2 made
	// the point a world one, then through the emitter's orientation like every velocity initializer
	if (m_worldSpace && !cp.worldSpace) {
	    velocity = glm::mat3 (glm::inverse (m_frame)) * velocity;
	}
	const float share = WallpaperEngine::Maths::randomFloat (m_rng, minValue->getFloat (), maxValue->getFloat ());
	p.velocity += m_emitOrientation * (velocity * share);
    };
}

OperatorFunc CParticle::createInheritValueFromEventOperator (const InheritValueFromEventOperator& op) {
    const ParticleEventInput input = op.input;

    // same clamps as sub_1401C2A40 so the blend windows never have zero length
    const float inStart = std::min (op.blend.x, op.blend.y - 0.0001f);
    const float inEnd = op.blend.y;
    const float outStart = op.blend.z;
    const float outEnd = std::max (op.blend.w, op.blend.z + 0.0001f);

    // sub_14023FBC0 case 20: every frame while the event's particle lives, nothing for eventdeath children
    return [this, input, inStart, inEnd, outStart, outEnd] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float
	   ) {
	if (!m_eventId.has_value ()) {
	    return;
	}

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float life = p.getLifetimePos ();
	    const float weight = std::clamp ((life - inStart) / (inEnd - inStart), 0.0f, 1.0f)
		* std::clamp ((outEnd - life) / (outEnd - outStart), 0.0f, 1.0f);

	    if (weight >= 1.0f) {
		applyEventInput (p, m_eventParticle, input, false);
		continue;
	    }

	    ParticleInstance target = p;
	    applyEventInput (target, m_eventParticle, input, false);
	    p.color = glm::mix (p.color, target.color, weight);
	    p.alpha = glm::mix (p.alpha, target.alpha, weight);
	    p.velocity = glm::mix (p.velocity, target.velocity, weight);
	    p.size = glm::mix (p.size, target.size, weight);
	    p.rotation = glm::mix (p.rotation, target.rotation, weight);
	    p.angularVelocity = glm::mix (p.angularVelocity, target.angularVelocity, weight);
	}
    };
}

void CParticle::setupOperators () {
    for (const auto& op : m_particle.operators) {
	if (!op) {
	    continue;
	}

	OperatorFunc func;

	if (op->is<MovementOperator> ()) {
	    func = createMovementOperator (*op->as<MovementOperator> ());
	} else if (op->is<AngularMovementOperator> ()) {
	    func = createAngularMovementOperator (*op->as<AngularMovementOperator> ());
	} else if (op->is<AlphaFadeOperator> ()) {
	    func = createAlphaFadeOperator (*op->as<AlphaFadeOperator> ());
	} else if (op->is<SizeChangeOperator> ()) {
	    func = createSizeChangeOperator (*op->as<SizeChangeOperator> ());
	} else if (op->is<AlphaChangeOperator> ()) {
	    func = createAlphaChangeOperator (*op->as<AlphaChangeOperator> ());
	} else if (op->is<ColorChangeOperator> ()) {
	    func = createColorChangeOperator (*op->as<ColorChangeOperator> ());
	} else if (op->is<TurbulenceOperator> ()) {
	    func = createTurbulenceOperator (*op->as<TurbulenceOperator> ());
	} else if (op->is<VortexOperator> ()) {
	    func = createVortexOperator (*op->as<VortexOperator> ());
	} else if (op->is<ControlPointAttractOperator> ()) {
	    func = createControlPointAttractOperator (*op->as<ControlPointAttractOperator> ());
	} else if (op->is<OscillateAlphaOperator> ()) {
	    func = createOscillateAlphaOperator (*op->as<OscillateAlphaOperator> ());
	} else if (op->is<OscillateSizeOperator> ()) {
	    func = createOscillateSizeOperator (*op->as<OscillateSizeOperator> ());
	} else if (op->is<OscillatePositionOperator> ()) {
	    func = createOscillatePositionOperator (*op->as<OscillatePositionOperator> ());
	} else if (op->is<InheritValueFromEventOperator> ()) {
	    func = createInheritValueFromEventOperator (*op->as<InheritValueFromEventOperator> ());
	} else if (op->is<CapVelocityOperator> ()) {
	    func = createCapVelocityOperator (*op->as<CapVelocityOperator> ());
	} else if (op->is<BoidsOperator> ()) {
	    func = createBoidsOperator (*op->as<BoidsOperator> ());
	    if (!m_hasBoids) {
		m_hasBoids = true;
		m_ghosts.resize (m_maxParticles);
		m_ghostUsed.assign (m_maxParticles, 0);
	    }
	} else if (op->is<RemapValueOperator> ()) {
	    func = createRemapValueOperator (*op->as<RemapValueOperator> ());
	} else if (op->is<MaintainDistanceToControlPointOperator> ()) {
	    func = createMaintainDistanceToControlPointOperator (*op->as<MaintainDistanceToControlPointOperator> ());
	} else if (op->is<MaintainDistanceBetweenControlPointsOperator> ()) {
	    func = createMaintainDistanceBetweenControlPointsOperator (
		*op->as<MaintainDistanceBetweenControlPointsOperator> ()
	    );
	} else if (op->is<ReduceMovementNearControlPointOperator> ()) {
	    func = createReduceMovementNearControlPointOperator (*op->as<ReduceMovementNearControlPointOperator> ());
	} else if (op->is<CollisionOperator> ()) {
	    func = createCollisionOperator (*op->as<CollisionOperator> ());
	} else {
	    sLog.out ("Unknown operator type");
	}

	if (func) {
	    m_operators.push_back (std::move (func));
	}
    }
}

OperatorFunc CParticle::createMovementOperator (const MovementOperator& op) {
    DynamicValue* dragValue = op.drag->value.get ();
    DynamicValue* gravityValue = op.gravity->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    // sub_14023FBC0 case 1: velocity first, then position with the new velocity, then drag over the frame scaled dt.
    // It runs over every pool slot, dead ones included
    const bool turnGravity = (op.flags & 1) != 0;

    return [this, dragValue, gravityValue, speedOverride, turnGravity] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	glm::vec3 gravity = gravityValue->getVec3 () * speedOverride->getFloat ();
	gravity.y = -gravity.y;
	// sub_1401F87E0 multiplies the gravity by the frame's 3x3 from the other side (its transpose), so drawn
	// through the frame a rotation cancels out and the scale applies twice
	if (turnGravity && !m_worldSpace) {
	    gravity = glm::transpose (glm::mat3 (m_frame)) * gravity;
	}
	const float drag = std::min (dragValue->getFloat () * frameScaledDelta (dt), 0.99999988f);
	const glm::vec3 step = gravity * dt;

	const auto move = [&] (ParticleInstance& p) {
	    const glm::vec3 velocity = p.velocity + step;
	    p.position += velocity * dt;
	    p.velocity = velocity * (1.0f - drag);
	};

	for (uint32_t i = 0; i < count; i++) {
	    if (particles[i].alive) {
		move (particles[i]);
	    }
	}
	for (uint32_t slot = 0; slot < m_ghostUsed.size (); slot++) {
	    if (m_ghostUsed[slot]) {
		move (m_ghosts[slot]);
	    }
	}
    };
}

OperatorFunc CParticle::createAngularMovementOperator (const AngularMovementOperator& op) {
    DynamicValue* dragValue = op.drag->value.get ();
    DynamicValue* forceValue = op.force->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    return [dragValue, forceValue, speedOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	float drag = dragValue->getFloat ();
	float speed = speedOverride->getFloat ();
	glm::vec3 force = forceValue->getVec3 ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }

	    p.rotation += p.angularVelocity * dt * speed;

	    p.angularVelocity += force * dt * speed;

	    // Positive drag slows down, negative speeds up; clamped so drag*dt > 1.0 can't reverse it
	    float dragFactor = 1.0f - (drag * dt);
	    if (dragFactor < 0.0f) {
		dragFactor = 0.0f;
	    }
	    p.angularVelocity *= dragFactor;

	    // Wrap rotation to prevent floating-point precision issues
	    const float pi = glm::pi<float> ();
	    const float two_pi = glm::two_pi<float> ();
	    for (int j = 0; j < 3; j++) {
		while (p.rotation[j] > pi) {
		    p.rotation[j] -= two_pi;
		}
		while (p.rotation[j] < -pi) {
		    p.rotation[j] += two_pi;
		}
	    }
	}
    };
}

OperatorFunc CParticle::createAlphaFadeOperator (const AlphaFadeOperator& op) {
    DynamicValue* fadeInTimeValue = op.fadeInTime->value.get ();
    DynamicValue* fadeOutTimeValue = op.fadeOutTime->value.get ();

    return
	[fadeInTimeValue, fadeOutTimeValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float fadeInTime = fadeInTimeValue->getFloat ();
	    float fadeOutTime = fadeOutTimeValue->getFloat ();

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();

		if (life <= fadeInTime) {
		    float fade = WallpaperEngine::Maths::fadeValue (life, 0.0f, fadeInTime, 0.0f, 1.0f);
		    p.alpha = p.initial.alpha * fade;
		} else if (life > fadeOutTime) {
		    float fade = 1.0f - WallpaperEngine::Maths::fadeValue (life, fadeOutTime, 1.0f, 0.0f, 1.0f);
		    p.alpha = p.initial.alpha * fade;
		} else {
		    p.alpha = p.initial.alpha;
		}

		// Update oscillator base so oscillateAlpha combines properly
		p.oscillateAlpha.base = p.alpha;
	    }
	};
}

OperatorFunc CParticle::createSizeChangeOperator (const SizeChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    // wallpaper64.exe binds the instanceoverride size to both values (sub_1401C5490, sub_1401D15A0 case 7), on top of
    // sizerandom's own binding. Particle flag 0x80 turns the size bindings off
    DynamicValue* sizeOverride = (m_particle.flags & 0x80) == 0 ? m_particle.instanceOverride.size->value.get () : nullptr;

    return
	[startTimeValue, endTimeValue, startValueValue, endValueValue, sizeOverride] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    const float scale = sizeOverride != nullptr ? sizeOverride->getFloat () : 1.0f;
	    float startTime = startTimeValue->getFloat ();
	    float endTime = endTimeValue->getFloat ();
	    float startValue = startValueValue->getFloat () * scale;
	    float endValue = endValueValue->getFloat () * scale;

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();
		float multiplier = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue, endValue);
		p.size = p.initial.size * multiplier;

		// Update oscillator base so oscillateSize combines properly
		p.oscillateSize.base = p.size;
	    }
	};
}

OperatorFunc CParticle::createAlphaChangeOperator (const AlphaChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    return
	[startTimeValue, endTimeValue, startValueValue, endValueValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float startTime = startTimeValue->getFloat ();
	    float endTime = endTimeValue->getFloat ();
	    float startValue = startValueValue->getFloat ();
	    float endValue = endValueValue->getFloat ();

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();
		float multiplier = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue, endValue);
		p.alpha = p.initial.alpha * multiplier;

		// Update oscillator base so oscillateAlpha combines properly
		p.oscillateAlpha.base = p.alpha;
	    }
	};
}

OperatorFunc CParticle::createColorChangeOperator (const ColorChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    return
	[this, startTimeValue, endTimeValue, startValueValue, endValueValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float startTime = startTimeValue->getFloat ();
	    float endTime = endTimeValue->getFloat ();
	    glm::vec3 startValue = startValueValue->getVec3 ();
	    glm::vec3 endValue = endValueValue->getVec3 ();
	    // sub_1401D15A0 case 0xC, same shift as colorrandom
	    if (m_colorOverride.active) {
		startValue = shiftColor (startValue, m_colorOverride.shift);
		endValue = shiftColor (endValue, m_colorOverride.shift);
	    }

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();

		glm::vec3 color;
		color.r = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue.r, endValue.r);
		color.g = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue.g, endValue.g);
		color.b = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue.b, endValue.b);

		p.color = p.initial.color * color;
	    }
	};
}

OperatorFunc CParticle::createTurbulenceOperator (const TurbulenceOperator& op) {
    DynamicValue* scaleValue = op.scale->value.get ();
    DynamicValue* speedMinValue = op.speedMin->value.get ();
    DynamicValue* speedMaxValue = op.speedMax->value.get ();
    DynamicValue* maskValue = op.mask->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();
    DynamicValue* audioModeValue = op.audioProcessingMode->value.get ();
    DynamicValue* audioBoundsValue = op.audioProcessingBounds->value.get ();
    DynamicValue* audioExponentValue = op.audioProcessingExponent->value.get ();
    DynamicValue* audioStartValue = op.audioProcessingFrequencyStart->value.get ();
    DynamicValue* audioEndValue = op.audioProcessingFrequencyEnd->value.get ();

    this->m_usesParticleSeed = true;

    // same formula as wallpaper64.exe (sub_1401C9F60), phasemin is parsed there but never used
    return [this, scaleValue, speedMinValue, speedMaxValue, maskValue, phaseMinValue, phaseMaxValue,
	    speedOverride, audioModeValue, audioBoundsValue, audioExponentValue, audioStartValue, audioEndValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&,
	       float, float dt
	   ) {
	const float audio = sampleAudio (
	    audioModeValue->getInt (), audioBoundsValue->getVec2 (), audioExponentValue->getFloat (),
	    audioStartValue->getInt (), audioEndValue->getInt ()
	);
	const float speedMin = speedMinValue->getFloat () * speedOverride->getFloat () * audio;
	const float speedRange = speedMaxValue->getFloat () * speedOverride->getFloat () * audio - speedMin;
	const float phaseRange = phaseMaxValue->getFloat () - phaseMinValue->getFloat ();
	const float scale = scaleValue->getFloat ();
	const glm::vec3 mask = maskValue->getVec3 ();

	for (size_t i = 0; i < count; ++i) {
	    ParticleInstance& p = particles[i];
	    if (!p.alive) {
		continue;
	    }

	    // WE adds the scene's camera path fade times timescale here, 0 outside of fades, so timescale does nothing
	    const float phase = p.seed * phaseRange;
	    // WE's particle space is y-up
	    const float x = scale * (p.position.x + phase);
	    const float y = scale * (-p.position.y + phase);
	    const float z = scale * (p.position.z + phase);
	    const float step = (p.seed * speedRange + speedMin) * dt;

	    glm::vec3 delta (0.0f);

	    if (mask.x != 0.0f) {
		delta.x = simplexNoise3D (x, y, z) * mask.x;
	    }
	    if (mask.y != 0.0f) {
		delta.y = simplexNoise3D (z, x, y) * mask.y;
	    }
	    if (mask.z != 0.0f) {
		delta.z = simplexNoise3D (y, z, x) * mask.z;
	    }

	    p.velocity += glm::vec3 (delta.x, -delta.y, delta.z) * step;
	}
    };
}

OperatorFunc CParticle::createVortexOperator (const VortexOperator& op) {
    int controlPoint = op.controlPoint;
    int flags = op.flags;
    DynamicValue* axisValue = op.axis->value.get ();
    DynamicValue* offsetValue = op.offset->value.get ();
    DynamicValue* distanceInnerValue = op.distanceInner->value.get ();
    DynamicValue* distanceOuterValue = op.distanceOuter->value.get ();
    DynamicValue* speedInnerValue = op.speedInner->value.get ();
    DynamicValue* speedOuterValue = op.speedOuter->value.get ();
    DynamicValue* centerForceValue = op.centerForce->value.get ();
    DynamicValue* ringRadiusValue = op.ringRadius->value.get ();
    DynamicValue* ringWidthValue = op.ringWidth->value.get ();
    DynamicValue* ringPullDistanceValue = op.ringPullDistance->value.get ();
    DynamicValue* ringPullForceValue = op.ringPullForce->value.get ();
    DynamicValue* audioModeValue = op.audioProcessingMode->value.get ();
    DynamicValue* audioBoundsValue = op.audioProcessingBounds->value.get ();
    DynamicValue* audioExponentValue = op.audioProcessingExponent->value.get ();
    DynamicValue* audioStartValue = op.audioProcessingFrequencyStart->value.get ();
    DynamicValue* audioEndValue = op.audioProcessingFrequencyEnd->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    bool infiniteAxis = (flags & 1) != 0;
    bool maintainDistance = (flags & 2) != 0;
    bool ringShape = (flags & 4) != 0;

    return [controlPoint, axisValue, offsetValue, distanceInnerValue, distanceOuterValue, speedInnerValue,
	    speedOuterValue, centerForceValue, ringRadiusValue, ringWidthValue, ringPullDistanceValue,
	    ringPullForceValue, audioModeValue, audioBoundsValue, audioExponentValue, audioStartValue, audioEndValue,
	    infiniteAxis, maintainDistance, ringShape, speedOverride, this] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float, float dt
	   ) {
	const float audioResponse = sampleAudio (
	    audioModeValue->getInt (), audioBoundsValue->getVec2 (), audioExponentValue->getFloat (),
	    audioStartValue->getInt (), audioEndValue->getInt ()
	);

	if (audioResponse <= 0.0f) {
	    return;
	}

	glm::vec3 axis = axisValue->getVec3 ();
	glm::vec3 offset = offsetValue->getVec3 ();
	float distanceInner = distanceInnerValue->getFloat ();
	float distanceOuter = distanceOuterValue->getFloat ();
	float speedInner = speedInnerValue->getFloat ();
	float speedOuter = speedOuterValue->getFloat ();
	float centerForce = centerForceValue->getFloat ();
	float ringRadius = ringRadiusValue->getFloat ();
	float ringWidth = ringWidthValue->getFloat ();
	float ringPullDistance = ringPullDistanceValue->getFloat ();
	float ringPullForce = ringPullForceValue->getFloat ();

	speedInner *= audioResponse;
	speedOuter *= audioResponse;

	glm::vec3 center = glm::vec3 (0.0f);
	if (controlPoint >= 0 && controlPoint < static_cast<int> (controlPoints.size ())) {
	    center = controlPoints[controlPoint].position + offset;
	} else {
	    center = offset;
	}

	if (glm::length (axis) > 0.0f) {
	    axis = glm::normalize (axis);
	} else {
	    axis = glm::vec3 (0.0f, 0.0f, 1.0f);
	}

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }

	    glm::vec3 toParticle = p.position - center;

	    // infiniteAxis: project onto the plane perpendicular to axis (cylinder shape);
	    // otherwise use full 3D distance (sphere shape)
	    float axialDistance = 0.0f;
	    glm::vec3 radialVector = toParticle;
	    if (infiniteAxis) {
		axialDistance = glm::dot (toParticle, axis);
		radialVector = toParticle - axis * axialDistance;
	    }

	    float distance = glm::length (radialVector);

	    glm::vec3 tangent = glm::cross (axis, radialVector);
	    if (glm::length (tangent) > 0.001f) {
		tangent = glm::normalize (tangent);
	    } else {
		continue; // particle is on the axis
	    }

	    float speed = 0.0f;
	    glm::vec3 radialForce = glm::vec3 (0.0f);

	    if (ringShape) {
		// Ring mode: hollow center with ring-shaped influence zone
		float ringInner = ringRadius - ringWidth * 0.5f;
		float ringOuter = ringRadius + ringWidth * 0.5f;

		if (distance < ringInner) {
		    // Inside the ring's hollow center - no spin, but may be pulled outward
		    speed = 0.0f;
		} else if (distance <= ringOuter) {
		    // Inside the ring - full effect
		    float t = (distance - ringInner) / ringWidth;
		    speed = glm::mix (speedInner, speedOuter, t);
		} else if (distance <= ringOuter + ringPullDistance) {
		    // Outside ring but within pull distance - attract toward ring
		    float pullT = (distance - ringOuter) / ringPullDistance;
		    speed = speedOuter * (1.0f - pullT);
		    if (distance > 0.001f) {
			glm::vec3 towardRing = -glm::normalize (radialVector);
			radialForce = towardRing * ringPullForce * pullT;
		    }
		} else {
		    // Too far from ring - no effect
		    speed = 0.0f;
		}
	    } else {
		// Standard vortex mode
		float disMid = distanceOuter - distanceInner + 0.1f;

		if (disMid < 0 || distance < distanceInner) {
		    speed = speedInner;
		} else if (distance > distanceOuter) {
		    speed = speedOuter;
		} else {
		    float t = (distance - distanceInner) / disMid;
		    speed = glm::mix (speedInner, speedOuter, t);
		}
	    }

	    p.velocity += tangent * speed * dt * speedOverride->getFloat ();

	    p.velocity += radialForce * dt * speedOverride->getFloat ();

	    if (maintainDistance && distance > 0.001f) {
		glm::vec3 towardCenter = -glm::normalize (radialVector);
		p.velocity += towardCenter * centerForce * dt * speedOverride->getFloat ();
	    }
	}
    };
}

OperatorFunc CParticle::createControlPointAttractOperator (const ControlPointAttractOperator& op) {
    int controlPoint = op.controlPoint;
    DynamicValue* originValue = op.origin->value.get ();
    DynamicValue* scaleValue = op.scale->value.get ();
    DynamicValue* thresholdValue = op.threshold->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    return [controlPoint, originValue, scaleValue, thresholdValue, speedOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float currentTime, float dt
	   ) {
	glm::vec3 origin = originValue->getVec3 ();
	float scale = scaleValue->getFloat ();
	float threshold = thresholdValue->getFloat () / 2.0f;

	if (controlPoint < 0 || controlPoint >= static_cast<int> (controlPoints.size ())) {
	    return;
	}

	glm::vec3 center = controlPoints[controlPoint].position + origin;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }

	    glm::vec3 toCenter = center - p.position;
	    float distance = glm::length (toCenter);

	    if (distance > 0.001f && distance < threshold) {
		glm::vec3 direction = toCenter / distance;
		glm::vec3 forceVec = direction * scale * dt;
		p.velocity += forceVec * speedOverride->getFloat ();
	    }
	}
    };
}

OperatorFunc CParticle::createOscillateAlphaOperator (const OscillateAlphaOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();

    return
	[this, freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float freqMin = freqMinValue->getFloat ();
	    float freqMax = freqMaxValue->getFloat ();
	    float scaleMin = scaleMinValue->getFloat ();
	    float scaleMax = scaleMaxValue->getFloat ();
	    float phaseMin = phaseMinValue->getFloat ();
	    float phaseMax = phaseMaxValue->getFloat ();

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];

		// Initialize per-particle oscillator values on first use
		if (!p.oscillateAlpha.initialized) {
		    p.oscillateAlpha.frequency = WallpaperEngine::Maths::randomFloat (m_rng, freqMin, freqMax);
		    p.oscillateAlpha.scale = WallpaperEngine::Maths::randomFloat (m_rng, scaleMin, scaleMax);
		    p.oscillateAlpha.phase
			= WallpaperEngine::Maths::randomFloat (m_rng, phaseMin, phaseMax + 2.0f * glm::pi<float> ());
		    p.oscillateAlpha.base = p.alpha;
		    p.oscillateAlpha.initialized = true;
		}

		// Cosine wave interpolating between scaleMin and scaleMax
		float w = p.oscillateAlpha.frequency;
		float t = p.age;
		float cosVal = (std::cos (w * t + p.oscillateAlpha.phase) + 1.0f) * 0.5f;
		float multiplier = glm::mix (scaleMin, scaleMax, cosVal);

		// Apply to base value (alphafade updates base each frame if present)
		p.alpha = p.oscillateAlpha.base * multiplier;
	    }
	};
}

OperatorFunc CParticle::createOscillateSizeOperator (const OscillateSizeOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();

    return
	[this, freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float freqMin = freqMinValue->getFloat ();
	    float freqMax = freqMaxValue->getFloat ();
	    float scaleMin = scaleMinValue->getFloat ();
	    float scaleMax = scaleMaxValue->getFloat ();
	    float phaseMin = phaseMinValue->getFloat ();
	    float phaseMax = phaseMaxValue->getFloat ();

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];

		// Initialize per-particle oscillator values on first use
		if (!p.oscillateSize.initialized) {
		    p.oscillateSize.frequency = WallpaperEngine::Maths::randomFloat (m_rng, freqMin, freqMax);
		    p.oscillateSize.scale = WallpaperEngine::Maths::randomFloat (m_rng, scaleMin, scaleMax);
		    p.oscillateSize.phase
			= WallpaperEngine::Maths::randomFloat (m_rng, phaseMin, phaseMax + 2.0f * glm::pi<float> ());
		    p.oscillateSize.base = p.size;
		    p.oscillateSize.initialized = true;
		}

		// Cosine wave interpolating between scaleMin and scaleMax
		float w = p.oscillateSize.frequency;
		float t = p.age;
		float cosVal = (std::cos (w * t + p.oscillateSize.phase) + 1.0f) * 0.5f;
		float multiplier = glm::mix (scaleMin, scaleMax, cosVal);

		// Apply to base value (sizeChange updates base each frame if present)
		p.size = p.oscillateSize.base * multiplier;
	    }
	};
}

OperatorFunc CParticle::createOscillatePositionOperator (const OscillatePositionOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    DynamicValue* maskValue = op.mask->value.get ();
    DynamicValue* speedOverride = m_particle.instanceOverride.speed->value.get ();

    return [this, freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue, maskValue,
	    speedOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	float freqMin = freqMinValue->getFloat ();
	float freqMax = freqMaxValue->getFloat ();
	float scaleMin = scaleMinValue->getFloat ();
	float scaleMax = scaleMaxValue->getFloat ();
	float phaseMin = phaseMinValue->getFloat ();
	float phaseMax = phaseMaxValue->getFloat ();
	glm::vec3 mask = maskValue->getVec3 ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];

	    // Initialize per-particle oscillator values on first use (per axis)
	    if (!p.oscillatePosition.initialized) {
		for (int axis = 0; axis < 3; axis++) {
		    p.oscillatePosition.frequency[axis] = WallpaperEngine::Maths::randomFloat (m_rng, freqMin, freqMax);
		    p.oscillatePosition.scale[axis] = WallpaperEngine::Maths::randomFloat (m_rng, scaleMin, scaleMax);
		    p.oscillatePosition.phase[axis]
			= WallpaperEngine::Maths::randomFloat (m_rng, phaseMin, phaseMax + 2.0f * glm::pi<float> ());
		}
		p.oscillatePosition.initialized = true;
	    }

	    float t = p.age;
	    glm::vec3 delta (0.0f);

	    for (int axis = 0; axis < 3; axis++) {
		float w = 2.0f * glm::pi<float> () * p.oscillatePosition.frequency[axis] / (2.0f * glm::pi<float> ());
		// Derivative of cos is -sin; multiplied by dt for position change
		float move
		    = -p.oscillatePosition.scale[axis] * w * std::sin (w * t + p.oscillatePosition.phase[axis]) * dt;
		// Apply mask as bias multiplier for this axis
		delta[axis] = move * mask[axis] * speedOverride->getFloat ();
	    }

	    p.position += delta;
	}
    };
}

// ========== 2.7 COMPONENTS ==========

namespace {
/** wallpaper64.exe works in a y-up particle space, the particles here live in the same space mirrored on y */
glm::vec3 flipY (glm::vec3 value) {
    value.y = -value.y;
    return value;
}

/** A blend window as sub_1401C2A40 stores it, active only when it changes anything (the loader's blended tags) */
struct BlendWindow {
    float inStart;
    float inScale;
    float outEnd;
    float outScale;
    bool active;
};

BlendWindow makeBlendWindow (const ParticleBlendWindow& window) {
    const float inStart = std::min (window.inStart, window.inEnd - 0.0001f);
    const float outEnd = window.outEnd <= window.outStart + 0.0001f ? window.outStart + 0.0001f : window.outEnd;
    const float inLength = window.inEnd - inStart;
    const float outLength = outEnd - window.outStart;
    const bool active = (window.inEnd > 0.01f || window.outStart < 0.99f)
	&& (window.outStart - window.inEnd > 0.01f || inLength > 0.01f || outLength > 0.01f);

    return { inStart, 1.0f / inLength, outEnd, 1.0f / outLength, active };
}

/** sub_14022A530 */
float blendWeight (const BlendWindow& window, const ParticleInstance& p) {
    const float life = p.age / p.lifetime;
    return std::clamp ((window.outEnd - life) * window.outScale, 0.0f, 1.0f)
	* std::clamp ((life - window.inStart) * window.inScale, 0.0f, 1.0f);
}

/** 0.0 up to the zero range wallpaper64.exe replaces with FLT_EPSILON */
glm::vec3 remapRange (const glm::vec3& min, const glm::vec3& max) {
    glm::vec3 range = max - min;
    for (int i = 0; i < 3; i++) {
	if (range[i] == 0.0f) {
	    range[i] = 1.1920929e-7f;
	}
    }
    return range;
}

bool remapIsVector (ParticleRemapValue value) { return static_cast<int> (value) > 12; }

glm::vec3 remapSelectComponent (const glm::vec3& value, ParticleRemapComponent component) {
    switch (component) {
	case ParticleRemapComponent::X: return glm::vec3 (value.x);
	case ParticleRemapComponent::Y: return glm::vec3 (value.y);
	case ParticleRemapComponent::Z: return glm::vec3 (value.z);
	case ParticleRemapComponent::Sum: return glm::vec3 ((value.y + value.x) + value.z);
	case ParticleRemapComponent::Average: return glm::vec3 (((value.y + value.x) + value.z) * 0.33333334f);
	case ParticleRemapComponent::Max: return glm::vec3 (std::fmax (std::fmax (value.x, value.y), value.z));
	case ParticleRemapComponent::Min: return glm::vec3 (std::fmin (std::fmin (value.x, value.y), value.z));
	default: return value;
    }
}

float remapApply (ParticleRemapOperation operation, float current, float value) {
    switch (operation) {
	case ParticleRemapOperation::Remap: return value;
	case ParticleRemapOperation::Multiply: return value * current;
	case ParticleRemapOperation::Add: return value + current;
	case ParticleRemapOperation::Subtract: return current - value;
	default: return current;
    }
}

/** The transform functions of sub_14023FBC0 case 19, seed is the particle's random as integer bits */
float remapTransformOperator (
    ParticleRemapTransform transform, float value, float scale, int octaves, float fbmAmplitude, int32_t seed
) {
    switch (transform) {
	case ParticleRemapTransform::Sine:
	    return std::sin (value * (scale * glm::pi<float> ()) - glm::half_pi<float> ()) * 0.5f + 0.5f;
	case ParticleRemapTransform::Square: {
	    const float scaled = value * scale;
	    return std::nearbyint (scaled - std::trunc (scaled)) + (scaled < 0.0f ? 1.0f : 0.0f);
	}
	case ParticleRemapTransform::Saw: {
	    const float scaled = value * scale;
	    return (scaled - std::trunc (scaled)) + (value < 0.0f ? 1.0f : 0.0f);
	}
	case ParticleRemapTransform::Triangle: {
	    const float scaled = std::fabs (value * scale);
	    return 1.0f - std::fabs ((scaled - std::trunc (scaled)) * 2.0f - 1.0f);
	}
	case ParticleRemapTransform::SimplexNoise: return hashedNoise2D (seed, value * scale, 0.0f) * 0.5f + 0.5f;
	case ParticleRemapTransform::FbmNoise:
	    return hashedNoiseFbm (octaves, fbmAmplitude, seed, value * scale, 0.0f) * 0.5f + 0.5f;
	default: return value;
    }
}

/** The transform functions of sub_14023B340 case 15: floor instead of trunc, 1D noise without a seed */
float remapTransformInitial (ParticleRemapTransform transform, float value, float scale, int octaves) {
    switch (transform) {
	case ParticleRemapTransform::Sine:
	    return std::sin ((value * glm::pi<float> ()) * scale - glm::half_pi<float> ()) * 0.5f + 0.5f;
	case ParticleRemapTransform::Square: {
	    const float scaled = value * scale;
	    return std::round (scaled - std::floor (scaled)) + (scaled < 0.0f ? 1.0f : 0.0f);
	}
	case ParticleRemapTransform::Saw: {
	    const float scaled = value * scale;
	    return (scaled - std::floor (scaled)) + (value < 0.0f ? 1.0f : 0.0f);
	}
	case ParticleRemapTransform::Triangle: {
	    const float scaled = std::fabs (value * scale);
	    return 1.0f - std::fabs ((scaled - std::floor (scaled)) * 2.0f - 1.0f);
	}
	case ParticleRemapTransform::SimplexNoise: return simplexNoise1D (value * scale) * 0.5f + 0.5f;
	case ParticleRemapTransform::FbmNoise: return simplexFbm1D (value, scale, octaves) * 0.5f + 0.5f;
	default: return value;
    }
}

int32_t particleSeedBits (const ParticleInstance& p) {
    int32_t bits;
    std::memcpy (&bits, &p.seed, sizeof (bits));
    return bits;
}

/** The engine's g_Daytime as sub_140110630 fills it: local time as a fraction of the day, milliseconds included */
float dayFraction () {
    const auto now = std::chrono::system_clock::now ();
    const std::time_t seconds = std::chrono::system_clock::to_time_t (now);
    const auto milliseconds
	= std::chrono::duration_cast<std::chrono::milliseconds> (now.time_since_epoch ()).count () % 1000;
    std::tm local {};
    localtime_r (&seconds, &local);

    const double fraction = local.tm_min * (1.0 / 1440.0) + local.tm_hour * (1.0 / 24.0)
	+ local.tm_sec * (1.0 / 86400.0) + static_cast<double> (milliseconds) * (1.0 / 86400000.0);
    return static_cast<float> (fraction);
}
} // namespace

glm::vec3 CParticle::controlPointWE (int index) const { return flipY (m_controlPoints[index].position); }

float CParticle::frameScaledDelta (float dt) const {
    const float ratio = m_frameDelta > 0.0f ? std::min (1.0f, 0.025f / m_frameDelta) : 1.0f;
    return std::pow (ratio, 0.7f) * dt;
}

float CParticle::layerTime () const {
    const CParticle* root = this;
    while (root->m_parent != nullptr) {
	root = root->m_parent;
    }
    return root->m_layerTime;
}

glm::vec3 CParticle::remapLayerOrigin () const {
    const CParticle* root = this;
    while (root->m_parent != nullptr) {
	root = root->m_parent;
    }

    // the translation of the layer's matrix, in WE's scene space (y up, from the bottom left) for 2D scenes
    const glm::vec3 translation (root->objectMatrix ()[3]);
    if (getScene ().getCamera ().isPerspective ()) {
	return translation;
    }
    const float width = static_cast<float> (getScene ().getWidth ());
    const float height = static_cast<float> (getScene ().getHeight ());
    return { translation.x + width / 2.0f, height / 2.0f - translation.y, translation.z };
}

InitializerFunc CParticle::createHsvColorRandomInitializer (const HsvColorRandomInitializer& init) {
    DynamicValue* hueMinValue = init.hueMin->value.get ();
    DynamicValue* hueMaxValue = init.hueMax->value.get ();
    DynamicValue* saturationMinValue = init.saturationMin->value.get ();
    DynamicValue* saturationMaxValue = init.saturationMax->value.get ();
    DynamicValue* valueMinValue = init.valueMin->value.get ();
    DynamicValue* valueMaxValue = init.valueMax->value.get ();
    const int steps = init.hueSteps;

    // wallpaper64.exe sub_14023B340 case 4, the hue step from sub_1401C5490
    return [this, hueMinValue, hueMaxValue, saturationMinValue, saturationMaxValue, valueMinValue, valueMaxValue,
	    steps] (ParticleInstance& p) {
	float hueMin = hueMinValue->getFloat ();
	float hueSpan = 0.0f;
	float step = 0.0f;
	if (static_cast<float> (steps) > 1.0f) {
	    const float span = hueMaxValue->getFloat () - hueMin;
	    // a full circle has as many steps as hues, anything shorter ends on huemax
	    float divisions = static_cast<float> (steps) - 1.0f;
	    if (std::fabs (std::fmod (span, 1.0f)) < 0.0027777778f) {
		divisions += 1.0f;
	    }
	    hueSpan = span != 0.0f ? span : 1.0f;
	    step = hueSpan / divisions;
	}

	float saturationMin = saturationMinValue->getFloat ();
	float saturationSpan = saturationMaxValue->getFloat () - saturationMin;
	float valueMin = valueMinValue->getFloat ();
	float valueSpan = valueMaxValue->getFloat () - valueMin;

	// sub_1401D15A0 case 0xD: the ranges get centered on the override color
	if (m_colorOverride.active) {
	    const glm::vec3& target = m_colorOverride.hsv;
	    const auto center = [] (float goal, float& min, float& span) {
		min = std::clamp (goal - (span * 0.5f + min) + min, 0.0f, 1.0f);
		if (min + span > 1.0f) {
		    span = 1.0f - std::fmin (min, 1.0f);
		}
	    };
	    center (target.y, saturationMin, saturationSpan);
	    center (target.z, valueMin, valueSpan);
	    hueMin = target.x - (hueSpan * 0.5f + hueMin) + hueMin;
	}

	// rand () / 32767 can reach steps + 1, clamped back
	const int draw = std::uniform_int_distribution<int> (0, 32767) (m_rng);
	int index = static_cast<int> ((static_cast<float> (draw) / 32767.0f) * static_cast<float> (steps + 1) + 0.0f);
	index = std::max (0, std::min (steps, index));

	const float hue = static_cast<float> (index) * step + hueMin;
	const float saturation = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * saturationSpan + saturationMin;
	const float value = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * valueSpan + valueMin;

	p.color *= WallpaperEngine::Maths::hsvToRgb ({ hue, saturation, value });
	p.initial.color = p.color;
    };
}

InitializerFunc CParticle::createColorListInitializer (const ColorListInitializer& init) {
    // sub_1401C5490 keeps the colors as HSV, an empty list is one pure red
    std::vector<glm::vec3> colors;
    for (const auto& color : init.colors) {
	colors.push_back (WallpaperEngine::Maths::rgbToHsv (color));
    }
    if (colors.empty ()) {
	colors.emplace_back (0.0f, 1.0f, 1.0f);
    }

    DynamicValue* hueNoiseValue = init.hueNoise->value.get ();
    DynamicValue* saturationNoiseValue = init.saturationNoise->value.get ();
    DynamicValue* valueNoiseValue = init.valueNoise->value.get ();

    // sub_14023B340 case 5: a random entry, each channel randomized within its noise
    return [this, colors, hueNoiseValue, saturationNoiseValue, valueNoiseValue] (ParticleInstance& p) {
	const auto& picked
	    = colors[std::uniform_int_distribution<size_t> (0, colors.size () - 1) (m_rng)];
	const glm::vec3 noise (
	    hueNoiseValue->getFloat (), saturationNoiseValue->getFloat (), valueNoiseValue->getFloat ()
	);

	// sub_1401D15A0 case 0xE: an active override color moves every entry by its distance to the first one
	const glm::vec3 offset = m_colorOverride.active ? m_colorOverride.hsv - colors.front () : glm::vec3 (0.0f);

	glm::vec3 hsv;
	for (int i = 0; i < 3; i++) {
	    const float low = std::max (picked[i] - noise[i], 0.0f);
	    const float high = std::min (noise[i] + picked[i], 1.0f);
	    hsv[i] = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * (high - low) + low + offset[i];
	}
	hsv.x -= std::floor (hsv.x);
	hsv.y = std::clamp (hsv.y, 0.0f, 1.0f);
	hsv.z = std::clamp (hsv.z, 0.0f, 1.0f);

	p.color *= WallpaperEngine::Maths::hsvToRgb (hsv);
	p.initial.color = p.color;
    };
}

InitializerFunc CParticle::createPositionOffsetRandomInitializer (const PositionOffsetRandomInitializer& init) {
    DynamicValue* directionsValue = init.directions ? init.directions->value.get () : nullptr;
    DynamicValue* signValue = init.sign->value.get ();
    DynamicValue* scaleValue = init.scale ? init.scale->value.get () : nullptr;
    DynamicValue* distanceValue = init.distance ? init.distance->value.get () : nullptr;
    DynamicValue* timeScaleValue = init.timeScale->value.get ();
    const int octaves = init.octaves;

    // sub_14023B340 case 11, defaults from sub_1401BB660
    return [this, directionsValue, signValue, scaleValue, distanceValue, timeScaleValue,
	    octaves] (ParticleInstance& p) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const glm::vec3 directions
	    = directionsValue != nullptr ? directionsValue->getVec3 () : (flat ? glm::vec3 (1, 1, 0) : glm::vec3 (1));
	const float scale = scaleValue != nullptr ? scaleValue->getFloat () : (flat ? 0.001f : 1.0f);
	const float distance = distanceValue != nullptr ? distanceValue->getFloat () : (flat ? 100.0f : 0.1f);
	const glm::vec3 sign = signValue->getVec3 ();
	// the renderer's scene clock is the time axis of the noise
	const float time = timeScaleValue->getFloat () * getScene ().getSceneClock ();
	const glm::vec3 position = flipY (p.position);

	const auto fbm = [octaves] (float x, float y) {
	    float sum = 0.0f;
	    float total = 0.0f;
	    float amplitude = 1.0f;
	    float frequency = 1.0f;
	    for (int octave = 0; octave < octaves; octave++) {
		const float noise = simplexNoise2D (frequency * x, frequency * y) * amplitude;
		total += amplitude;
		amplitude *= 0.5f;
		frequency += frequency;
		sum += noise;
	    }
	    return sum / total;
	};

	glm::vec3 offset (
	    fbm (position.x * scale, time) * directions.x, fbm (time, position.y * scale) * directions.y,
	    fbm (position.z * scale, -time) * directions.z
	);

	// sign pushes the offset to one side per axis
	if (glm::length (sign) > 1.1920929e-7f) {
	    const glm::vec3 absoluteSign = glm::abs (sign);
	    offset = glm::abs (offset) * sign + offset * (1.0f - absoluteSign);
	}

	p.position = flipY (offset * distance + position);
    };
}

InitializerFunc
CParticle::createMapSequenceBetweenControlPointsInitializer (const MapSequenceBetweenControlPointsInitializer& init) {
    const int start = init.controlPointStart;
    const int end = init.controlPointEnd;
    const uint32_t flags = init.flags;
    const bool mirror = init.mirror;
    const float boundsMin = init.bounds.x;
    const float boundsRange = init.bounds.y - init.bounds.x;
    DynamicValue* arcAmountValue = init.arcAmount->value.get ();
    DynamicValue* arcDirectionValue = init.arcDirection->value.get ();
    DynamicValue* sizeReductionValue = init.sizeReductionAmount->value.get ();

    // sub_1401C5490, with flag 0x10 the instance override's count takes part (sub_1401D15A0 case 4)
    float step;
    if ((flags & 0x10) != 0 && (m_particle.flags & 0x20) == 0) {
	const float countOverride = m_particle.instanceOverride.count->value->getFloat ();
	step = 1.0f / std::fmax (init.count * countOverride - 1.0f, 0.000099999997f);
    } else {
	step = 1.0f / (init.count - 1.0f <= 0.000099999997f ? 0.000099999997f : init.count - 1.0f);
    }
    float sequence = 0.0f;

    // sub_14023B340 case 14: spread along the line between two control points, one step per particle
    return [this, start, end, flags, mirror, boundsMin, boundsRange, arcAmountValue, arcDirectionValue,
	    sizeReductionValue, step, sequence] (ParticleInstance& p) mutable {
	const glm::vec3 first = m_controlPoints[start].position;
	const glm::vec3 line = m_controlPoints[end].position - first;
	const float length = std::fmax (glm::length (line), 1.1754944e-38f);
	const glm::vec3 direction = line / length;

	glm::vec3 position = p.position;
	if (m_worldSpace) {
	    position -= first;
	}
	const float along = glm::dot (position, direction);
	glm::vec3 offset = position - along * direction;
	const float at = sequence * boundsRange + boundsMin;
	const float middle = 1.0f - std::pow (std::fabs ((sequence + sequence) - 1.0f), 2.0f);

	if ((flags & 1) != 0) {
	    offset *= middle;
	}
	position = offset + ((at * direction) * length + first);
	if ((flags & 8) != 0) {
	    position += flipY (arcDirectionValue->getVec3 ()) * (middle * length * arcAmountValue->getFloat ());
	}
	p.position = position;

	if ((flags & 2) != 0) {
	    p.velocity *= middle;
	}
	if ((flags & 4) != 0) {
	    const float reduction = sizeReductionValue->getFloat ();
	    p.size *= (1.0f - reduction) + reduction * middle;
	    p.initial.size = p.size;
	}

	sequence += step;
	if (sequence > 1.0f) {
	    if (mirror) {
		step = -step;
		sequence = 1.0f - (sequence - 1.0f);
	    } else {
		sequence = 0.0f;
	    }
	} else if (sequence < 0.0f) {
	    sequence = -sequence;
	    step = -step;
	}
    };
}

glm::vec3 CParticle::remapInitialInput (const ParticleRemap& remap, ParticleInstance& p) {
    // sub_14023B340 case 15: scalars fill every component, the base values are read where the engine keeps them
    const auto controlPoint = [this] (int index) { return this->controlPointWE (index); };
    switch (remap.input) {
	case ParticleRemapValue::LifetimeFraction:
	    // the sprite frame array, only filled at spawn for randomframe (with the particle's random)
	    return glm::vec3 (m_particle.animationMode == "randomframe" ? p.seed : 0.0f);
	case ParticleRemapValue::MaxLifetime: return glm::vec3 (p.lifetime);
	case ParticleRemapValue::Size: return glm::vec3 (p.initial.size);
	case ParticleRemapValue::Opacity: return glm::vec3 (p.initial.alpha);
	case ParticleRemapValue::Speed: return glm::vec3 (glm::length (p.velocity));
	case ParticleRemapValue::Rotation: return glm::vec3 (p.rotation.z);
	case ParticleRemapValue::AngularSpeed: return glm::vec3 (m_hasAngularVelocity ? p.angularVelocity.z : 0.0f);
	case ParticleRemapValue::DistanceToControlPoint:
	    return glm::vec3 (glm::length (flipY (p.position) - controlPoint (remap.inputControlPoint0)));
	case ParticleRemapValue::PositionBetweenTwoControlPoints: {
	    // the engine reads the output control points here, same as remapvalue
	    const glm::vec3 first = controlPoint (remap.outputControlPoint0);
	    glm::vec3 line = controlPoint (remap.outputControlPoint1) - first;
	    const float length = glm::length (line);
	    if (length <= 1.1920929e-7f) {
		return glm::vec3 (0.0f);
	    }
	    line /= length;
	    return glm::vec3 (glm::dot (flipY (p.position) - first, line) / length);
	}
	case ParticleRemapValue::Runtime: return glm::vec3 (getScene ().getSceneClock ());
	case ParticleRemapValue::TimeOfDay: return glm::vec3 (dayFraction ());
	case ParticleRemapValue::ParticleSystemTime: return glm::vec3 (m_systemTime);
	case ParticleRemapValue::LayerTime: return glm::vec3 (layerTime ());
	case ParticleRemapValue::Color: return p.initial.color;
	case ParticleRemapValue::Position: return flipY (p.position);
	case ParticleRemapValue::Velocity: return flipY (p.velocity);
	case ParticleRemapValue::ControlPoint:
	case ParticleRemapValue::DeltaToControlPoint:
	case ParticleRemapValue::DirectionToControlPoint: {
	    // the engine writes zeros into the control point's translation here instead of reading it
	    m_controlPoints[remap.inputControlPoint0].position = glm::vec3 (0.0f);
	    if (remap.input == ParticleRemapValue::ControlPoint) {
		return glm::vec3 (0.0f);
	    }
	    const glm::vec3 delta = -flipY (p.position);
	    if (remap.input == ParticleRemapValue::DeltaToControlPoint) {
		return delta;
	    }
	    const float length = glm::length (delta);
	    return length != 0.0f ? delta / length : glm::vec3 (0.0f);
	}
	case ParticleRemapValue::LayerOrigin: return remapLayerOrigin ();
	default: return glm::vec3 (0.0f);
    }
}

InitializerFunc CParticle::createRemapInitialValueInitializer (const RemapInitialValueInitializer& init) {
    const ParticleRemap remap = init.remap;
    const glm::vec3 inputRange = remapRange (remap.inputRangeMin, remap.inputRangeMax);
    const glm::vec3 outputRange = remap.outputRangeMax - remap.outputRangeMin;
    if (remap.input == ParticleRemapValue::LifetimeFraction && m_particle.animationMode == "randomframe") {
	m_usesParticleSeed = true;
    }

    return [this, remap, inputRange, outputRange] (ParticleInstance& p) {
	if (remap.output == ParticleRemapValue::Unknown) {
	    return;
	}

	glm::vec3 value = remapInitialInput (remap, p);
	if (remapIsVector (remap.input)) {
	    value = remapSelectComponent (value, remap.inputComponent);
	}
	value = (value - remap.inputRangeMin) / inputRange;
	if ((remap.flags & 1) != 0) {
	    value = glm::clamp (value, 0.0f, 1.0f);
	}

	value.x = remapTransformInitial (remap.transform, value.x, remap.transformInputScale, remap.transformOctaves);
	if (remapIsVector (remap.output)) {
	    for (int i = 1; i < 3; i++) {
		value[i] = remapTransformInitial (
		    remap.transform, value[i], remap.transformInputScale, remap.transformOctaves
		);
	    }
	}

	value = value * outputRange + remap.outputRangeMin;
	if ((remap.flags & 2) != 0) {
	    value = glm::clamp (value, 0.0f, 1.0f);
	}

	const auto apply = [&remap] (float current, float target) { return remapApply (remap.operation, current, target); };
	// all three components, or the one outputcomponent names
	const auto applyVector = [&remap, &apply] (glm::vec3 current, const glm::vec3& target) {
	    switch (remap.outputComponent) {
		case ParticleRemapComponent::All:
		    for (int i = 0; i < 3; i++) {
			current[i] = apply (current[i], target[i]);
		    }
		    break;
		case ParticleRemapComponent::X: current.x = apply (current.x, target.x); break;
		case ParticleRemapComponent::Y: current.y = apply (current.y, target.y); break;
		case ParticleRemapComponent::Z: current.z = apply (current.z, target.z); break;
		default: break;
	    }
	    return current;
	};

	switch (remap.output) {
	    case ParticleRemapValue::MaxLifetime:
		p.lifetime = apply (p.lifetime, value.x);
		p.initial.lifetime = p.lifetime;
		break;
	    case ParticleRemapValue::Size:
		p.initial.size = apply (p.initial.size, value.x);
		p.size = p.initial.size;
		break;
	    case ParticleRemapValue::Opacity:
		p.initial.alpha = apply (p.initial.alpha, value.x);
		p.alpha = p.initial.alpha;
		break;
	    case ParticleRemapValue::Speed: {
		const float speed = glm::length (p.velocity);
		float scale = apply (speed, value.x);
		if (speed != 0.0f) {
		    scale /= speed;
		}
		p.velocity *= scale;
		break;
	    }
	    case ParticleRemapValue::Rotation: p.rotation.z = apply (p.rotation.z, value.x); break;
	    case ParticleRemapValue::AngularSpeed:
		if (m_hasAngularVelocity) {
		    p.angularVelocity.z = apply (p.angularVelocity.z, value.x);
		}
		break;
	    case ParticleRemapValue::DistanceToControlPoint: {
		const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		glm::vec3 offset = flipY (p.position) - point;
		const float distance = glm::length (offset);
		if (distance != 0.0f) {
		    offset /= distance;
		}
		p.position = flipY (point + offset * apply (distance, value.x));
		break;
	    }
	    case ParticleRemapValue::PositionBetweenTwoControlPoints: {
		const glm::vec3 first = controlPointWE (remap.outputControlPoint0);
		glm::vec3 line = controlPointWE (remap.outputControlPoint1) - first;
		const float length = glm::length (line);
		if (length > 1.1920929e-7f) {
		    line /= length;
		}
		const glm::vec3 relative = flipY (p.position) - first;
		const float along = glm::dot (relative, line);
		const glm::vec3 offset = relative - along * line;
		const float fraction = apply (length > 1.1920929e-7f ? along / length : along, value.x);
		p.position = flipY ((first + offset) + (line * fraction) * length);
		break;
	    }
	    case ParticleRemapValue::Color:
		p.initial.color = applyVector (p.initial.color, value);
		p.color = p.initial.color;
		break;
	    case ParticleRemapValue::Position: p.position = flipY (applyVector (flipY (p.position), value)); break;
	    case ParticleRemapValue::Velocity: p.velocity = flipY (applyVector (flipY (p.velocity), value)); break;
	    case ParticleRemapValue::ControlPoint: {
		auto& point = m_controlPoints[remap.outputControlPoint0];
		point.position = flipY (applyVector (flipY (point.position), value));
		break;
	    }
	    case ParticleRemapValue::DeltaToControlPoint: {
		const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		p.position = flipY (point - applyVector (point - flipY (p.position), value));
		break;
	    }
	    case ParticleRemapValue::DirectionToControlPoint: {
		const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		glm::vec3 direction = point - flipY (p.position);
		const float distance = glm::length (direction);
		if (distance != 0.0f) {
		    direction /= distance;
		}
		direction = applyVector (direction, value);
		const float length = glm::length (direction);
		p.position = flipY (point - (length != 0.0f ? direction / length : glm::vec3 (0.0f)) * distance);
		break;
	    }
	    default: break;
	}
    };
}

glm::vec3 CParticle::remapOperatorInput (const ParticleRemap& remap, const ParticleInstance& p) const {
    // sub_14023FBC0 case 19, scalars in x
    switch (remap.input) {
	case ParticleRemapValue::LifetimeFraction: return glm::vec3 (p.age / p.lifetime, 0.0f, 0.0f);
	case ParticleRemapValue::MaxLifetime: return glm::vec3 (p.lifetime, 0.0f, 0.0f);
	case ParticleRemapValue::Size: return glm::vec3 (p.size, 0.0f, 0.0f);
	case ParticleRemapValue::Opacity: return glm::vec3 (p.alpha, 0.0f, 0.0f);
	case ParticleRemapValue::Speed: return glm::vec3 (glm::length (p.velocity), 0.0f, 0.0f);
	case ParticleRemapValue::Rotation: return glm::vec3 (p.rotation.z, 0.0f, 0.0f);
	case ParticleRemapValue::AngularSpeed:
	    return glm::vec3 (m_hasAngularVelocity ? p.angularVelocity.z : 0.0f, 0.0f, 0.0f);
	case ParticleRemapValue::DistanceToControlPoint:
	    return glm::vec3 (glm::length (flipY (p.position) - controlPointWE (remap.inputControlPoint0)), 0.0f, 0.0f);
	case ParticleRemapValue::PositionBetweenTwoControlPoints: {
	    // the engine reads the output control points here, not the input ones
	    const glm::vec3 first = controlPointWE (remap.outputControlPoint0);
	    const glm::vec3 line = controlPointWE (remap.outputControlPoint1) - first;
	    const float lengthSquared = glm::dot (line, line);
	    return glm::vec3 (
		lengthSquared > 0.0f ? glm::dot (flipY (p.position) - first, line) / lengthSquared : 0.0f, 0.0f, 0.0f
	    );
	}
	// runtime and particlesystemtime both read the renderer's scene clock here (+304)
	case ParticleRemapValue::Runtime:
	case ParticleRemapValue::ParticleSystemTime: return glm::vec3 (getScene ().getSceneClock (), 0.0f, 0.0f);
	case ParticleRemapValue::TimeOfDay: return glm::vec3 (dayFraction (), 0.0f, 0.0f);
	case ParticleRemapValue::LayerTime: return glm::vec3 (layerTime (), 0.0f, 0.0f);
	case ParticleRemapValue::Color: return p.color;
	case ParticleRemapValue::Position: return flipY (p.position);
	case ParticleRemapValue::Velocity: return flipY (p.velocity);
	case ParticleRemapValue::ControlPoint: return controlPointWE (remap.inputControlPoint0);
	case ParticleRemapValue::DeltaToControlPoint:
	    return controlPointWE (remap.inputControlPoint0) - flipY (p.position);
	case ParticleRemapValue::DirectionToControlPoint: {
	    const glm::vec3 delta = controlPointWE (remap.inputControlPoint0) - flipY (p.position);
	    const float length = glm::length (delta);
	    return length > 0.0f ? delta / length : glm::vec3 (0.0f);
	}
	case ParticleRemapValue::LayerOrigin: return remapLayerOrigin ();
	default: return glm::vec3 (0.0f);
    }
}

OperatorFunc CParticle::createRemapValueOperator (const RemapValueOperator& op) {
    const ParticleRemap remap = op.remap;
    const BlendWindow blend = makeBlendWindow (op.blend);
    const glm::vec3 inputRange = remapRange (remap.inputRangeMin, remap.inputRangeMax);
    const glm::vec3 outputRange = remap.outputRangeMax - remap.outputRangeMin;
    const float fbmAmplitude = hashedNoiseFbmNormalizer (remap.transformOctaves);

    // sub_14023FBC0 restores size every frame, and alpha or color when a remap writes them (system flags 0x10/8)
    m_resetSizeFromBase = true;
    if (remap.output == ParticleRemapValue::Opacity) {
	m_resetAlphaFromBase = true;
    } else if (remap.output == ParticleRemapValue::Color) {
	m_resetColorFromBase = true;
    }
    if (remap.transform == ParticleRemapTransform::SimplexNoise || remap.transform == ParticleRemapTransform::FbmNoise) {
	m_usesParticleSeed = true;
    }

    // sub_14023FBC0 case 19 and its blended variant 39, which moves every value only part of the way
    return [this, remap, blend, inputRange, outputRange, fbmAmplitude] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float
	   ) {
	if (remap.input == ParticleRemapValue::Unknown || remap.output == ParticleRemapValue::Unknown) {
	    return;
	}

	const bool vectorInput = remapIsVector (remap.input);
	const bool vectorOutput = remapIsVector (remap.output);

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];

	    const glm::vec3 raw = remapOperatorInput (remap, p);
	    glm::vec3 value;
	    if (vectorInput) {
		value = (remapSelectComponent (raw, remap.inputComponent) - remap.inputRangeMin) / inputRange;
	    } else {
		value = glm::vec3 ((raw.x - remap.inputRangeMin.x) / inputRange.x);
	    }
	    if ((remap.flags & 1) != 0) {
		value = glm::clamp (value, 0.0f, 1.0f);
	    }

	    // the y and z noise use the particle's random with a few bits flipped
	    const int32_t seed = particleSeedBits (p);
	    const int32_t seeds[3] = { seed, seed ^ 0x0B3924AD, seed ^ 0x493A8E83 };
	    const int transformed = vectorOutput ? 3 : 1;
	    for (int c = 0; c < transformed; c++) {
		value[c] = remapTransformOperator (
		    remap.transform, value[c], remap.transformInputScale, remap.transformOctaves, fbmAmplitude, seeds[c]
		);
		value[c] = outputRange[c] * value[c] + remap.outputRangeMin[c];
		if ((remap.flags & 2) != 0) {
		    value[c] = std::clamp (value[c], 0.0f, 1.0f);
		}
	    }

	    const float weight = blend.active ? blendWeight (blend, p) : 1.0f;
	    const auto apply = [&remap, &blend, weight] (float current, float target) {
		const float result = remapApply (remap.operation, current, target);
		return blend.active ? (result - current) * weight + current : result;
	    };
	    // blended "remap" on all components gives every component the x value, like wallpaper64.exe's variant 39
	    const auto applyVector = [&remap, &blend, &apply] (glm::vec3 current, const glm::vec3& target, bool setQuirk) {
		switch (remap.outputComponent) {
		    case ParticleRemapComponent::All: {
			const bool useX
			    = setQuirk && blend.active && remap.operation == ParticleRemapOperation::Remap;
			for (int c = 0; c < 3; c++) {
			    current[c] = apply (current[c], useX ? target.x : target[c]);
			}
			break;
		    }
		    case ParticleRemapComponent::X: current.x = apply (current.x, target.x); break;
		    case ParticleRemapComponent::Y: current.y = apply (current.y, target.y); break;
		    case ParticleRemapComponent::Z: current.z = apply (current.z, target.z); break;
		    default: break;
		}
		return current;
	    };

	    switch (remap.output) {
		case ParticleRemapValue::MaxLifetime: p.lifetime = apply (p.lifetime, value.x); break;
		case ParticleRemapValue::Size: p.size = apply (p.size, value.x); break;
		case ParticleRemapValue::Opacity: p.alpha = apply (p.alpha, value.x); break;
		case ParticleRemapValue::Speed: {
		    const float speed = glm::length (p.velocity);
		    const float target = apply (speed, value.x);
		    p.velocity *= speed > 0.0f ? target / speed : target;
		    break;
		}
		case ParticleRemapValue::Rotation: p.rotation.z = apply (p.rotation.z, value.x); break;
		case ParticleRemapValue::AngularSpeed:
		    if (m_hasAngularVelocity) {
			p.angularVelocity.z = apply (p.angularVelocity.z, value.x);
		    }
		    break;
		case ParticleRemapValue::DistanceToControlPoint: {
		    const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		    const glm::vec3 offset = flipY (p.position) - point;
		    const float distance = glm::length (offset);
		    const glm::vec3 direction = distance != 0.0f ? offset / distance : glm::vec3 (0.0f);
		    p.position = flipY (point + direction * apply (distance, value.x));
		    break;
		}
		case ParticleRemapValue::PositionBetweenTwoControlPoints: {
		    const glm::vec3 first = controlPointWE (remap.outputControlPoint0);
		    const glm::vec3 line = controlPointWE (remap.outputControlPoint1) - first;
		    const float length = glm::length (line);
		    const glm::vec3 direction = length != 0.0f ? line / length : glm::vec3 (0.0f);
		    const glm::vec3 relative = flipY (p.position) - first;
		    const float along = glm::dot (relative, direction);
		    const glm::vec3 offset = relative - along * direction;
		    const float fraction = apply (length != 0.0f ? along / length : 0.0f, value.x);
		    p.position = flipY ((fraction * length) * direction + offset + first);
		    break;
		}
		case ParticleRemapValue::Color: p.color = applyVector (p.color, value, true); break;
		case ParticleRemapValue::Position: p.position = flipY (applyVector (flipY (p.position), value, true)); break;
		case ParticleRemapValue::Velocity: p.velocity = flipY (applyVector (flipY (p.velocity), value, true)); break;
		case ParticleRemapValue::ControlPoint: {
		    // written per particle, the last one wins
		    auto& point = m_controlPoints[remap.outputControlPoint0];
		    point.position = flipY (applyVector (flipY (point.position), value, false));
		    break;
		}
		case ParticleRemapValue::DeltaToControlPoint: {
		    const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		    p.position = flipY (point - applyVector (point - flipY (p.position), value, false));
		    break;
		}
		case ParticleRemapValue::DirectionToControlPoint: {
		    const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		    const glm::vec3 delta = point - flipY (p.position);
		    const float distance = glm::length (delta);
		    const glm::vec3 direction
			= applyVector (distance > 0.0f ? delta / distance : glm::vec3 (0.0f), value, false);
		    const float length = glm::length (direction);
		    p.position = flipY (point - (length > 0.0f ? direction / length : glm::vec3 (0.0f)) * distance);
		    break;
		}
		default: break;
	    }
	}
    };
}

OperatorFunc CParticle::createCapVelocityOperator (const CapVelocityOperator& op) {
    DynamicValue* maxSpeedValue = op.maxSpeed ? op.maxSpeed->value.get () : nullptr;
    const BlendWindow blend = makeBlendWindow (op.blend);

    // sub_14023FBC0 case 18 and its blended variant 38, maxspeed defaults from sub_1401BFAB0
    return [this, maxSpeedValue, blend] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float
	   ) {
	const float maxSpeed = maxSpeedValue != nullptr ? maxSpeedValue->getFloat ()
						     : (getScene ().getCamera ().isPerspective () ? 1.0f : 100.0f);

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float speed = glm::length (p.velocity);
	    if (speed == 0.0f) {
		continue;
	    }
	    const float ratio = maxSpeed / speed;
	    p.velocity *= blend.active ? std::min (0.0f, ratio - 1.0f) * blendWeight (blend, p) + 1.0f
				       : std::min (1.0f, ratio);
	}
    };
}

OperatorFunc CParticle::createBoidsOperator (const BoidsOperator& op) {
    DynamicValue* separationThresholdValue = op.separationThreshold ? op.separationThreshold->value.get () : nullptr;
    DynamicValue* neighborThresholdValue = op.neighborThreshold ? op.neighborThreshold->value.get () : nullptr;
    DynamicValue* maxSpeedValue = op.maxSpeed ? op.maxSpeed->value.get () : nullptr;
    DynamicValue* separationFactorValue = op.separationFactor->value.get ();
    DynamicValue* alignmentFactorValue = op.alignmentFactor->value.get ();
    DynamicValue* cohesionFactorValue = op.cohesionFactor->value.get ();
    const uint32_t flags = op.flags;

    // sub_14023FBC0 case 17, defaults from sub_1401BF700. The engine walks its pool four slots at a time and only
    // every stride-th block of them (and of their neighbors) per frame, rotating with the frame count, the forces
    // grow by the stride to make up for it
    return [this, separationThresholdValue, neighborThresholdValue, maxSpeedValue, separationFactorValue,
	    alignmentFactorValue, cohesionFactorValue, flags] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const float separationThreshold
	    = separationThresholdValue != nullptr ? separationThresholdValue->getFloat () : (flat ? 20.0f : 0.02f);
	const float neighborThreshold
	    = neighborThresholdValue != nullptr ? neighborThresholdValue->getFloat () : (flat ? 50.0f : 0.2f);
	const float maxSpeed = maxSpeedValue != nullptr ? maxSpeedValue->getFloat () : (flat ? 500.0f : 1.0f);

	// every pool slot below the high water mark takes part, a dead one with its last state (m_ghosts)
	const uint32_t blocks = (m_slotExtent + 3) / 4;
	std::vector<ParticleInstance*> slots (static_cast<size_t> (blocks) * 4, nullptr);
	std::vector<uint8_t> alive (slots.size (), 0);
	for (uint32_t i = 0; i < count; i++) {
	    if (particles[i].slot < slots.size ()) {
		slots[particles[i].slot] = &particles[i];
		alive[particles[i].slot] = 1;
	    }
	}
	for (uint32_t slot = 0; slot < slots.size () && slot < m_ghostUsed.size (); slot++) {
	    if (slots[slot] == nullptr && m_ghostUsed[slot]) {
		slots[slot] = &m_ghosts[slot];
	    }
	}

	const uint32_t stride = m_slotExtent / 200 + 1;
	const float scaledDt = frameScaledDelta (dt);
	const float separationFactor = static_cast<float> (stride) * separationFactorValue->getFloat () * scaledDt;
	const float alignmentFactor = static_cast<float> (stride) * alignmentFactorValue->getFloat () * scaledDt;
	const float cohesionFactor = static_cast<float> (stride) * cohesionFactorValue->getFloat () * scaledDt;
	// the lane a neighbor block is compared on in each of the four passes (_mm_shuffle_ps 0, 147, 78, 57)
	static constexpr int lanes[4][4] = { { 0, 1, 2, 3 }, { 3, 0, 1, 2 }, { 2, 3, 0, 1 }, { 1, 2, 3, 0 } };

	for (uint32_t block = m_frameCounter % stride; block < blocks; block += stride) {
	    glm::vec3 results[4];

	    for (int lane = 0; lane < 4; lane++) {
		const ParticleInstance* self = slots[block * 4 + lane];
		if (self == nullptr) {
		    continue;
		}

		float separationCount = 0.0f;
		float neighborCount = 0.0f;
		glm::vec3 separation (0.0f);
		glm::vec3 velocitySum (0.0f);
		glm::vec3 positionSum (0.0f);

		for (uint32_t other = (block * 4 + m_frameCounter) % stride; other < blocks; other += stride) {
		    // WE's alive mask (lifetime != 0) is taken from the neighbor block unshuffled, so it belongs to this
		    // lane's slot there, not to the shuffled neighbor it gets applied to
		    if (!alive[other * 4 + lane]) {
			continue;
		    }
		    for (const auto& pass : lanes) {
			const ParticleInstance* neighbor = slots[other * 4 + pass[lane]];
			if (neighbor == nullptr) {
			    continue;
			}
			const glm::vec3 delta = self->position - neighbor->position;
			const float distanceSquared = glm::dot (delta, delta);
			const float distance = std::sqrt (distanceSquared);

			if (distanceSquared != 0.0f && distance < separationThreshold) {
			    separationCount += 1.0f;
			    separation += (separationThreshold / distance - 1.0f) * delta;
			}
			if (distance < neighborThreshold) {
			    neighborCount += 1.0f;
			    velocitySum += neighbor->velocity;
			    positionSum += neighbor->position;
			}
		    }
		}

		const float separationWeight = separationCount != 0.0f ? separationFactor / separationCount : 0.0f;
		const float average = neighborCount != 0.0f ? 1.0f / neighborCount : 0.0f;
		const float alignment = neighborCount != 0.0f ? alignmentFactor : 0.0f;
		const float cohesion = neighborCount != 0.0f ? cohesionFactor : 0.0f;
		const glm::vec3 change
		    = ((average * velocitySum - self->velocity) * alignment + separationWeight * separation)
		    + (average * positionSum - self->position) * cohesion;

		glm::vec3 velocity = self->velocity + change;
		if ((flags & 1) != 0) {
		    const float speedSquared = glm::dot (velocity, velocity);
		    if (std::max (glm::dot (self->velocity, self->velocity), maxSpeed * maxSpeed) < speedSquared) {
			velocity *= maxSpeed / std::sqrt (speedSquared);
		    }
		}
		results[lane] = velocity;
	    }

	    // the four lanes are written together, after all of them were computed
	    for (int lane = 0; lane < 4; lane++) {
		if (ParticleInstance* target = slots[block * 4 + lane]) {
		    target->velocity = results[lane];
		}
	    }
	}
    };
}

OperatorFunc CParticle::createMaintainDistanceToControlPointOperator (const MaintainDistanceToControlPointOperator& op) {
    const int controlPoint = op.controlPoint;
    DynamicValue* distanceValue = op.distance ? op.distance->value.get () : nullptr;
    DynamicValue* strengthValue = op.variableStrength->value.get ();
    const BlendWindow blend = makeBlendWindow (op.blend);

    // sub_14023FBC0 case 11 and its blended variant 33, distance defaults from sub_1401BE2A0
    return [this, controlPoint, distanceValue, strengthValue, blend] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>& points,
	       float, float dt
	   ) {
	const auto& point = points[controlPoint];
	const float distance = distanceValue != nullptr ? distanceValue->getFloat ()
						       : (getScene ().getCamera ().isPerspective () ? 1.0f : 200.0f);
	const float variableStrength = strengthValue->getFloat ();
	const float strength = variableStrength == 0.0f ? 1.0f : std::clamp (variableStrength * dt, 0.0f, 1.0f);
	// the distance is measured in the control point's own frame
	const glm::mat3 toPoint = glm::inverse (point.orientation);

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    // particles move along with the control point, then get pulled onto the sphere around it
	    const glm::vec3 moved = p.position + point.movement;
	    const glm::vec3 offset = moved - point.position;
	    const float length = glm::length (toPoint * offset);
	    if (length == 0.0f) {
		p.position = moved;
		continue;
	    }
	    float pull = (distance / length - 1.0f) * strength;
	    if (blend.active) {
		pull *= blendWeight (blend, p);
	    }
	    p.position = pull * offset + moved;
	}
    };
}

OperatorFunc
CParticle::createMaintainDistanceBetweenControlPointsOperator (const MaintainDistanceBetweenControlPointsOperator& op) {
    const int start = op.controlPointStart;
    const int end = op.controlPointEnd;
    const BlendWindow blend = makeBlendWindow (op.blend);

    // sub_14023FBC0 case 12 and its blended variant 34: whatever lay along the line between the two control points
    // last frame is moved to the same share of the line now
    return [start, end, blend] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>& points,
	       float, float
	   ) {
	const glm::vec3 first = points[start].position;
	const glm::vec3 previousFirst = first - points[start].movement;
	const glm::vec3 previousLast = points[end].position - points[end].movement;
	const glm::vec3 line = points[end].position - first;
	const glm::vec3 previousLine = previousLast - previousFirst;
	const float lengthSquared = glm::dot (line, line);
	const float previousLengthSquared = glm::dot (previousLine, previousLine);
	if (lengthSquared <= 1.4210855e-14f || previousLengthSquared <= 1.4210855e-14f) {
	    return;
	}

	const float length = std::sqrt (lengthSquared);
	const float previousLength = std::sqrt (previousLengthSquared);
	const glm::vec3 direction = line / length;
	const glm::vec3 previousDirection = previousLine / previousLength;
	const glm::vec3 shift = first - previousFirst;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float along = glm::dot (p.position - previousFirst, previousDirection);
	    const float scaled = std::clamp (along / previousLength, 0.0f, 1.0f) * length;
	    glm::vec3 change = (scaled * direction - along * previousDirection) + shift;
	    if (blend.active) {
		change *= blendWeight (blend, p);
	    }
	    p.position += change;
	}
    };
}

OperatorFunc CParticle::createReduceMovementNearControlPointOperator (const ReduceMovementNearControlPointOperator& op) {
    const int controlPoint = op.controlPoint;
    DynamicValue* innerValue = op.distanceInner ? op.distanceInner->value.get () : nullptr;
    DynamicValue* outerValue = op.distanceOuter ? op.distanceOuter->value.get () : nullptr;
    DynamicValue* reductionInnerValue = op.reductionInner->value.get ();
    DynamicValue* reductionOuterValue = op.reductionOuter->value.get ();
    const BlendWindow blend = makeBlendWindow (op.blend);

    // sub_14023FBC0 case 13 and its blended variant 35, distance defaults from sub_1401BE810
    return [this, controlPoint, innerValue, outerValue, reductionInnerValue, reductionOuterValue, blend] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>& points,
	       float, float dt
	   ) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const float inner = innerValue != nullptr ? innerValue->getFloat () : (flat ? 100.0f : 0.5f);
	const float outer = outerValue != nullptr ? outerValue->getFloat () : (flat ? 350.0f : 1.0f);
	const float reductionInner = reductionInnerValue->getFloat ();
	const float reductionOuter = reductionOuterValue->getFloat ();
	const float distanceScale = inner == outer ? 1.0f : 1.0f / (outer - inner);
	const float reductionRange = reductionInner == reductionOuter ? 1.0f : reductionOuter - reductionInner;
	const glm::vec3 center = points[controlPoint].position;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float distance = glm::length (p.position - center);
	    const float share = std::clamp ((distance - inner) * distanceScale, 0.0f, 1.0f);
	    float reduction = std::clamp ((share * reductionRange + reductionInner) * dt, 0.0f, 1.0f);
	    if (blend.active) {
		reduction *= blendWeight (blend, p);
	    }
	    p.velocity *= 1.0f - reduction;
	}
    };
}

OperatorFunc CParticle::createCollisionOperator (const CollisionOperator& op) {
    if (op.shape == ParticleCollisionShape::Model) {
	sLog.error ("Particle operator collisionmodel is not supported, it is ignored");
	return nullptr;
    }
    // sub_14023FBC0 case 23 does nothing, collisionbox only exists in the file format
    if (op.shape == ParticleCollisionShape::Box) {
	return nullptr;
    }
    if (op.shape == ParticleCollisionShape::Quad) {
	m_tracksPreviousPosition = true;
    }

    const ParticleCollisionShape shape = op.shape;
    const ParticleCollisionBehavior behavior = op.behavior;
    const uint32_t flags = op.flags;
    const int controlPoint = op.controlPoint;
    DynamicValue* bounceValue = op.bounceFactor->value.get ();
    DynamicValue* planeValue = op.plane->value.get ();
    DynamicValue* distanceValue = op.distance ? op.distance->value.get () : nullptr;
    DynamicValue* originValue = op.origin ? op.origin->value.get () : nullptr;
    DynamicValue* radiusValue = op.radius ? op.radius->value.get () : nullptr;
    DynamicValue* forwardValue = op.forward->value.get ();
    DynamicValue* sizeValue = op.size ? op.size->value.get () : nullptr;

    // sub_14023FBC0 cases 21, 22, 24 and 25 with the per behavior workers (sub_14024F5E0 and siblings), defaults
    // from sub_1401C00A0, sub_1401C0540, sub_1401C0740 and sub_1401C0870
    return [this, shape, behavior, flags, controlPoint, bounceValue, planeValue, distanceValue, originValue,
	    radiusValue, forwardValue, sizeValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>& points,
	       float, float
	   ) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const float bounce = -1.0f - bounceValue->getFloat ();
	const bool followPoint = (flags & 1) != 0;
	const auto& point = points[controlPoint];

	// pushes the particle back by depth along the normal, then the velocity rule, flag 2 also stops the spin
	const auto collide = [behavior, bounce, flags] (ParticleInstance& p, const glm::vec3& normal, float depth) {
	    if (behavior == ParticleCollisionBehavior::Delete) {
		p.age = p.lifetime;
	    } else {
		p.position -= depth * normal;
		const float along = glm::dot (p.velocity, normal);
		switch (behavior) {
		    case ParticleCollisionBehavior::Bounce: p.velocity += (along * bounce) * normal; break;
		    case ParticleCollisionBehavior::Slide: p.velocity -= along * normal; break;
		    default: p.velocity = glm::vec3 (0.0f); break;
		}
	    }
	    if ((flags & 2) != 0) {
		p.angularVelocity = glm::vec3 (0.0f);
	    }
	};

	switch (shape) {
	    case ParticleCollisionShape::Plane: {
		glm::vec3 normal = flipY (glm::normalize (planeValue->getVec3 ()));
		float distance = distanceValue != nullptr ? distanceValue->getFloat () : (flat ? -150.0f : 0.0f);
		if (followPoint) {
		    normal = point.orientation * normal;
		    distance = glm::dot (normal, point.position);
		}
		for (uint32_t i = 0; i < count; i++) {
		    const float along = glm::dot (particles[i].position, normal);
		    if (along < distance) {
			collide (particles[i], normal, along - distance);
		    }
		}
		break;
	    }
	    case ParticleCollisionShape::Sphere: {
		glm::vec3 center = originValue != nullptr ? flipY (originValue->getVec3 ())
							  : (flat ? glm::vec3 (0.0f, 200.0f, 0.0f) : glm::vec3 (0.0f));
		const float radius = radiusValue != nullptr ? radiusValue->getFloat () : (flat ? 50.0f : 1.0f);
		if (followPoint) {
		    center = point.position;
		}
		for (uint32_t i = 0; i < count; i++) {
		    const glm::vec3 offset = particles[i].position - center;
		    const float distanceSquared = glm::dot (offset, offset);
		    // at the very center the engine's normal would be NaN
		    if (distanceSquared < radius * radius && distanceSquared > 0.0f) {
			const float distance = std::sqrt (distanceSquared);
			collide (particles[i], offset / distance, distance - radius);
		    }
		}
		break;
	    }
	    case ParticleCollisionShape::Quad: {
		glm::vec3 origin = originValue != nullptr ? flipY (originValue->getVec3 ())
							  : (flat ? glm::vec3 (0.0f, 150.0f, 0.0f) : glm::vec3 (0.0f));
		const glm::vec2 halfSize
		    = (sizeValue != nullptr ? sizeValue->getVec2 () : (flat ? glm::vec2 (200.0f) : glm::vec2 (1.0f)))
		    * 0.5f;
		glm::vec3 normal = glm::normalize (flipY (planeValue->getVec3 ()));
		const glm::vec3 forward = glm::normalize (flipY (forwardValue->getVec3 ()));
		glm::vec3 right = glm::normalize (glm::cross (normal, forward));
		glm::vec3 up = glm::normalize (glm::cross (right, normal));
		if (followPoint) {
		    origin = point.position;
		    normal = point.orientation * normal;
		    up = point.orientation * up;
		    right = point.orientation * right;
		}
		// only particles crossing it from the front during this frame hit it
		for (uint32_t i = 0; i < count; i++) {
		    auto& p = particles[i];
		    const glm::vec3 offset = p.position - origin;
		    const float along = glm::dot (offset, normal);
		    if (glm::dot (p.previousPosition - origin, normal) > 0.0f && along <= 0.0f
			&& std::fabs (glm::dot (offset, up)) < halfSize.y
			&& std::fabs (glm::dot (offset, right)) < halfSize.x) {
			collide (p, normal, along * 1.05f);
		    }
		}
		break;
	    }
	    case ParticleCollisionShape::Bounds: {
		// the scene's own orthographic size, 0 for automatic and perspective ones, as a box from (0, 0) up in
		// WE's scene space, turned into this scene's space and then the system's
		const auto& projection = getScene ().getScene ().camera.projection;
		const float width = static_cast<float> (projection.width);
		const float height = static_cast<float> (projection.height);
		glm::vec3 low (0.0f);
		glm::vec3 high (width, -height, 0.0f);
		if (flat) {
		    const float sceneWidth = static_cast<float> (getScene ().getWidth ());
		    const float sceneHeight = static_cast<float> (getScene ().getHeight ());
		    low = glm::vec3 (-sceneWidth / 2.0f, sceneHeight / 2.0f, 0.0f);
		    high = glm::vec3 (width - sceneWidth / 2.0f, sceneHeight / 2.0f - height, 0.0f);
		}
		glm::vec3 normals[4] = { { 1, 0, 0 }, { 0, -1, 0 }, { -1, 0, 0 }, { 0, 1, 0 } };
		if (!m_worldSpace) {
		    const glm::mat4 toLocal = glm::inverse (m_frame);
		    for (auto& normal : normals) {
			normal = glm::mat3 (toLocal) * normal;
		    }
		    low = glm::vec3 (toLocal * glm::vec4 (low, 1.0f));
		    high = glm::vec3 (toLocal * glm::vec4 (high, 1.0f));
		}
		const float distances[4] = { glm::dot (low, normals[0]), glm::dot (low, normals[1]),
					     glm::dot (high, normals[2]), glm::dot (high, normals[3]) };

		// the last side the particle is outside of is the one it hits
		for (uint32_t i = 0; i < count; i++) {
		    auto& p = particles[i];
		    int side = -1;
		    for (int s = 0; s < 4; s++) {
			if (glm::dot (p.position, normals[s]) < distances[s]) {
			    side = s;
			}
		    }
		    if (side >= 0) {
			collide (p, normals[side], glm::dot (p.position, normals[side]) - distances[side]);
		    }
		}
		break;
	    }
	    default: break;
	}
    };
}

// ========== RENDERING ==========

void CParticle::setupPass () {
    if (!m_particle.material || !m_particle.material->material || m_particle.material->material->passes.empty ()) {
	sLog.error ("No valid material for particle ", m_particle.name);
	return;
    }

    const auto& firstPass = **m_particle.material->material->passes.begin ();

    m_passOverride = std::make_unique<ImageEffectPassOverride> ();
    m_passOverride->combos["THICKFORMAT"] = 1;
    if (m_useRopeRenderer) {
	m_passOverride->shaderOverride = "genericropeparticle";
    }
    if (m_spritesheetFrames > 0) {
	m_passOverride->combos["SPRITESHEET"] = 1;
    }
    if (m_useTrailRenderer) {
	m_passOverride->combos["TRAILRENDERER"] = 1;
    }
    if (m_useRopeRenderer && m_useTrailRenderer) {
	// sub_1401D2340 case 4. WE leaves THICKFORMAT off here and packs a single size and color per point, the
	// THICKFORMAT layout below repeats them as the end values, which the shader treats the same way
	m_passOverride->combos["TRAILSUBDIVISION"] = m_ropeSubdivision;
	if (m_ropeUVScrolling) {
	    m_passOverride->combos["TRAILSCROLLALPHA"] = 1;
	}
	if (m_trailFadeAlpha) {
	    m_passOverride->combos["TRAILFADEALPHA"] = 1;
	}
	if (m_trailFadeSize) {
	    m_passOverride->combos["TRAILFADESIZE"] = 1;
	}
    }

    // Force texture 0 to use the input (particle texture) rather than the shader's
    // default "util/white" annotation, which would override it in setupRenderTexture()
    m_passBinds = { { 0, "previous" } };

    auto refractIt = firstPass.combos.find ("REFRACT");
    m_hasRefract = refractIt != firstPass.combos.end () && refractIt->second != 0;

    m_passFBOProvider = std::make_shared<FBOProvider> (this);

    // REFRACT: create a copy FBO shadowing _rt_FullFrameBuffer. The shader reads g_Texture3
    // (= _rt_FullFrameBuffer) while we render TO the scene FBO; reading and writing the same FBO
    // is undefined behavior in OpenGL and causes black reads on NVIDIA. Placing a copy FBO under
    // the same name in our FBOProvider makes CPass resolve g_Texture3 to the copy instead - we
    // blit the scene content into it before each render.
    if (m_hasRefract) {
	auto sceneFBO = getScene ().getFBO ();
	float w = static_cast<float> (sceneFBO->getRealWidth ());
	float h = static_cast<float> (sceneFBO->getRealHeight ());
	m_refractFBO = m_passFBOProvider->create (
	    "_rt_FullFrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, { w, h }, { w, h }
	);
    }

    m_pass = new Effects::CPass (*this, m_passFBOProvider, firstPass, *m_passOverride, m_passBinds, std::nullopt);

    m_pass->setDestination (getScene ().getFBO ());
    m_pass->setInput (getTexture ());

    // Set matrix pointers - CPass will dereference these each frame
    m_pass->setModelViewProjectionMatrix (&m_mvpMatrix);
    m_pass->setModelViewProjectionMatrixInverse (&m_mvpMatrixInverse);
    m_pass->setModelMatrix (&m_modelMatrix);
    m_pass->setViewProjectionMatrix (&m_viewProjectionMatrix);

    GLint prevVAO = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVAO);

    glGenVertexArrays (1, &m_vao);
    glGenBuffers (1, &m_vbo);
    glGenBuffers (1, &m_ebo);

    glBindVertexArray (m_vao);
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);

    const GLuint program = m_pass->getProgramID ();

    if (m_useRopeRenderer) {
	// Rope vertex layout: 7 attributes, 26 floats/vertex, stride=104 bytes
	// a_PositionVec4(4) + a_TexCoordVec4(4) + a_TexCoordVec4C1(4) + a_TexCoordVec4C2(4)
	// + a_TexCoordVec4C3(4) + a_TexCoordC4(2) + a_Color(4) = 26
	const GLsizei stride = sizeof (float) * ROPE_FLOATS_PER_VERTEX;

	const GLint loc0 = glGetAttribLocation (program, "a_PositionVec4");
	const GLint loc1 = glGetAttribLocation (program, "a_TexCoordVec4");
	const GLint loc2 = glGetAttribLocation (program, "a_TexCoordVec4C1");
	const GLint loc3 = glGetAttribLocation (program, "a_TexCoordVec4C2");
	const GLint loc4 = glGetAttribLocation (program, "a_TexCoordVec4C3");
	const GLint loc5 = glGetAttribLocation (program, "a_TexCoordC4");
	const GLint loc6 = glGetAttribLocation (program, "a_Color");

	if (loc0 >= 0) {
	    glEnableVertexAttribArray (loc0);
	    glVertexAttribPointer (loc0, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 0));
	}
	if (loc1 >= 0) {
	    glEnableVertexAttribArray (loc1);
	    glVertexAttribPointer (loc1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 4));
	}
	if (loc2 >= 0) {
	    glEnableVertexAttribArray (loc2);
	    glVertexAttribPointer (loc2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 8));
	}
	if (loc3 >= 0) {
	    glEnableVertexAttribArray (loc3);
	    glVertexAttribPointer (loc3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 12));
	}
	if (loc4 >= 0) {
	    glEnableVertexAttribArray (loc4);
	    glVertexAttribPointer (loc4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 16));
	}
	if (loc5 >= 0) {
	    glEnableVertexAttribArray (loc5);
	    glVertexAttribPointer (loc5, 2, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 20));
	}
	if (loc6 >= 0) {
	    glEnableVertexAttribArray (loc6);
	    glVertexAttribPointer (loc6, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 22));
	}
    } else {
	// Sprite vertex layout: 5 attributes, 17 floats/vertex, stride=68 bytes
	// a_Position(3) + a_TexCoordVec4(4) + a_Color(4) + a_TexCoordVec4C1(4) + a_TexCoordC2(2) = 17
	const GLsizei stride = sizeof (float) * SPRITE_FLOATS_PER_VERTEX;

	const GLint loc0 = glGetAttribLocation (program, "a_Position");
	const GLint loc1 = glGetAttribLocation (program, "a_TexCoordVec4");
	const GLint loc2 = glGetAttribLocation (program, "a_Color");
	const GLint loc3 = glGetAttribLocation (program, "a_TexCoordVec4C1");
	const GLint loc4 = glGetAttribLocation (program, "a_TexCoordC2");

	if (loc0 >= 0) {
	    glEnableVertexAttribArray (loc0);
	    glVertexAttribPointer (loc0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 0));
	}
	if (loc1 >= 0) {
	    glEnableVertexAttribArray (loc1);
	    glVertexAttribPointer (loc1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 3));
	}
	if (loc2 >= 0) {
	    glEnableVertexAttribArray (loc2);
	    glVertexAttribPointer (loc2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 7));
	}
	if (loc3 >= 0) {
	    glEnableVertexAttribArray (loc3);
	    glVertexAttribPointer (loc3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 11));
	}
	if (loc4 >= 0) {
	    glEnableVertexAttribArray (loc4);
	    glVertexAttribPointer (loc4, 2, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 15));
	}
    }

    glBindVertexArray (prevVAO);

    setupGeometryCallbacks ();
    setupParticleUniforms ();
}

void CParticle::setupGeometryCallbacks () {
    m_pass->setGeometryCallback (
	// Setup attribs: save current VAO, bind particle VAO
	[this] () {
	    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &m_prevVAO);
	    glBindVertexArray (m_vao);
	},
	// Draw geometry: indexed rendering
	[this] () { glDrawElements (GL_TRIANGLES, m_activeIndexCount, GL_UNSIGNED_INT, nullptr); },
	// Cleanup: restore previous VAO
	[this] () { glBindVertexArray (m_prevVAO); }
    );
}

void CParticle::setupParticleUniforms () {
    // Add particle-specific uniforms from common_particles.h that CPass doesn't provide
    // These are pointer-based: CPass reads the current value each frame
    m_pass->addUniform ("g_ModelMatrixInverse", &m_modelMatrixInverse);
    m_pass->addUniform ("g_OrientationUp", &m_orientationUp);
    m_pass->addUniform ("g_OrientationRight", &m_orientationRight);
    m_pass->addUniform ("g_OrientationForward", &m_orientationForward);
    m_pass->addUniform ("g_ViewUp", &m_viewUp);
    m_pass->addUniform ("g_ViewRight", &m_viewRight);
    m_pass->addUniform ("g_EyePosition", &m_eyePosition);
    m_pass->addUniform ("g_RenderVar0", &m_renderVar0);
    m_pass->addUniform ("g_RenderVar1", &m_renderVar1);

    // REFRACT: set g_RefractAmount (shader default 0.05, may not be applied by CPass's parameter system)
    if (m_hasRefract) {
	m_pass->addUniform ("g_RefractAmount", &m_refractAmount);
    }
}

void CParticle::updateMatrices () {
    // m_modelMatrix comes from draw ()
    m_modelMatrixInverse = glm::inverse (m_modelMatrix);

    this->updateParticleViewProjection ();
    m_mvpMatrix = m_viewProjectionMatrix * m_modelMatrix;
    m_mvpMatrixInverse = glm::inverse (m_mvpMatrix);

    m_orientationUp = glm::vec3 (0.0f, 1.0f, 0.0f);
    m_orientationRight = glm::vec3 (1.0f, 0.0f, 0.0f);
    m_orientationForward = glm::vec3 (0.0f, 0.0f, 1.0f);
    m_viewUp = glm::vec3 (0.0f, 1.0f, 0.0f);
    m_viewRight = glm::vec3 (1.0f, 0.0f, 0.0f);

    this->updateParticleRenderVars ();
}

void CParticle::updateParticleViewProjection () {
    const auto& camera = getScene ().getCamera ();

    if (camera.isPerspective ()) {
	m_viewProjectionMatrix = camera.getPerspective () * camera.getView ();
	m_eyePosition = camera.getEye ();
    } else {
	// particle file flags 4 (sub_1402366F0) and the object's "perspective" (sub_1402222A0) both switch to the
	// perspective layer camera (sub_1401E5B60)
	const bool perspective = (m_particle.flags & 4) != 0 || m_particle.perspective->value->getBool ();
	m_viewProjectionMatrix = perspective ? camera.getPerspectiveLayerViewProjection ()
					     : camera.getProjection () * camera.getLookAt ();
	// g_EyePosition in 2D scenes is the camera position 2000 units out (end of sub_1401891A0), the trail
	// shader's ComputeParticleTrailTangents crosses the eye direction with the velocity
	const glm::vec2 eye = getScene ().getCameraEye ();
	m_eyePosition = glm::vec3 (eye.x, -eye.y, 2000.0f);
    }
}

void CParticle::updateParticleRenderVars () {
    if (m_useRopeRenderer && m_useTrailRenderer) {
	// sub_1402366F0 renderer type 4: z is how far the history timer got, w the segment count the UVs span
	const float segments = static_cast<float> (m_ropeSegments);
	const float timeOffset = 1.0f - std::max (m_trailTimer, 0.0f) / m_trailInterval;
	m_renderVar0 = m_ropeUVScrolling
	    ? glm::vec4 (segments - 1.0f, 0.0f, timeOffset, (segments - 1.0f) / this->ropeUVScale ())
	    : glm::vec4 (0.0f, 0.0f, timeOffset, segments - 0.5f);
    } else {
	m_renderVar0 = glm::vec4 (m_trailLength, m_trailMaxLength, m_trailMinLength, 0.0f);
    }

    if (m_spritesheetFrames > 0 && m_spritesheetCols > 0 && m_spritesheetRows > 0) {
	float frameWidth = 1.0f / static_cast<float> (m_spritesheetCols);
	float frameHeight = 1.0f / static_cast<float> (m_spritesheetRows);
	float textureRatio = 1.0f;
	if (const auto texture = getTexture ()) {
	    // Use atlas dimensions (resolution vec4) rather than getRealWidth/Height, which
	    // returns per-frame dimensions for animated textures - the shader needs the
	    // per-frame pixel aspect ratio: (atlasH * frameHeight) / (atlasW * frameWidth).
	    const glm::vec4* res = texture->getResolution ();
	    float w = res->x;
	    float h = res->y;
	    if (w > 0.0f) {
		textureRatio = (h * frameHeight) / (w * frameWidth);
	    }
	}
	m_renderVar1 = glm::vec4 (frameWidth, frameHeight, static_cast<float> (m_spritesheetFrames), textureRatio);
    } else {
	float textureRatio = 1.0f;
	if (const auto texture = getTexture ()) {
	    float w = static_cast<float> (texture->getRealWidth ());
	    float h = static_cast<float> (texture->getRealHeight ());
	    if (w > 0.0f) {
		textureRatio = h / w;
	    }
	}
	m_renderVar1 = glm::vec4 (0.0f, 0.0f, 0.0f, textureRatio);
    }
}

void CParticle::renderSprites () {
    if (m_particleCount == 0 || m_pass == nullptr) {
	return;
    }

    uint32_t aliveCount = 0;
    for (uint32_t i = 0; i < m_particleCount; i++) {
	if (m_particles[i].alive) {
	    aliveCount++;
	}
    }

    if (aliveCount == 0) {
	return;
    }

    // Build vertex data in WP shader layout:
    // a_Position(3) + a_TexCoordVec4(uv.x, uv.y, rotZ, size)(4) + a_Color(4)
    //   + a_TexCoordVec4C1(vel.x, vel.y, vel.z, lifetime)(4) + a_TexCoordC2(rotX, rotY)(2) = 17 floats
    uint32_t vertexIndex = 0;
    uint32_t indexOffset = 0;

    for (uint32_t i = 0; i < m_particleCount; i++) {
	const auto& p = m_particles[i];
	if (!p.alive) {
	    continue;
	}

	// Skip particles with invalid values
	if (!std::isfinite (p.position.x) || !std::isfinite (p.position.y) || !std::isfinite (p.position.z)
	    || !std::isfinite (p.size) || p.size <= 0.0f || p.size > 10000.0f) {
	    continue;
	}

	// Encode the CPU-computed frame (accounts for sequenceMultiplier and animation mode)
	// into the lifetime value the WP shader's ComputeSpriteFrame expects: it derives the
	// current frame via floor(frac(lifetime) * numFrames) and the inter-frame blend via
	// frac(lifetime * numFrames).
	float lifetime = p.getLifetimePos ();

	if (m_spritesheetFrames > 0 && p.frame >= 0.0f) {
	    if (m_particle.animationMode == "randomframe") {
		// Center within the frame to avoid floating-point edge cases
		lifetime = (p.frame + 0.5f) / static_cast<float> (m_spritesheetFrames);
	    } else {
		lifetime = p.frame / static_cast<float> (m_spritesheetFrames);
	    }
	}

	auto addVertex = [&] (float u, float v) {
	    const uint32_t base = vertexIndex * SPRITE_FLOATS_PER_VERTEX;
	    // a_Position (vec3)
	    m_vertices[base + 0] = p.position.x;
	    m_vertices[base + 1] = p.position.y;
	    m_vertices[base + 2] = p.position.z;
	    // a_TexCoordVec4 (vec4: uv.x, uv.y, rotZ, size)
	    m_vertices[base + 3] = u;
	    m_vertices[base + 4] = v;
	    m_vertices[base + 5] = p.rotation.z;
	    m_vertices[base + 6] = p.size;
	    // a_Color (vec4: r, g, b, a)
	    m_vertices[base + 7] = p.color.r;
	    m_vertices[base + 8] = p.color.g;
	    m_vertices[base + 9] = p.color.b;
	    m_vertices[base + 10] = p.alpha;
	    // a_TexCoordVec4C1 (vec4: vel.x, vel.y, vel.z, lifetime)
	    m_vertices[base + 11] = p.velocity.x;
	    m_vertices[base + 12] = p.velocity.y;
	    m_vertices[base + 13] = p.velocity.z;
	    m_vertices[base + 14] = lifetime;
	    // a_TexCoordC2 (vec2: rotX, rotY)
	    m_vertices[base + 15] = p.rotation.x;
	    m_vertices[base + 16] = p.rotation.y;
	    vertexIndex++;
	};

	uint32_t baseVertex = vertexIndex;
	addVertex (0.0f, 1.0f); // 0: Bottom-left
	addVertex (1.0f, 1.0f); // 1: Bottom-right
	addVertex (1.0f, 0.0f); // 2: Top-right
	addVertex (0.0f, 0.0f); // 3: Top-left

	m_indices[indexOffset++] = baseVertex + 0;
	m_indices[indexOffset++] = baseVertex + 1;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 3;
	m_indices[indexOffset++] = baseVertex + 0;
    }

    m_activeIndexCount = static_cast<GLsizei> (indexOffset);
    if (m_activeIndexCount == 0) {
	return;
    }

#if !NDEBUG
    std::string str = "Particles ";
    str += this->getParticle ().name + " (" + std::to_string (this->getId ()) + ", " + this->getParticle ().particleFile
	+ ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif

    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertexIndex * SPRITE_FLOATS_PER_VERTEX * sizeof (float)),
	m_vertices.data (), GL_DYNAMIC_DRAW
    );

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indexOffset * sizeof (uint32_t)), m_indices.data (),
	GL_DYNAMIC_DRAW
    );

    updateMatrices ();

    // REFRACT: blit current scene content into the copy FBO first, giving the shader a
    // snapshot of what's behind the particles without a read/write feedback loop
    if (m_hasRefract && m_refractFBO) {
	auto sceneFBO = getScene ().getFBO ();
	GLint w = static_cast<GLint> (sceneFBO->getRealWidth ());
	GLint h = static_cast<GLint> (sceneFBO->getRealHeight ());
	glBindFramebuffer (GL_READ_FRAMEBUFFER, sceneFBO->getFramebuffer ());
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_refractFBO->getFramebuffer ());
	glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // ComputeParticleTrailTangents produces a right vector with a Z component (from
    // cross(eyeDirection, velocity), where eyeDirection has an XY offset from the model
    // transform). For 2D/ortho particles at z=0, the ortho near plane sits at ndc.z=-1, so any
    // Z offset pushes vertices past it and clips half the quad. GL_DEPTH_CLAMP avoids that by
    // clamping depth instead of clipping.
    glEnable (GL_DEPTH_CLAMP);

    // CPass::render() handles: FBO binding, texture setup, uniforms, blending, draw call, cleanup
    m_pass->render ();

    glDisable (GL_DEPTH_CLAMP);

#if !NDEBUG
    glPopDebugGroup ();
#endif
}

float CParticle::ropeUVScale () const {
    return m_ropeUVScale != 0.0f ? m_ropeUVScale : 1.0f;
}

void CParticle::buildRopeTrail (uint32_t& vertexIndex, uint32_t& indexOffset) {
    // sub_1402308A0, the ropetrail vertex build without a geometry shader: every particle gets one strip through
    // its position and its history, a quad per segment whether that history is filled yet or not
    const int segments = m_ropeSegments;
    const float uvScaleInverse = 1.0f / this->ropeUVScale ();

    for (uint32_t i = 0; i < m_particleCount; i++) {
	const auto& p = m_particles[i];
	const glm::vec3* history = m_trailHistory.data () + static_cast<size_t> (i) * segments;
	const auto point = [&] (int index) -> const glm::vec3& { return index == 0 ? p.position : history[index - 1]; };
	const glm::vec4 color (p.color, p.alpha);
	const float trailLength = static_cast<float> (m_trailCount[i]) * uvScaleInverse;
	const float scroll = static_cast<float> (m_trailScroll[i]);

	for (int k = 0; k < segments; k++) {
	    const glm::vec3& start = point (k);
	    const glm::vec3& end = point (k + 1);
	    const glm::vec3& before = point (std::max (k - 1, 0));
	    const glm::vec3& after = point (std::min (k + 2, segments));
	    // with uvscrolling the length slot carries the segment index and positions move back with every push
	    const float lengthSlot = m_ropeUVScrolling ? static_cast<float> (k) : trailLength;
	    const float position = m_ropeUVScrolling ? static_cast<float> (k) - scroll : static_cast<float> (k);

	    const uint32_t baseVertex = vertexIndex;
	    for (const glm::vec2 uv : { glm::vec2 (0.0f, 0.0f), glm::vec2 (1.0f, 0.0f), glm::vec2 (1.0f, 1.0f),
					glm::vec2 (0.0f, 1.0f) }) {
		float* v = &m_vertices[static_cast<size_t> (vertexIndex++) * ROPE_FLOATS_PER_VERTEX];
		const float values[ROPE_FLOATS_PER_VERTEX] = {
		    start.x,  start.y,  start.z,  p.size,  end.x,   end.y,   end.z,   lengthSlot, before.x,
		    before.y, before.z, position, after.x, after.y, after.z, p.size, color.r,    color.g,
		    color.b,  color.a,  uv.x,     uv.y,    color.r, color.g, color.b, color.a,
		};
		std::copy (std::begin (values), std::end (values), v);
	    }

	    for (const uint32_t corner : { 0u, 1u, 2u, 2u, 3u, 0u }) {
		m_indices[indexOffset++] = baseVertex + corner;
	    }
	}
    }
}

void CParticle::renderRope () {
    if (m_pass == nullptr || m_particleCount < (m_useTrailRenderer ? 1u : 2u)) {
	return;
    }

    uint32_t vertexIndex = 0;
    uint32_t indexOffset = 0;

    if (m_useTrailRenderer) {
	this->buildRopeTrail (vertexIndex, indexOffset);
    } else {
	// Already in spawn order (oldest at index 0) thanks to compaction in update();
	// all particles in [0, m_particleCount) are alive.
	const uint32_t aliveCount = m_particleCount;

	// Each segment between consecutive particles is subdivided into m_ropeSubdivision
	// sub-segments via Catmull-Rom spline, for smooth curves instead of harsh corners.
	//
	// Rope vertex layout (26 floats per vertex, THICKFORMAT):
	// [0-3]   a_PositionVec4:   startPos.xyz, sizeStart
	// [4-7]   a_TexCoordVec4:   endPos.xyz, trailLength
	// [8-11]  a_TexCoordVec4C1: CP0.xyz, trailPosition
	// [12-15] a_TexCoordVec4C2: CP1.xyz, sizeEnd
	// [16-19] a_TexCoordVec4C3: colorEnd.rgba
	// [20-21] a_TexCoordC4:     uvs.xy
	// [22-25] a_Color:          colorStart.rgba

	const uint32_t numSegments = aliveCount - 1;
	const int subdivision = std::max (1, m_ropeSubdivision);

	auto catmullRom = [] (const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
			      float t) -> glm::vec3 {
	    float t2 = t * t, t3 = t2 * t;
	    return 0.5f
		* ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
		   + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
	};

	// First pass: evaluate the spline to get all interpolated points (position, size, color)
	const uint32_t totalPoints = numSegments * subdivision + 1;
	this->m_splinePositions.resize (totalPoints);
	this->m_splineSizes.resize (totalPoints);
	this->m_splineColors.resize (totalPoints);
	auto& splinePositions = this->m_splinePositions;
	auto& splineSizes = this->m_splineSizes;
	auto& splineColors = this->m_splineColors;

	for (uint32_t i = 0; i < numSegments; i++) {
	    const auto& p1 = m_particles[i];
	    const auto& p2 = m_particles[i + 1];
	    const auto& p0 = (i > 0) ? m_particles[i - 1] : p1;
	    const auto& p3 = (i + 2 < aliveCount) ? m_particles[i + 2] : p2;

	    for (int k = 0; k < subdivision; k++) {
		float t = static_cast<float> (k) / static_cast<float> (subdivision);
		uint32_t idx = i * subdivision + k;

		splinePositions[idx] = catmullRom (p0.position, p1.position, p2.position, p3.position, t);
		splineSizes[idx] = glm::mix (p1.size, p2.size, t);
		splineColors[idx] = glm::mix (glm::vec4 (p1.color, p1.alpha), glm::vec4 (p2.color, p2.alpha), t);
	    }
	}
	// Last point is the final particle
	{
	    const auto& pLast = m_particles[aliveCount - 1];
	    splinePositions[totalPoints - 1] = pLast.position;
	    splineSizes[totalPoints - 1] = pLast.size;
	    splineColors[totalPoints - 1] = glm::vec4 (pLast.color, pLast.alpha);
	}

	// Second pass: build quads from consecutive spline points. The shader computes UV.v as
	// trailPosition / (trailLength - 1), so trailLength/trailPosition are expressed in
	// sub-segment units for the correct UV slice per quad. UV scale divides the effective
	// length, pushing UVs past [0,1] so the texture repeats.
	const uint32_t totalSubSegments = totalPoints - 1;
	const float uvScale = (m_ropeUVScale > 0.0f) ? m_ropeUVScale : 1.0f;
	const float trailLength = static_cast<float> (totalSubSegments) / uvScale + 1.0f;
	const float usableLength = trailLength - 1.0f;

	// UV smoothing: distribute UV proportional to arc length instead of uniform index.
	// Per wiki: only when all particle lifetimes match and scrolling is disabled.
	const bool useSmoothing = m_ropeUVSmoothing && m_uniformLifetimes && !m_ropeUVScrolling;
	auto& cumulativeArcLength = this->m_cumulativeArcLength;
	float totalArcLength = 0.0f;

	if (useSmoothing) {
	    cumulativeArcLength.resize (totalPoints, 0.0f);
	    for (uint32_t i = 1; i < totalPoints; i++) {
		totalArcLength += glm::distance (splinePositions[i], splinePositions[i - 1]);
		cumulativeArcLength[i] = totalArcLength;
	    }
	}

	// UV scrolling: shift UV along the rope over time (1 UV cycle per second)
	float scrollOffset = 0.0f;
	if (m_ropeUVScrolling && usableLength > 0.0f) {
	    scrollOffset = std::fmod (static_cast<float> (g_Time), 10000.0f) * usableLength;
	}

	for (uint32_t s = 0; s < totalSubSegments; s++) {
	    const glm::vec3& posStart = splinePositions[s];
	    const glm::vec3& posEnd = splinePositions[s + 1];
	    float sizeStart = splineSizes[s];
	    float sizeEnd = splineSizes[s + 1];
	    const glm::vec4& colorStart = splineColors[s];
	    const glm::vec4& colorEnd = splineColors[s + 1];

	    // Neighboring points for shader tangent computation (CP0/CP1)
	    const glm::vec3& posPrev = (s > 0) ? splinePositions[s - 1] : posStart;
	    const glm::vec3& posAfter = (s + 2 < totalPoints) ? splinePositions[s + 2] : posEnd;

	    // Compute trailPosition for UV mapping
	    float trailPosition;
	    if (useSmoothing && totalArcLength > 0.0f) {
		// Arc-length parameterization: map cumulative distance to sub-segment space
		trailPosition = cumulativeArcLength[s] / totalArcLength * static_cast<float> (totalSubSegments);
	    } else {
		trailPosition = static_cast<float> (s);
	    }
	    trailPosition += scrollOffset;

	    auto addRopeVertex = [&] (float uvX, float uvY) {
		const uint32_t base = vertexIndex * ROPE_FLOATS_PER_VERTEX;

		// a_PositionVec4: startPos.xyz, sizeStart
		m_vertices[base + 0] = posStart.x;
		m_vertices[base + 1] = posStart.y;
		m_vertices[base + 2] = posStart.z;
		m_vertices[base + 3] = sizeStart;

		// a_TexCoordVec4: endPos.xyz, trailLength
		m_vertices[base + 4] = posEnd.x;
		m_vertices[base + 5] = posEnd.y;
		m_vertices[base + 6] = posEnd.z;
		m_vertices[base + 7] = trailLength;

		// a_TexCoordVec4C1: CP0.xyz (neighbor before start), trailPosition
		m_vertices[base + 8] = posPrev.x;
		m_vertices[base + 9] = posPrev.y;
		m_vertices[base + 10] = posPrev.z;
		m_vertices[base + 11] = trailPosition;

		// a_TexCoordVec4C2: CP1.xyz (neighbor after end), sizeEnd
		m_vertices[base + 12] = posAfter.x;
		m_vertices[base + 13] = posAfter.y;
		m_vertices[base + 14] = posAfter.z;
		m_vertices[base + 15] = sizeEnd;

		// a_TexCoordVec4C3: colorEnd.rgba
		m_vertices[base + 16] = colorEnd.r;
		m_vertices[base + 17] = colorEnd.g;
		m_vertices[base + 18] = colorEnd.b;
		m_vertices[base + 19] = colorEnd.a;

		// a_TexCoordC4: uvs.xy
		m_vertices[base + 20] = uvX;
		m_vertices[base + 21] = uvY;

		// a_Color: colorStart.rgba
		m_vertices[base + 22] = colorStart.r;
		m_vertices[base + 23] = colorStart.g;
		m_vertices[base + 24] = colorStart.b;
		m_vertices[base + 25] = colorStart.a;

		vertexIndex++;
	    };

	    // Quad: 4 vertices (left/right at start/end of segment)
	    uint32_t baseVertex = vertexIndex;
	    addRopeVertex (0.0f, 0.0f); // left at start
	    addRopeVertex (1.0f, 0.0f); // right at start
	    addRopeVertex (1.0f, 1.0f); // right at end
	    addRopeVertex (0.0f, 1.0f); // left at end

	    m_indices[indexOffset++] = baseVertex + 0;
	    m_indices[indexOffset++] = baseVertex + 1;
	    m_indices[indexOffset++] = baseVertex + 2;
	    m_indices[indexOffset++] = baseVertex + 2;
	    m_indices[indexOffset++] = baseVertex + 3;
	    m_indices[indexOffset++] = baseVertex + 0;
	}
    }

    m_activeIndexCount = static_cast<GLsizei> (indexOffset);
    if (m_activeIndexCount == 0) {
	return;
    }

#if !NDEBUG
    std::string str = "Rope particles ";
    str += this->getParticle ().name + " (" + std::to_string (this->getId ()) + ", " + this->getParticle ().particleFile
	+ ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif

    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertexIndex * ROPE_FLOATS_PER_VERTEX * sizeof (float)),
	m_vertices.data (), GL_DYNAMIC_DRAW
    );

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indexOffset * sizeof (uint32_t)), m_indices.data (),
	GL_DYNAMIC_DRAW
    );

    updateMatrices ();

    // REFRACT: blit current scene content into the copy FBO before rendering
    if (m_hasRefract && m_refractFBO) {
	auto sceneFBO = getScene ().getFBO ();
	GLint w = static_cast<GLint> (sceneFBO->getRealWidth ());
	GLint h = static_cast<GLint> (sceneFBO->getRealHeight ());
	glBindFramebuffer (GL_READ_FRAMEBUFFER, sceneFBO->getFramebuffer ());
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_refractFBO->getFramebuffer ());
	glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glEnable (GL_DEPTH_CLAMP);
    m_pass->render ();
    glDisable (GL_DEPTH_CLAMP);

#if !NDEBUG
    glPopDebugGroup ();
#endif
}
