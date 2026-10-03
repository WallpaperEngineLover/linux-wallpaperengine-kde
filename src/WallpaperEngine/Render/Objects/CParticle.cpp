#include "CParticle.h"
#include "CImage.h"
#include "CMesh.h"

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
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <numeric>

extern float g_Time;
extern float g_RealTime;

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Utils;
using namespace WallpaperEngine::Data::Model;

namespace {
/** wallpaper64.exe works in a y-up particle space, the particles here live in the same space mirrored on y */
glm::vec3 flipY (glm::vec3 value) {
    value.y = -value.y;
    return value;
}

EmitterClock startClock (const ParticleEmitter& emitter) {
    return { .delay = emitter.delay, .duration = emitter.duration, .pendingBurst = emitter.instantaneous };
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

/** sub_1401D15A0 cases 0xB/0xC: a color moved in HSV by the override color's distance to the reference */
glm::vec3 shiftColor (const glm::vec3& rgb, const glm::vec3& shift) {
    const glm::vec3 hsv = WallpaperEngine::Maths::rgbToHsv (rgb);
    const float hue = shift.x + hsv.x;
    return WallpaperEngine::Maths::hsvToRgb (
	glm::vec3 (
	    hue - std::floor (hue), std::clamp (shift.y + hsv.y, 0.0f, 1.0f), std::clamp (shift.z + hsv.z, 0.0f, 1.0f)
	)
    );
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
	    m_ropeSubdivision = std::clamp (static_cast<int> (renderer.subdivision), 0, 32);
	    m_ropeUVScale = renderer.uvScale;
	    m_ropeUVScrolling = renderer.uvScrolling;

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

    // the pool is maxcount big whatever the count override says, that one only binds emitter rates
    // (wallpaper64.exe sub_1401D3780 copies maxcount unscaled, sub_1402378A0 emits while alive < maxcount)
    m_maxParticles = particle.maxCount > 0 ? particle.maxCount : DEFAULT_MAX_PARTICLES;

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
	// rope: a segment between every two neighbouring particles, the geometry shader subdivides it
	const size_t segments = std::max (1u, m_maxParticles - 1);
	m_vertices.resize (segments * 4 * ROPE_FLOATS_PER_VERTEX);
	m_indices.resize (segments * 6);
    } else {
	// 4 vertices, 6 indices per particle
	const int verticesPerParticle = 4;
	const int indicesPerParticle = 6;

	m_vertices.resize (m_maxParticles * verticesPerParticle * SPRITE_FLOATS_PER_VERTEX);
	m_indices.resize (m_maxParticles * indicesPerParticle);
    }
}

CParticle::CParticle (CParticle& parent, const ParticleChild& child) : CParticle (parent.getScene (), *child.particle) {
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

    if (m_particle.material && m_particle.material->material && !m_particle.material->material->passes.empty ()) {
	auto& firstPass = *m_particle.material->material->passes.begin ();

	// Overbright: brightness multiplier for additive particles
	auto overbrightIt = firstPass->constants.find ("ui_editor_properties_overbright");
	if (overbrightIt != firstPass->constants.end ()) {
	    m_overbright = overbrightIt->second->value->getFloat ();
	}
    }

    if (const auto texture = getTexture (); texture && texture->isAnimated ()) {
	m_spritesheetFrames = static_cast<int> (texture->getFrames ().size ());
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

    const auto playback = this->getPlayback ();
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

    if (dt > 0.0f) {
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

    m_drawFlipY = !getScene ().getCamera ().isPerspective ();

    if (m_drawFlipY) {
	m_modelMatrix = m_modelMatrix * glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
    }

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
    // sub_14024CA10: never while paused, otherwise while anything still emits or lives, children included
    return this->getPlayback () != Playback::Paused && !this->isFinished ();
}

void CParticle::applyPlayback (Playback playback) {
    // wallpaper64.exe 2.8.42 IParticleSystem: pause only holds back the emitters (+1719 bit 1, read by sub_1402378A0),
    // stop leaves the pause alone
    if (playback == Playback::Paused) {
	this->setPlayback (Playback::Paused);
	return;
    }

    if (playback == Playback::Playing) {
	// sub_14024C5B0: a system with nothing left lets its emitters go again (sub_14022F5B0), their timers stay
	this->setPlayback (Playback::Playing);

	if (this->isFinished ()) {
	    this->resumeEmitters ();
	}
	return;
    }

    // sub_14024C680: every particle and child gone, the emitters back at their start but finished
    this->restart ();
    this->finishEmitters ();
}

bool CParticle::emissionPaused () const {
    const CParticle* root = this;

    while (root->m_parent != nullptr) {
	root = root->m_parent;
    }

    return root->getPlayback () == Playback::Paused;
}

void CParticle::finishEmitters () {
    for (auto& clock : m_emitterClocks) {
	clock.finished = true;
    }
    for (const auto& child : m_staticChildren) {
	child->finishEmitters ();
    }
}

void CParticle::resumeEmitters () {
    for (auto& clock : m_emitterClocks) {
	clock.finished = false;
    }
    m_emissionStopped = false;
    for (const auto& child : m_staticChildren) {
	child->resumeEmitters ();
    }
}

void CParticle::emitParticles (int count) {
    // sub_14024CAC0: nothing for a negative count. sub_1402378A0 with dt 0 then adds it to every emitter's spawn
    // count, paused or not, as long as the pool has room
    if (count < 0) {
	return;
    }

    m_forcedEmission = count == 0 ? 1 : static_cast<uint32_t> (count);
    const uint32_t firstNew = m_particleCount;

    for (auto& emitter : m_emitters) {
	emitter (m_particles, m_particleCount, 0.0f);
    }

    m_forcedEmission = 0;
    this->registerNewParticles (firstNew);
}

void CParticle::registerNewParticles (uint32_t firstNew) {
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
	    std::fill_n (
		m_trailHistory.begin () + static_cast<size_t> (i) * m_ropeSegments, m_ropeSegments, p.position
	    );
	    m_trailCount[i] = 1;
	    m_trailScroll[i] = 0;
	}

	if (m_hasBirthEvents && !m_prewarming) {
	    m_births.push_back (p.id);
	}
    }
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

    // pause() stops emission (and the emitter timers) but keeps simulating what is already alive
    if (!this->emissionPaused () && !m_emissionStopped) {
	const uint32_t firstNew = m_particleCount;

	for (auto& emitter : m_emitters) {
	    emitter (m_particles, m_particleCount, dt);
	}

	this->registerNewParticles (firstNew);
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
	    // sub_140236CD0: frame = age / lifetime * sequencemultiplier, the shader takes frac() of it. WE's loader
	    // only knows "randomframe", every other mode is this one, and the texture's own frame times are not used
	    if (m_particle.animationMode == "randomframe") {
		if (p.frame < 0.0f) {
		    // per slot rather than per address, the address changes between runs
		    std::mt19937 particleRng (
			static_cast<std::mt19937::result_type> (i + this->getId () * 2654435761u)
		    );
		    std::uniform_int_distribution<int> dist (0, m_spritesheetFrames - 1);
		    p.frame = static_cast<float> (dist (particleRng));
		}
	    } else {
		p.frame = p.getLifetimePos () * m_particle.sequenceMultiplier * m_spritesheetFrames;
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
    m_colorOverride.active = (instanceOverride.hasColor || m_scriptColor) && color.r >= 0.0f
	&& (m_particle.flags & 8) == 0
	&& (distance.x >= 0.0035294117f || distance.y >= 0.0035294117f || distance.z >= 0.0035294117f)
	&& (m_parent == nullptr || m_parent->m_colorOverride.active);

    // sub_1401D15A0: the spawn color starts at the brightness override (HDR scene rendering only, not with particle
    // flag 8). A file without a color initializer, or any scene before version 5, also multiplies by the color
    const float brightness
	= getScene ().isHDR () && (m_particle.flags & 8) == 0 ? instanceOverride.brightness->value->getFloat () : 1.0f;
    m_colorOverride.tint
	= (m_colorOverride.active && m_particle.overrideColorTints ? color : glm::vec3 (1.0f)) * brightness;

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
    const auto it = std::lower_bound (m_particles.begin (), end, id, [] (const ParticleInstance& p, uint32_t value) {
	return p.id < value;
    });

    return it != end && it->id == id ? &*it : nullptr;
}

void CParticle::placeChild (const glm::mat4& placement) { m_placement = placement; }

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
    // WE's world matrix, parent chain included: particles parented to a layer or group move and scale with it
    glm::mat4 matrix = getScene ().objectWorldMatrix (m_particle);

    // 2D scenes: WE's space (y up from the bottom left) to the centered y down space of the ortho projection here
    if (!getScene ().getCamera ().isPerspective ()) {
	const glm::mat4 flipY = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
	const glm::vec3 center (getScene ().getWidth () / 2.0f, getScene ().getHeight () / 2.0f, 0.0f);
	matrix = flipY * glm::translate (glm::mat4 (1.0f), -center) * matrix * flipY;
    }

    // CScene::renderFrame() already folds disableparallax into getParallaxDisplacement()
    if (getScene ().getScene ().camera.parallax.enabled->value->getBool ()) {
	const glm::vec2 offset = getScene ().getParallaxOffset (m_particle);
	matrix = glm::translate (glm::mat4 (1.0f), glm::vec3 (offset.x, offset.y, 0.0f)) * matrix;
    }

    return matrix;
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
    const bool takesParentParticles = m_childDefinition != nullptr && (m_childDefinition->flags & 1) != 0;
    const int firstParticlePoint = m_childDefinition != nullptr ? m_childDefinition->controlPointStartIndex : 0;

    for (size_t i = 0; i < m_controlPoints.size (); i++) {
	auto& cp = m_controlPoints[i];

	if (cp.remapOutput) {
	    continue;
	}

	if (cp.linkMouse) {
	    // the cursor through the scene camera replaces the point's translation, its offset plays no part
	    const glm::vec3 position = getScene ().unprojectCursor ();

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

	// sub_14022A070: the point's local matrix (its offset, or what the instance override made of it) is local
	// unless flag 2 says world, and the point lives in the system's space. Control point 0 of a world space system
	// always counts as local
	const glm::mat4 local = localControlPointMatrix (i);
	glm::mat4 matrix;
	if (m_worldSpace) {
	    matrix = cp.worldSpace && i != 0 ? local : m_frame * local;
	} else {
	    matrix = cp.worldSpace ? toLocal * local : local;
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
    m_emissionStopped = false;
    m_prewarmed = false;
    m_eventId.reset ();
    m_hasEventParticle = false;
    m_following = false;
    for (auto& cp : m_controlPoints) {
	cp.hasPreviousPosition = false;
    }
    for (auto& counter : m_sequences) {
	counter.sequence = 0.0f;
    }

    for (const auto& child : m_staticChildren) {
	child->restart ();
    }
    this->clearEventChildren ();
}

void CParticle::restartEmission () {
    m_systemTime = 0.0f;
    m_emitterClocks.clear ();
    for (const auto& emitter : m_particle.emitters) {
	if (emitter.name == "boxrandom" || emitter.name == "sphererandom" || emitter.name == "layerimage") {
	    m_emitterClocks.push_back (startClock (emitter));
	}
    }
    m_emissionStopped = false;
    for (auto& counter : m_sequences) {
	counter.sequence = 0.0f;
    }

    for (const auto& child : m_staticChildren) {
	child->restartEmission ();
    }
}

void CParticle::startEmitterPeriod () {
    // static children with child flag 2 start emitting over, the particles already out stay
    for (const auto& child : m_staticChildren) {
	if ((child->m_childDefinition->flags & 2) != 0) {
	    child->restartEmission ();
	}
    }
    for (auto& counter : m_sequences) {
	if (counter.periodReset) {
	    counter.sequence = 0.0f;
	}
    }
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
    return std::all_of (m_emitterClocks.begin (), m_emitterClocks.end (), [] (const EmitterClock& clock) {
	return clock.finished;
    });
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

void CParticle::setupEmitters () {
    m_emitterClocks.clear ();
    m_imageEmitters.clear ();

    for (const auto& emitter : m_particle.emitters) {
	EmitterFunc func;

	if (emitter.name == "boxrandom") {
	    func = createBoxEmitter (emitter);
	} else if (emitter.name == "sphererandom") {
	    func = createSphereEmitter (emitter);
	} else if (emitter.name == "layerimage") {
	    func = createImageEmitter (emitter);
	} else {
	    sLog.out ("Unknown emitter type: ", emitter.name);
	    continue;
	}

	if (func) {
	    m_emitterClocks.push_back (startClock (emitter));
	    m_emitters.push_back (std::move (func));
	}
    }
}

uint32_t CParticle::emitCount (EmitterClock& clock, const ParticleEmitter& emitter, float dt) {
    // wallpaper64.exe sub_1402378A0. The count override is bound to rate and maxtoemitperperiod at load
    // (sub_1401C5490, type 2 float and type 1 int bindings), not with particle flag 0x20
    DynamicValue* countOverride = this->emitterCountOverride ();
    const float count = countOverride != nullptr ? countOverride->getFloat () : 1.0f;
    const float rate = emitter.rate * count;
    const int maxPerPeriod = countOverride != nullptr
	? static_cast<int> (static_cast<float> (emitter.maxToEmitPerPeriod) * count)
	: emitter.maxToEmitPerPeriod;
    const bool periodic = (emitter.flags & 4) != 0;
    const bool room = m_particleCount < m_particles.size ();
    // emitParticles () spawns its count whatever state the emitter is in, the pool only has to have room
    uint32_t toEmit = room ? m_forcedEmission : 0;

    if (!clock.finished && room && clock.delay <= 0.0f && !this->emissionPaused () && !m_emissionStopped) {
	float emitRate = rate;
	if (emitter.audioProcessingMode != 0) {
	    emitRate *= sampleAudio (
		emitter.audioProcessingMode, emitter.audioProcessingBounds, emitter.audioProcessingExponent,
		emitter.audioProcessingFrequencyStart, emitter.audioProcessingFrequencyEnd
	    );
	}

	if (periodic) {
	    // the loader clamps both minimums to their maximum (sub_1401C1C70)
	    if (clock.period <= 0.0f) {
		clock.period += dt;
		if (clock.period < 0.0f) {
		    emitRate = 0.0f;
		} else {
		    const float minDuration = std::min (emitter.minPeriodicDuration, emitter.maxPeriodicDuration);
		    clock.period = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f)
			    * (emitter.maxPeriodicDuration - minDuration)
			+ minDuration;
		    clock.pendingBurst = emitter.instantaneous;
		    clock.emittedInPeriod = 0;
		    this->startEmitterPeriod ();
		}
	    } else {
		clock.period -= dt;
		if (clock.period < 0.0f) {
		    const float minDelay = std::min (emitter.minPeriodicDelay, emitter.maxPeriodicDelay);
		    clock.period
			= -(WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f)
				* (emitter.maxPeriodicDelay - minDelay)
			    + minDelay);
		}
	    }
	}

	toEmit += clock.pendingBurst;
	clock.pendingBurst = 0;

	// WE takes the burst off the accumulator too, so an instantaneous burst holds the rate back
	const float accumulated = emitRate * dt + clock.accumulator;
	if (accumulated < 1.0f || clock.duration < 0.0f) {
	    clock.accumulator = accumulated - static_cast<float> (toEmit);
	} else {
	    int n = static_cast<int> (std::floor (accumulated));
	    clock.accumulator = accumulated - static_cast<float> (n + static_cast<int> (toEmit));
	    if ((emitter.flags & 2) != 0) {
		n = std::min (n, 1);
	    }
	    if (periodic && maxPerPeriod != 0) {
		n = std::min (std::max (maxPerPeriod - clock.emittedInPeriod, 0), n);
		clock.emittedInPeriod += n;
	    }
	    toEmit += static_cast<uint32_t> (std::max (n, 0));
	}
    }

    // after the spawn: the delay runs down first, then the duration. Without a duration an emitter stops
    // right away unless it has a rate or a periodic burst
    if (clock.delay > 0.0f) {
	clock.delay -= dt;
    } else if (clock.duration <= 0.0f) {
	if (!(rate > 0.0f || (emitter.instantaneous != 0 && periodic))) {
	    clock.finished = true;
	}
    } else {
	clock.duration -= dt;
	if (clock.duration <= 0.0f) {
	    clock.duration = -1.0f;
	    clock.finished = true;
	}
    }

    return std::min<uint32_t> (toEmit, static_cast<uint32_t> (m_particles.size ()) - m_particleCount);
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
    glm::vec3 transformedEmitterOrigin = emitter.origin;
    transformedEmitterOrigin.y = -transformedEmitterOrigin.y;

    int controlPointIndex = emitter.controlPoint;
    if (controlPointIndex == -1 && !m_particle.controlPoints.empty ()) {
	const auto& cp0 = m_particle.controlPoints[0];
	if ((cp0.flags & 1) != 0) {
	    controlPointIndex = 0;
	}
    }

    const size_t clockIndex = m_emitters.size ();

    return [this, emitter, transformedEmitterOrigin, controlPointIndex,
	    clockIndex] (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) {
	const uint32_t toEmit = this->emitCount (m_emitterClocks[clockIndex], emitter, dt);

	for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
	    auto& p = particles[count];

	    const ControlPointData* cp
		= controlPointIndex >= 0 && controlPointIndex < static_cast<int> (m_controlPoints.size ())
		? &m_controlPoints[controlPointIndex]
		: nullptr;

	    // sub_1402378A0 box emitter: u = (2 rand - 1) * directions per axis, then
	    // pos = sign (u) * (|u| * (distancemax - distancemin) + distancemin), in WE's y-up space
	    glm::vec3 randomPos;
	    for (int axis = 0; axis < 3; axis++) {
		const float u = (WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * 2.0f - 1.0f)
		    * emitter.directions[axis];
		const float sign = u > 0.0f ? 1.0f : (u < 0.0f ? -1.0f : 0.0f);
		randomPos[axis] = sign
		    * (std::abs (u) * (emitter.distanceMax[axis] - emitter.distanceMin[axis])
		       + emitter.distanceMin[axis]);
	    }
	    randomPos.y = -randomPos.y;

	    this->placeSpawn (p, cp, controlPointIndex, transformedEmitterOrigin, randomPos);

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

	    // sub_14023B340 starts the step collisionquad looks at where the emitter put the particle
	    p.previousPosition = p.position;

	    for (auto& init : m_initializers) {
		init (p);
	    }

	    count++;
	}
    };
}

void CParticle::buildImagePixels (ImageEmitter& state, const CImage& image) {
    // sub_1401D3AE0: the layer drawn a quarter of its size (capped to 3840x2160 keeping the aspect) through
    // materials/util/downsample_quarter (four bilinear taps two texels out on the diagonals), read back, and every
    // pixel at least half opaque kept with its colour and its place in layer units around the layer's center. WE
    // samples the layer's buffer, effects included; here it is the layer's texture stretched over its size
    state.pixels.clear ();
    state.built = true;

    const auto texture = image.getTexture ();
    const glm::vec2 layerSize = image.getSize ();
    if (texture == nullptr || layerSize.x < 1.0f || layerSize.y < 1.0f) {
	return;
    }
    const int width = static_cast<uint16_t> (layerSize.x);
    const int height = static_cast<uint16_t> (layerSize.y);
    int cappedWidth = width;
    int cappedHeight = height;
    if (width > 0xF00 || height > 0x870) {
	const float aspect = static_cast<float> (width) / static_cast<float> (height);
	if (aspect < 1.7777778f) {
	    cappedHeight = 2160;
	    cappedWidth = static_cast<int> (aspect * 2160.0f);
	} else {
	    cappedWidth = 3840;
	    cappedHeight = static_cast<int> (3840.0f / aspect);
	}
    }
    const int columns = std::max (2, cappedWidth / 4);
    const int rows = std::max (2, cappedHeight / 4);
    const float scale = static_cast<float> (width) / static_cast<float> (columns);

    const int textureWidth = static_cast<int> (texture->getTextureWidth (0));
    const int textureHeight = static_cast<int> (texture->getTextureHeight (0));
    if (textureWidth <= 0 || textureHeight <= 0) {
	return;
    }
    std::vector<uint8_t> data (static_cast<size_t> (textureWidth) * textureHeight * 4);
    glBindTexture (GL_TEXTURE_2D, texture->getTextureID (0));
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data ());

    // the layer covers the texture's real part
    const float uScale = static_cast<float> (texture->getRealWidth ()) / static_cast<float> (textureWidth);
    const float vScale = static_cast<float> (texture->getRealHeight ()) / static_cast<float> (textureHeight);
    const auto texel = [&] (int x, int y) {
	x = std::clamp (x, 0, textureWidth - 1);
	y = std::clamp (y, 0, textureHeight - 1);
	const uint8_t* p = &data[(static_cast<size_t> (y) * textureWidth + x) * 4];
	return glm::vec4 (p[0], p[1], p[2], p[3]) / 255.0f;
    };
    const auto sample = [&] (glm::vec2 uv) {
	const glm::vec2 at = glm::vec2 (uv.x * uScale * textureWidth, uv.y * vScale * textureHeight) - 0.5f;
	const glm::ivec2 base = glm::floor (at);
	const glm::vec2 f = at - glm::vec2 (base);
	return glm::mix (
	    glm::mix (texel (base.x, base.y), texel (base.x + 1, base.y), f.x),
	    glm::mix (texel (base.x, base.y + 1), texel (base.x + 1, base.y + 1), f.x), f.y
	);
    };

    const glm::vec2 step (2.0f / static_cast<float> (width), 2.0f / static_cast<float> (height));
    for (int x = 0; x < columns; x++) {
	for (int y = 0; y < rows; y++) {
	    const glm::vec2 uv ((x + 0.5f) / columns, (y + 0.5f) / rows);
	    const glm::vec4 albedo
		= (sample (uv - step) + sample (uv + step) + sample (uv + glm::vec2 (-step.x, step.y))
		   + sample (uv + glm::vec2 (step.x, -step.y)))
		* 0.25f;
	    const auto unorm
		= [] (float value) { return static_cast<uint8_t> (std::clamp (value * 255.0f + 0.5f, 0.0f, 255.0f)); };
	    if (unorm (albedo.a) < 0x7F) {
		continue;
	    }
	    state.pixels.push_back (
		{ .r = unorm (albedo.r),
		  .g = unorm (albedo.g),
		  .b = unorm (albedo.b),
		  .x = static_cast<int16_t> (static_cast<int> (scale * x - (width >> 1) + scale * 0.5f)),
		  .y = static_cast<int16_t> (static_cast<int> (scale * y - (height >> 1) + scale * 0.5f)) }
	    );
	}
    }
}

EmitterFunc CParticle::createImageEmitter (const ParticleEmitter& emitter) {
    const size_t clockIndex = m_emitters.size ();
    m_imageEmitters.push_back ({ .index = static_cast<int> (m_imageEmitters.size ()) });
    const size_t stateIndex = m_imageEmitters.size () - 1;

    // sub_1402378A0 emitter type 3 (layerimage) with the defaults of sub_1401B9930
    return [this, emitter, clockIndex,
	    stateIndex] (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) {
	ImageEmitter& state = m_imageEmitters[stateIndex];
	const uint32_t toEmit = this->emitCount (m_emitterClocks[clockIndex], emitter, dt);

	const CObject* target = this->componentDependency ("emitterimage", state.index);
	const auto* image = target != nullptr && target->is<CImage> () ? target->as<CImage> () : nullptr;
	if (image == nullptr) {
	    return;
	}

	// flags 0x20000 reads the layer again every second
	if ((emitter.flags & 0x20000) != 0) {
	    state.refreshTimer += dt;
	    if (state.refreshTimer > 1.0f) {
		state.refreshTimer = std::fmod (state.refreshTimer, 1.0f);
		state.built = false;
	    }
	}
	if (!state.built || state.size != glm::ivec2 (image->getSize ())) {
	    this->buildImagePixels (state, *image);
	    state.size = glm::ivec2 (image->getSize ());
	}
	if (state.pixels.empty ()) {
	    return;
	}

	const glm::mat4 toParticles = this->worldToParticles ();
	const glm::mat4 world = getScene ().objectWorldMatrix (image->getImage ());
	const glm::mat4 current = toParticles * world;
	const glm::mat4 previous = toParticles * state.previousWorld;
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const glm::vec3 offsetMin
	    = emitter.offsetMin.value_or (flat ? glm::vec3 (-5.0f, -5.0f, 0.0f) : glm::vec3 (0.0f));
	const glm::vec3 offsetRange
	    = emitter.offsetMax.value_or (flat ? glm::vec3 (5.0f, 5.0f, 0.0f) : glm::vec3 (0.0f)) - offsetMin;

	std::uniform_int_distribution<size_t> pick (0, state.pixels.size () - 1);
	for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
	    auto& p = particles[count];
	    const ImagePixel& pixel = state.pixels[pick (m_rng)];

	    glm::vec3 offset (0.0f);
	    if ((emitter.flags & 0x80000) != 0) {
		for (int axis = 0; axis < 3; axis++) {
		    offset[axis]
			= WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * offsetRange[axis] + offsetMin[axis];
		}
	    }
	    const glm::vec4 local (pixel.x + offset.x, -pixel.y + offset.y, offset.z, 1.0f);
	    p.position = glm::vec3 (current * local);

	    // flags 0x40000: the pixel's movement over the last frame, times a random speed
	    p.velocity = glm::vec3 (0.0f);
	    if ((emitter.flags & 0x40000) != 0 && m_frameDelta > 0.0f) {
		const float speed
		    = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * (emitter.speedMax - emitter.speedMin)
		    + emitter.speedMin;
		p.velocity = (p.position - glm::vec3 (previous * local)) / m_frameDelta * speed;
	    }
	    p.acceleration = glm::vec3 (0.0f);
	    p.rotation = glm::vec3 (0.0f);
	    p.angularVelocity = glm::vec3 (0.0f);
	    p.angularAcceleration = glm::vec3 (0.0f);

	    // flags 0x10000: the spawn colour takes the pixel's
	    p.color = m_colorOverride.tint;
	    if ((emitter.flags & 0x10000) != 0) {
		p.color *= glm::vec3 (pixel.r, pixel.g, pixel.b) / 255.0f;
	    }
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
	    p.previousPosition = p.position;
	    m_emitOrientation = glm::mat3 (1.0f);

	    for (auto& init : m_initializers) {
		init (p);
	    }

	    count++;
	}

	// the frame's image matrix, what the next frame's velocities start from
	state.previousWorld = world;
    };
}

EmitterFunc CParticle::createSphereEmitter (const ParticleEmitter& emitter) {
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

    const size_t clockIndex = m_emitters.size ();

    return [this, emitter, transformedEmitterOrigin, controlPointIndex, speedOverride, lifetime,
	    clockIndex] (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) {
	const uint32_t toEmit = this->emitCount (m_emitterClocks[clockIndex], emitter, dt);

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
	    const glm::vec3 point
		= glm::vec3 (r * c, r * s * std::sin (phi), r * s * std::cos (phi)) * emitter.directions;
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
				 + (emitter.speedMax - emitter.speedMin)
				     * WallpaperEngine::Maths::randomFloat (m_rng, 0.00001f, 1.0f))
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
	    func = createLifetimeRandomInitializer (*initializer->as<LifetimeRandomInitializer> ());
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
	glm::vec3 min = minValue->getVec3 () / 255.0f;
	glm::vec3 max = maxValue->getVec3 () / 255.0f;
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
    DynamicValue* sizeOverride
	= (m_particle.flags & 0x80) == 0 ? m_particle.instanceOverride.size->value.get () : nullptr;

    return [this, minValue, maxValue, exponentValue, sizeOverride] (ParticleInstance& p) {
	float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	float exponent = exponentValue->getFloat ();
	float min = minValue->getFloat ();
	float max = maxValue->getFloat ();

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

namespace {
/** min + t * (max - min) per axis, t = random ^ exponent (plain random for exponent 1), x drawn first */
glm::vec3 randomExponentVec3 (std::mt19937& rng, const glm::vec3& min, const glm::vec3& max, float exponent) {
    glm::vec3 result;
    for (int i = 0; i < 3; i++) {
	float t = WallpaperEngine::Maths::randomFloat (rng, 0.0f, 1.0f);
	if (exponent != 1.0f) {
	    t = std::pow (t, exponent);
	}
	result[i] = min[i] + t * (max[i] - min[i]);
    }
    return result;
}
} // namespace

InitializerFunc CParticle::createVelocityRandomInitializer (const VelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min ? init.min->value.get () : nullptr;
    DynamicValue* maxValue = init.max ? init.max->value.get () : nullptr;
    DynamicValue* exponentValue = init.exponent->value.get ();
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;

    // sub_14023B340 case 7: z is drawn first, then y and x
    return [this, minValue, maxValue, exponentValue, speedOverride] (ParticleInstance& p) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const glm::vec3 min = minValue != nullptr ? minValue->getVec3 ()
	    : flat                                ? glm::vec3 (-32.0f, -32.0f, 0.0f)
						  : glm::vec3 (-1.0f);
	const glm::vec3 max = maxValue != nullptr ? maxValue->getVec3 ()
	    : flat                                ? glm::vec3 (32.0f, 32.0f, 0.0f)
						  : glm::vec3 (1.0f);
	const float exponent = exponentValue->getFloat ();

	glm::vec3 vel;
	for (int i = 2; i >= 0; i--) {
	    float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	    if (exponent != 1.0f) {
		t = std::pow (t, exponent);
	    }
	    vel[i] = min[i] + t * (max[i] - min[i]);
	}
	if (speedOverride != nullptr) {
	    vel *= speedOverride->getFloat ();
	}
	vel.y = -vel.y;
	p.velocity += m_emitOrientation * vel;
    };
}

InitializerFunc CParticle::createRotationRandomInitializer (const RotationRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    // case 10, no speed binding
    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	p.rotation
	    += randomExponentVec3 (m_rng, minValue->getVec3 (), maxValue->getVec3 (), exponentValue->getFloat ());
    };
}

InitializerFunc CParticle::createAngularVelocityRandomInitializer (const AngularVelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;

    // case 12
    return [this, minValue, maxValue, exponentValue, speedOverride] (ParticleInstance& p) {
	glm::vec3 result
	    = randomExponentVec3 (m_rng, minValue->getVec3 (), maxValue->getVec3 (), exponentValue->getFloat ());
	if (speedOverride != nullptr) {
	    result *= speedOverride->getFloat ();
	}
	p.angularVelocity += result;
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
	// sub_14023B340 case 9: the renderer's scene clock is added to the phase
	const float phase = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * phaseRange + phaseMin
	    + getScene ().getSceneClock ();
	const float timeScale = timeScaleVal->getFloat () * m_particle.instanceOverride.rate->value->getFloat ();

	const float angle
	    = simplexNoise1D (phase * timeScale) * glm::pi<float> () * scaleVal->getFloat () + offsetVal->getFloat ();
	const float speed = WallpaperEngine::Maths::randomFloat (m_rng, speedMin->getFloat (), speedMax->getFloat ());

	glm::vec3 right = rightVal->getVec3 ();

	if (glm::length (right) < 0.0001f) {
	    right = glm::vec3 (0.0f, 0.0f, 1.0f);
	}

	glm::vec3 direction = glm::mat3 (glm::rotate (glm::mat4 (1.0f), angle, right)) * forwardVal->getVec3 ();

	direction.y = -direction.y;

	p.velocity += m_emitOrientation * (direction * speed * speedOverride->getFloat ());
    };
}

InitializerFunc
CParticle::createMapSequenceAroundControlPointInitializer (const MapSequenceAroundControlPointInitializer& init) {
    // the loader's basis (sub_1401C19E0): the axis normalized, (0, 0, 1) when zero, and two directions around it
    glm::vec3 axis = init.axis == glm::vec3 (0.0f) ? glm::vec3 (0.0f, 0.0f, 1.0f) : glm::normalize (init.axis);
    glm::vec3 across (1.0f, 0.0f, 0.0f);
    glm::vec3 up (0.0f, 1.0f, 0.0f);
    if (axis.x != 0.0f || axis.y != 0.0f) {
	const glm::vec3 side = glm::cross (axis, glm::vec3 (0.0f, 0.0f, 1.0f));
	across = glm::normalize (side);
	up = glm::normalize (glm::cross (axis, side));
    }

    const int controlPoint = init.controlPoint;
    const bool mirror = init.mirror;
    const float boundsMin = init.bounds.x;
    const float boundsRange = init.bounds.y - init.bounds.x;
    const glm::vec3 speedMin = init.speedMin;
    const glm::vec3 speedRange = init.speedMax - init.speedMin;
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;
    SequenceCounter& counter = m_sequences.emplace_back (
	SequenceCounter { .step = 1.0f / (init.count <= 0.000099999997f ? 0.000099999997f : init.count) }
    );

    // sub_14023B340 case 13: the particle is turned around the control point's axis to the sequence's angle, keeping
    // its distance from the axis and its height along it, then gets a random speed in that frame
    return [this, axis, across, up, controlPoint, mirror, boundsMin, boundsRange, speedMin, speedRange, speedOverride,
	    &counter] (ParticleInstance& p) {
	const ControlPointData& point = m_controlPoints[controlPoint];
	const auto turn = [&point] (const glm::vec3& value) { return flipY (point.orientation * flipY (value)); };
	const glm::vec3 center = controlPointWE (controlPoint);
	const glm::vec3 pointAxis = turn (axis);
	const glm::vec3 pointAcross = turn (across);
	const glm::vec3 pointUp = turn (up);

	const glm::vec3 relative = flipY (p.position) - center;
	const float along = glm::dot (relative, pointAxis);
	const float radius = glm::length (relative - along * pointAxis);

	const float angle = (counter.sequence * boundsRange + boundsMin) * 6.2831855f;
	const float sine = std::sin (angle);
	const float cosine = std::cos (angle);
	const glm::vec3 outwards = sine * pointAcross + cosine * pointUp;
	const glm::vec3 around = cosine * pointAcross - sine * pointUp;

	const float randomZ = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	const float randomX = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	const float randomY = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	const glm::vec3 velocity = around * (speedMin.x + randomX * speedRange.x)
	    + outwards * (speedMin.y + randomY * speedRange.y) + pointAxis * (speedMin.z + randomZ * speedRange.z);

	p.position = flipY (outwards * radius + (along * pointAxis + center));
	p.velocity += flipY (velocity) * (speedOverride != nullptr ? speedOverride->getFloat () : 1.0f);

	counter.sequence += counter.step;
	if (counter.sequence > 1.0f) {
	    if (mirror) {
		counter.step = -counter.step;
		counter.sequence = 1.0f - (counter.sequence - 1.0f);
	    } else {
		counter.sequence = std::fmod (counter.sequence, 1.0f);
	    }
	} else if (counter.sequence < 0.0f) {
	    counter.sequence = -counter.sequence;
	    counter.step = -counter.step;
	}
    };
}

namespace {
/** Applies an inherit input from the event's particle, the initializer also moves the base values operators start from
 */
void applyEventInput (ParticleInstance& p, const ParticleInstance& source, ParticleEventInput input, bool initial) {
    switch (input) {
	case ParticleEventInput::SetColor:
	    p.color = source.color;
	    break;
	case ParticleEventInput::MultiplyColor:
	    p.color *= source.color;
	    break;
	case ParticleEventInput::SetOpacity:
	    p.alpha = source.alpha;
	    break;
	case ParticleEventInput::MultiplyOpacity:
	    p.alpha *= source.alpha;
	    break;
	case ParticleEventInput::SetColorOpacity:
	    p.color = source.color;
	    p.alpha = source.alpha;
	    break;
	case ParticleEventInput::MultiplyColorOpacity:
	    p.color *= source.color;
	    p.alpha *= source.alpha;
	    break;
	case ParticleEventInput::SetVelocity:
	    p.velocity = source.velocity;
	    break;
	case ParticleEventInput::AddVelocity:
	    p.velocity += source.velocity;
	    break;
	case ParticleEventInput::SetSize:
	    p.size = source.size;
	    break;
	case ParticleEventInput::MultiplySize:
	    p.size *= source.size;
	    break;
	case ParticleEventInput::SetRotation:
	    p.rotation = source.rotation;
	    break;
	case ParticleEventInput::AddRotation:
	    p.rotation += source.rotation;
	    break;
	case ParticleEventInput::SetAngularVelocity:
	    p.angularVelocity = source.angularVelocity;
	    break;
	case ParticleEventInput::AddAngularVelocity:
	    p.angularVelocity += source.angularVelocity;
	    break;
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
    return
	[this, input, inStart, inEnd, outStart, outEnd] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
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
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;

    // sub_14023FBC0 case 1: velocity first, then position with the new velocity, then drag over the frame scaled dt.
    // It runs over every pool slot, dead ones included
    const bool turnGravity = (op.flags & 1) != 0;

    return [this, dragValue, gravityValue, speedOverride, turnGravity] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	glm::vec3 gravity = gravityValue->getVec3 () * (speedOverride != nullptr ? speedOverride->getFloat () : 1.0f);
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
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;

    // sub_14023FBC0 case 2, like movement: the speed override only scales the force (a loader binding)
    return [this, dragValue, forceValue, speedOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	const float drag = std::min (dragValue->getFloat () * frameScaledDelta (dt), 0.99999988f);
	glm::vec3 force = forceValue->getVec3 ();
	if (speedOverride != nullptr) {
	    force *= speedOverride->getFloat ();
	}
	const glm::vec3 step = force * dt;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }

	    const glm::vec3 angularVelocity = p.angularVelocity + step;
	    p.rotation += angularVelocity * dt;
	    p.angularVelocity = angularVelocity * (1.0f - drag);
	}
    };
}

OperatorFunc CParticle::createAlphaFadeOperator (const AlphaFadeOperator& op) {
    DynamicValue* fadeInTimeValue = op.fadeInTime->value.get ();
    DynamicValue* fadeOutTimeValue = op.fadeOutTime->value.get ();

    // the loader sets system flag 0x10 for alphafade, alphachange and oscillatealpha: sub_14023FBC0 starts every
    // pass from the spawn alpha and they multiply it
    m_resetAlphaFromBase = true;

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
		    p.alpha *= WallpaperEngine::Maths::fadeValue (life, 0.0f, fadeInTime, 0.0f, 1.0f);
		} else if (life > fadeOutTime) {
		    p.alpha *= 1.0f - WallpaperEngine::Maths::fadeValue (life, fadeOutTime, 1.0f, 0.0f, 1.0f);
		}
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
    DynamicValue* sizeOverride
	= (m_particle.flags & 0x80) == 0 ? m_particle.instanceOverride.size->value.get () : nullptr;

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
		p.size *= multiplier;
	    }
	};
}

OperatorFunc CParticle::createAlphaChangeOperator (const AlphaChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    m_resetAlphaFromBase = true;

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
		p.alpha *= multiplier;
	    }
	};
}

OperatorFunc CParticle::createColorChangeOperator (const ColorChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    // system flag 8: the pass starts from the spawn color
    m_resetColorFromBase = true;

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

		p.color *= color;
	    }
	};
}

OperatorFunc CParticle::createTurbulenceOperator (const TurbulenceOperator& op) {
    DynamicValue* scaleValue = op.scale ? op.scale->value.get () : nullptr;
    DynamicValue* speedMinValue = op.speedMin ? op.speedMin->value.get () : nullptr;
    DynamicValue* speedMaxValue = op.speedMax ? op.speedMax->value.get () : nullptr;
    DynamicValue* timeScaleValue = op.timeScale ? op.timeScale->value.get () : nullptr;
    DynamicValue* maskValue = op.mask ? op.mask->value.get () : nullptr;
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    // the loader binds speedmin/speedmax to the speed override unless particle flag 0x10, timescale to the rate
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;
    DynamicValue* rateOverride = m_particle.instanceOverride.rate->value.get ();
    DynamicValue* audioModeValue = op.audioProcessingMode->value.get ();
    DynamicValue* audioBoundsValue = op.audioProcessingBounds->value.get ();
    DynamicValue* audioExponentValue = op.audioProcessingExponent->value.get ();
    DynamicValue* audioStartValue = op.audioProcessingFrequencyStart->value.get ();
    DynamicValue* audioEndValue = op.audioProcessingFrequencyEnd->value.get ();

    this->m_usesParticleSeed = true;

    // sub_14023FBC0 case 14, defaults from sub_1401BEB80. phasemin is loaded but never read
    return [this, scaleValue, speedMinValue, speedMaxValue, timeScaleValue, maskValue, phaseMinValue, phaseMaxValue,
	    speedOverride, rateOverride, audioModeValue, audioBoundsValue, audioExponentValue, audioStartValue,
	    audioEndValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const glm::vec3 mask
	    = maskValue != nullptr ? maskValue->getVec3 () : (flat ? glm::vec3 (1.0f, 1.0f, 0.0f) : glm::vec3 (1.0f));
	if (mask.x == 0.0f && mask.y == 0.0f && mask.z == 0.0f) {
	    return;
	}

	const float audio = audioModeValue->getInt () != 0
	    ? sampleAudio (
		  audioModeValue->getInt (), audioBoundsValue->getVec2 (), audioExponentValue->getFloat (),
		  audioStartValue->getInt (), audioEndValue->getInt ()
	      )
	    : 1.0f;
	const float speed = speedOverride != nullptr ? speedOverride->getFloat () : 1.0f;
	const float speedMin = (speedMinValue != nullptr ? speedMinValue->getFloat () : (flat ? 500.0f : 1.0f)) * speed;
	const float speedMax
	    = (speedMaxValue != nullptr ? speedMaxValue->getFloat () : (flat ? 1000.0f : 5.0f)) * speed;
	const float speedRange = (speedMax - speedMin) * audio;
	const float scaledMin = speedMin * audio;
	const float phaseRange = phaseMaxValue->getFloat () - phaseMinValue->getFloat ();
	const float scale = scaleValue != nullptr ? scaleValue->getFloat () : (flat ? 0.01f : 0.5f);
	const float timeScale = (timeScaleValue != nullptr ? timeScaleValue->getFloat () : (flat ? 20.0f : 1.0f))
	    * rateOverride->getFloat ();
	// the renderer's scene clock moves the noise field
	const float time = getScene ().getSceneClock () * timeScale;
	const glm::vec3 step = mask * frameScaledDelta (dt);

	const auto push = [&] (ParticleInstance& p) {
	    const float phase = p.seed * phaseRange + time;
	    // WE's particle space is y-up
	    const float x = scale * (phase + p.position.x);
	    const float y = scale * (phase - p.position.y);
	    const float z = scale * (phase + p.position.z);
	    const float strength = p.seed * speedRange + scaledMin;

	    glm::vec3 delta (0.0f);

	    if (mask.x != 0.0f) {
		delta.x = simplexNoise3D (x, y, z);
	    }
	    if (mask.y != 0.0f) {
		delta.y = simplexNoise3D (z, x, y);
	    }
	    if (mask.z != 0.0f) {
		delta.z = simplexNoise3D (y, z, x);
	    }

	    delta *= step * strength;
	    p.velocity += glm::vec3 (delta.x, -delta.y, delta.z);
	};

	// every pool slot, dead ones included
	for (uint32_t i = 0; i < count; i++) {
	    if (particles[i].alive) {
		push (particles[i]);
	    }
	}
	for (uint32_t slot = 0; slot < m_ghostUsed.size (); slot++) {
	    if (m_ghostUsed[slot]) {
		push (m_ghosts[slot]);
	    }
	}
    };
}

OperatorFunc CParticle::createVortexOperator (const VortexOperator& op) {
    const int controlPoint = op.controlPoint;
    const bool v2 = op.v2;
    const bool infiniteAxis = (op.flags & 1) != 0;
    // vortex (v1) reads neither centerforce nor the ring
    const bool useCenterForce = v2 && (op.flags & 2) != 0;
    const bool ringShape = v2 && (op.flags & 4) != 0;
    const BlendWindow blend = makeBlendWindow (op.blend);
    DynamicValue* axisValue = op.axis->value.get ();
    DynamicValue* offsetValue = op.offset->value.get ();
    DynamicValue* distanceInnerValue = op.distanceInner ? op.distanceInner->value.get () : nullptr;
    DynamicValue* distanceOuterValue = op.distanceOuter ? op.distanceOuter->value.get () : nullptr;
    DynamicValue* speedInnerValue = op.speedInner ? op.speedInner->value.get () : nullptr;
    DynamicValue* speedOuterValue = op.speedOuter->value.get ();
    DynamicValue* centerForceValue = op.centerForce->value.get ();
    DynamicValue* ringRadiusValue = op.ringRadius ? op.ringRadius->value.get () : nullptr;
    DynamicValue* ringWidthValue = op.ringWidth ? op.ringWidth->value.get () : nullptr;
    DynamicValue* ringPullDistanceValue = op.ringPullDistance ? op.ringPullDistance->value.get () : nullptr;
    DynamicValue* ringPullForceValue = op.ringPullForce ? op.ringPullForce->value.get () : nullptr;
    DynamicValue* audioModeValue = op.audioProcessingMode->value.get ();
    DynamicValue* audioBoundsValue = op.audioProcessingBounds->value.get ();
    DynamicValue* audioExponentValue = op.audioProcessingExponent->value.get ();
    DynamicValue* audioStartValue = op.audioProcessingFrequencyStart->value.get ();
    DynamicValue* audioEndValue = op.audioProcessingFrequencyEnd->value.get ();
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;

    // sub_14023FBC0 case 15 (vortex), 16 and its blended variant 37 (vortex_v2). Loaders in sub_1401C5490, defaults
    // from sub_1401BEF00 / sub_1401BF2D0. Worked out in WE's y-up space
    return [this, controlPoint, v2, infiniteAxis, useCenterForce, ringShape, blend, axisValue, offsetValue,
	    distanceInnerValue, distanceOuterValue, speedInnerValue, speedOuterValue, centerForceValue, ringRadiusValue,
	    ringWidthValue, ringPullDistanceValue, ringPullForceValue, audioModeValue, audioBoundsValue,
	    audioExponentValue, audioStartValue, audioEndValue, speedOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float, float dt
	   ) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const float audio = audioModeValue->getInt () != 0
	    ? sampleAudio (
		  audioModeValue->getInt (), audioBoundsValue->getVec2 (), audioExponentValue->getFloat (),
		  audioStartValue->getInt (), audioEndValue->getInt ()
	      )
	    : 1.0f;
	const float speedScale = speedOverride != nullptr ? speedOverride->getFloat () : 1.0f;
	const float speedInner
	    = (speedInnerValue != nullptr ? speedInnerValue->getFloat () : (flat ? 2500.0f : 1.0f)) * speedScale;
	const float speedOuter = speedOuterValue->getFloat () * speedScale;
	const float scaledDt = frameScaledDelta (dt);
	const float innerSpeed = speedInner * audio * scaledDt;
	const float speedRange = (speedOuter - speedInner) * audio * scaledDt;

	// the loader normalizes the axis, (0, 0, 1) when it is too short
	glm::vec3 axis = axisValue->getVec3 ();
	axis = glm::length (axis) >= 0.001f ? glm::normalize (axis) : glm::vec3 (0.0f, 0.0f, 1.0f);

	const ControlPointData& point = controlPoints[controlPoint];
	glm::vec3 center = controlPointWE (controlPoint);
	if (v2) {
	    // turned (and scaled) by the control point's matrix, not normalized again
	    axis = flipY (point.orientation * flipY (axis));
	} else {
	    center += offsetValue->getVec3 ();
	}

	float start;
	float rangeScale;
	if (ringShape) {
	    const float pullDistance
		= ringPullDistanceValue != nullptr ? ringPullDistanceValue->getFloat () : (flat ? 50.0f : 0.25f);
	    start = ringWidthValue != nullptr ? ringWidthValue->getFloat () : (flat ? 50.0f : 0.2f);
	    rangeScale = pullDistance != 0.0f ? 1.0f / pullDistance : 1.0f;
	} else {
	    const float distanceOuter
		= distanceOuterValue != nullptr ? distanceOuterValue->getFloat () : (flat ? 650.0f : 2.0f);
	    start = distanceInnerValue != nullptr ? distanceInnerValue->getFloat () : (flat ? 500.0f : 1.0f);
	    rangeScale = start != distanceOuter ? 1.0f / (distanceOuter - start) : 1.0f;
	}
	const float ringRadius = ringRadiusValue != nullptr ? ringRadiusValue->getFloat () : (flat ? 300.0f : 1.0f);
	const float ringPull
	    = (ringPullForceValue != nullptr ? ringPullForceValue->getFloat () : (flat ? 10.0f : 0.05f)) * dt;
	const float centerPull = useCenterForce ? centerForceValue->getFloat () / dt : 0.0f;

	const auto spin = [&] (ParticleInstance& p) {
	    const glm::vec3 position = flipY (p.position);
	    const glm::vec3 velocity = flipY (p.velocity);
	    const glm::vec3 toParticle = position - center;
	    const glm::vec3 along = infiniteAxis ? axis * glm::dot (toParticle, axis) : glm::vec3 (0.0f);
	    const glm::vec3 radial = toParticle - along;
	    const float distance = glm::length (radial);
	    // WE's rsqrt turns a particle on the axis into NaN, leave it alone instead
	    if (distance == 0.0f) {
		return;
	    }
	    const glm::vec3 direction = radial / distance;
	    const float weight = v2 && blend.active ? blendWeight (blend, p) : 1.0f;

	    float t;
	    if (ringShape) {
		t = std::clamp ((std::abs (ringRadius - distance) - start) * rangeScale, 0.0f, 1.0f);
	    } else {
		t = std::clamp ((distance - start) * rangeScale, 0.0f, 1.0f);
	    }

	    glm::vec3 change = glm::cross (direction, axis) * ((t * speedRange + innerSpeed) * weight);

	    if (v2) {
		// keeps the particle at its distance from the axis over the coming move, and pulls it onto the ring
		const glm::vec3 next = velocity * dt + position - center - along;
		const float nextDistance = glm::length (next);
		float pull = nextDistance > 0.0f ? (distance / nextDistance - 1.0f) * centerPull : 0.0f;
		if (ringShape) {
		    const float falloff = 1.0f - t;
		    pull += (falloff != 1.0f ? std::copysign (falloff, ringRadius - distance) : 0.0f) * ringPull;
		}
		change += next * (pull * weight);
	    }

	    p.velocity += flipY (change);
	};

	for (uint32_t i = 0; i < count; i++) {
	    if (particles[i].alive) {
		spin (particles[i]);
	    }
	}
	for (uint32_t slot = 0; slot < m_ghostUsed.size (); slot++) {
	    if (m_ghostUsed[slot]) {
		spin (m_ghosts[slot]);
	    }
	}
    };
}

OperatorFunc CParticle::createControlPointAttractOperator (const ControlPointAttractOperator& op) {
    const int controlPoint = op.controlPoint;
    const bool deleteNear = (op.flags & 1) != 0;
    const bool clampToPoint = (op.flags & 2) != 0;
    DynamicValue* scaleValue = op.scale ? op.scale->value.get () : nullptr;
    DynamicValue* thresholdValue = op.threshold ? op.threshold->value.get () : nullptr;
    DynamicValue* deleteThresholdValue = op.deleteThreshold ? op.deleteThreshold->value.get () : nullptr;
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;

    if (deleteNear) {
	m_tracksPreviousPosition = true;
    }

    // sub_14023FBC0 case 10, defaults from sub_1401BDEE0
    return [this, controlPoint, deleteNear, clampToPoint, scaleValue, thresholdValue, deleteThresholdValue,
	    speedOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float, float dt
	   ) {
	const bool flat = !getScene ().getCamera ().isPerspective ();
	const float scale = (scaleValue != nullptr ? scaleValue->getFloat () : (flat ? 512.0f : 20.0f))
	    * (speedOverride != nullptr ? speedOverride->getFloat () : 1.0f);
	const float threshold = thresholdValue != nullptr ? thresholdValue->getFloat () : (flat ? 512.0f : 5.0f);
	const float force = scale * frameScaledDelta (dt);
	const glm::vec3 center = controlPoints[controlPoint].position;

	const auto attract = [&] (ParticleInstance& p) {
	    const glm::vec3 toCenter = center - p.position;
	    const float distance = glm::length (toCenter);
	    if (!(distance < threshold) || !(distance > std::numeric_limits<float>::min ())) {
		return;
	    }
	    float pull = (1.0f - distance / threshold) * force;
	    if (clampToPoint && distance < pull) {
		pull = distance;
	    }
	    p.velocity += toCenter * (pull / distance);
	};

	for (uint32_t i = 0; i < count; i++) {
	    if (particles[i].alive) {
		attract (particles[i]);
	    }
	}
	for (uint32_t slot = 0; slot < m_ghostUsed.size (); slot++) {
	    if (m_ghostUsed[slot]) {
		attract (m_ghosts[slot]);
	    }
	}

	if (!deleteNear) {
	    return;
	}

	// sub_14022A150: a particle whose step since the operators started came within deletethreshold dies
	const float deleteThreshold
	    = deleteThresholdValue != nullptr ? deleteThresholdValue->getFloat () : (flat ? 15.0f : 0.5f);
	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }
	    const glm::vec3 step = p.position - p.previousPosition;
	    const float length = glm::length (step);
	    const glm::vec3 direction = length > 0.0f ? step / length : step;
	    const float along = std::clamp (glm::dot (center - p.previousPosition, direction), 0.0f, length);
	    const glm::vec3 closest = center - (p.previousPosition + direction * along);
	    if (!(deleteThreshold * deleteThreshold < glm::dot (closest, closest))) {
		p.age = p.lifetime;
	    }
	}
    };
}

namespace {
/** oscillatealpha/oscillatesize as the loader stores them: min and max - min */
struct OscillationRanges {
    float frequencyMin;
    float frequencyRange;
    float phaseMin;
    float phaseRange;
    float scaleMin;
    float scaleRange;
};

template <typename T> OscillationRanges oscillationRanges (const T& op) {
    const float frequencyMin = op.frequencyMin->value->getFloat ();
    const float phaseMin = op.phaseMin->value->getFloat ();
    const float scaleMin = op.scaleMin->value->getFloat ();
    return {
	.frequencyMin = frequencyMin,
	.frequencyRange = op.frequencyMax->value->getFloat () - frequencyMin,
	.phaseMin = phaseMin,
	.phaseRange = op.phaseMax->value->getFloat () - phaseMin,
	.scaleMin = scaleMin,
	.scaleRange = op.scaleMax->value->getFloat () - scaleMin,
    };
}

/** sub_14023FBC0 cases 8/9, from the particle's spawn random */
float oscillateFactor (const ParticleInstance& p, const OscillationRanges& ranges) {
    const float frequency = p.seed * ranges.frequencyRange + ranges.frequencyMin;
    const float phase = p.seed * ranges.phaseRange + ranges.phaseMin;
    return (std::sin ((phase + p.age) * frequency) + 1.0f) * (p.seed * ranges.scaleRange * 0.5f) + ranges.scaleMin;
}
} // namespace

OperatorFunc CParticle::createOscillateAlphaOperator (const OscillateAlphaOperator& op) {
    const OscillateAlphaOperator* definition = &op;
    const BlendWindow blend = makeBlendWindow (op.blend);
    m_resetAlphaFromBase = true;
    m_usesParticleSeed = true;

    // case 8, blended 30 lerps the factor from 1
    return
	[definition, blend] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    const OscillationRanges ranges = oscillationRanges (*definition);
	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}
		const float factor = oscillateFactor (p, ranges);
		p.alpha *= blend.active ? 1.0f - (1.0f - factor) * blendWeight (blend, p) : factor;
	    }
	};
}

OperatorFunc CParticle::createOscillateSizeOperator (const OscillateSizeOperator& op) {
    const OscillateSizeOperator* definition = &op;
    const BlendWindow blend = makeBlendWindow (op.blend);
    m_usesParticleSeed = true;

    // case 9, blended 31
    return
	[definition, blend] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    const OscillationRanges ranges = oscillationRanges (*definition);
	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}
		const float factor = oscillateFactor (p, ranges);
		p.size *= blend.active ? 1.0f - (1.0f - factor) * blendWeight (blend, p) : factor;
	    }
	};
}

OperatorFunc CParticle::createOscillatePositionOperator (const OscillatePositionOperator& op) {
    DynamicValue* frequencyMinValue = op.frequencyMin->value.get ();
    DynamicValue* frequencyMaxValue = op.frequencyMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax ? op.scaleMax->value.get () : nullptr;
    DynamicValue* maskValue = op.mask->value.get ();
    // the loader binds the frequencies to the speed override unless particle flag 0x10
    DynamicValue* speedOverride
	= (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value.get () : nullptr;
    const BlendWindow blend = makeBlendWindow (op.blend);
    m_usesParticleSeed = true;

    // case 7, blended 29 scales the move by the weight. x and z share one wave, y's is 2pi * random ahead. Its
    // phase is in seconds, the move is the wave's change since the last frame
    return [this, frequencyMinValue, frequencyMaxValue, phaseMinValue, phaseMaxValue, scaleMinValue, scaleMaxValue,
	    maskValue, speedOverride, blend] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	const float speed = speedOverride != nullptr ? speedOverride->getFloat () : 1.0f;
	const float frequencyMin = frequencyMinValue->getFloat () * speed;
	const float frequencyRange = frequencyMaxValue->getFloat () * speed - frequencyMin;
	const float phaseMin = phaseMinValue->getFloat ();
	const float phaseRange = phaseMaxValue->getFloat () - phaseMin;
	const float scaleMin = scaleMinValue->getFloat ();
	const float scaleMax = scaleMaxValue != nullptr ? scaleMaxValue->getFloat ()
							: (getScene ().getCamera ().isPerspective () ? 0.5f : 10.0f);
	const float scaleRange = scaleMax - scaleMin;
	const glm::vec3 mask = maskValue->getVec3 ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }

	    const float frequency = p.seed * frequencyRange + frequencyMin;
	    const float time = p.seed * phaseRange + phaseMin + p.age;
	    const float shifted = p.seed * 6.2831855f + time;
	    float scale = p.seed * scaleRange + scaleMin;
	    if (blend.active) {
		scale *= blendWeight (blend, p);
	    }
	    const float wave = (std::sin (time * frequency) - std::sin ((time - dt) * frequency)) * scale;
	    const float waveY = (std::sin (shifted * frequency) - std::sin ((shifted - dt) * frequency)) * scale;

	    // WE's particle space is y-up
	    p.position += glm::vec3 (wave * mask.x, -waveY * mask.y, wave * mask.z);
	}
    };
}

namespace {
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
	case ParticleRemapComponent::X:
	    return glm::vec3 (value.x);
	case ParticleRemapComponent::Y:
	    return glm::vec3 (value.y);
	case ParticleRemapComponent::Z:
	    return glm::vec3 (value.z);
	case ParticleRemapComponent::Sum:
	    return glm::vec3 ((value.y + value.x) + value.z);
	case ParticleRemapComponent::Average:
	    return glm::vec3 (((value.y + value.x) + value.z) * 0.33333334f);
	case ParticleRemapComponent::Max:
	    return glm::vec3 (std::fmax (std::fmax (value.x, value.y), value.z));
	case ParticleRemapComponent::Min:
	    return glm::vec3 (std::fmin (std::fmin (value.x, value.y), value.z));
	default:
	    return value;
    }
}

float remapApply (ParticleRemapOperation operation, float current, float value) {
    switch (operation) {
	case ParticleRemapOperation::Remap:
	    return value;
	case ParticleRemapOperation::Multiply:
	    return value * current;
	case ParticleRemapOperation::Add:
	    return value + current;
	case ParticleRemapOperation::Subtract:
	    return current - value;
	default:
	    return current;
    }
}

/** The transform functions of sub_14023FBC0 case 19, seed is the particle's random as integer bits */
float remapTransformOperator (
    ParticleRemapTransform transform, float value, float scale, int octaves, float fbmAmplitude, int32_t seed
) {
    switch (transform) {
	case ParticleRemapTransform::Sine:
	    return std::sin (value * (scale * glm::pi<float> ()) - glm::half_pi<float> ()) * 0.5f + 0.5f;
	case ParticleRemapTransform::Square:
	    {
		const float scaled = value * scale;
		return std::nearbyint (scaled - std::trunc (scaled)) + (scaled < 0.0f ? 1.0f : 0.0f);
	    }
	case ParticleRemapTransform::Saw:
	    {
		const float scaled = value * scale;
		return (scaled - std::trunc (scaled)) + (value < 0.0f ? 1.0f : 0.0f);
	    }
	case ParticleRemapTransform::Triangle:
	    {
		const float scaled = std::fabs (value * scale);
		return 1.0f - std::fabs ((scaled - std::trunc (scaled)) * 2.0f - 1.0f);
	    }
	case ParticleRemapTransform::SimplexNoise:
	    return hashedNoise2D (seed, value * scale, 0.0f) * 0.5f + 0.5f;
	case ParticleRemapTransform::FbmNoise:
	    return hashedNoiseFbm (octaves, fbmAmplitude, seed, value * scale, 0.0f) * 0.5f + 0.5f;
	default:
	    return value;
    }
}

/** The transform functions of sub_14023B340 case 15: floor instead of trunc, 1D noise without a seed */
float remapTransformInitial (ParticleRemapTransform transform, float value, float scale, int octaves) {
    switch (transform) {
	case ParticleRemapTransform::Sine:
	    return std::sin ((value * glm::pi<float> ()) * scale - glm::half_pi<float> ()) * 0.5f + 0.5f;
	case ParticleRemapTransform::Square:
	    {
		const float scaled = value * scale;
		return std::round (scaled - std::floor (scaled)) + (scaled < 0.0f ? 1.0f : 0.0f);
	    }
	case ParticleRemapTransform::Saw:
	    {
		const float scaled = value * scale;
		return (scaled - std::floor (scaled)) + (value < 0.0f ? 1.0f : 0.0f);
	    }
	case ParticleRemapTransform::Triangle:
	    {
		const float scaled = std::fabs (value * scale);
		return 1.0f - std::fabs ((scaled - std::floor (scaled)) * 2.0f - 1.0f);
	    }
	case ParticleRemapTransform::SimplexNoise:
	    return simplexNoise1D (value * scale) * 0.5f + 0.5f;
	case ParticleRemapTransform::FbmNoise:
	    return simplexFbm1D (value, scale, octaves) * 0.5f + 0.5f;
	default:
	    return value;
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

glm::vec3 CParticle::drawVector (const glm::vec3& value) const { return m_drawFlipY ? flipY (value) : value; }

glm::mat4 CParticle::localControlPointMatrix (size_t index) const {
    const auto& cp = m_controlPoints[index];
    glm::mat4 local = glm::translate (glm::mat4 (1.0f), cp.offset);

    // sub_14022BD40, skipped for points with flags 0x10005 like WE
    if (cp.followParent || index >= m_particle.instanceOverride.controlPoints.size ()) {
	return local;
    }

    const auto& angles = m_particle.instanceOverride.controlPointAngles[index];
    const auto& scriptAngles = m_scriptControlPointAngles[index];

    if (angles || scriptAngles) {
	// Rz * Ry * Rx in WE's y-up space, radians
	const glm::vec3 angle = angles ? angles->value->getVec3 () : *scriptAngles;
	const float cx = std::cos (angle.x), sx = std::sin (angle.x);
	const float cy = std::cos (angle.y), sy = std::sin (angle.y);
	const float cz = std::cos (angle.z), sz = std::sin (angle.z);
	const glm::mat3 rotation (
	    cy * cz, cy * sz, -sy, sy * cz * sx - cx * sz, sy * sz * sx + cx * cz, sx * cy, cx * cz * sy + sx * sz,
	    cx * sz * sy - sx * cz, cx * cy
	);
	// the same rotation in the y mirrored space used here
	const glm::mat3 mirror (1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
	const glm::mat3 mirrored = mirror * rotation * mirror;
	local[0] = glm::vec4 (mirrored[0], 0.0f);
	local[1] = glm::vec4 (mirrored[1], 0.0f);
	local[2] = glm::vec4 (mirrored[2], 0.0f);
    }

    if (const auto& position = m_particle.instanceOverride.controlPoints[index]; position) {
	local[3] = glm::vec4 (flipY (position->value->getVec3 ()), 1.0f);
    } else if (const auto& scriptPosition = m_scriptControlPoints[index]) {
	local[3] = glm::vec4 (flipY (*scriptPosition), 1.0f);
    }

    return local;
}

glm::vec3 CParticle::getInstanceColor () const {
    const auto& instanceOverride = m_particle.instanceOverride;

    return instanceOverride.hasColor || m_scriptColor ? instanceOverride.colorn->value->getVec3 () : glm::vec3 (-1.0f);
}

void CParticle::setInstanceColor (const glm::vec3& color) {
    m_scriptColor = true;
    m_particle.instanceOverride.colorn->value->update (color, DynamicValue::UpdateSource::Script);
}

glm::vec3 CParticle::getInstanceControlPoint (size_t index, bool angle) const {
    const auto& scene
	= angle ? m_particle.instanceOverride.controlPointAngles : m_particle.instanceOverride.controlPoints;
    const auto& script = angle ? m_scriptControlPointAngles : m_scriptControlPoints;

    if (index >= scene.size ()) {
	return glm::vec3 (0.0f);
    }

    if (scene[index]) {
	return scene[index]->value->getVec3 ();
    }

    return script[index].value_or (glm::vec3 (std::numeric_limits<float>::max (), 0.0f, 0.0f));
}

void CParticle::setInstanceControlPoint (size_t index, bool angle, const glm::vec3& value) {
    const auto& scene
	= angle ? m_particle.instanceOverride.controlPointAngles : m_particle.instanceOverride.controlPoints;
    auto& script = angle ? m_scriptControlPointAngles : m_scriptControlPoints;

    if (index >= scene.size ()) {
	return;
    }

    if (scene[index]) {
	scene[index]->value->update (value, DynamicValue::UpdateSource::Script);
	return;
    }

    // sub_14022BD40 skips a point whose x is FLT_MAX
    if (value.x == std::numeric_limits<float>::max ()) {
	script[index].reset ();
    } else {
	script[index] = value;
    }
}

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
	const float saturation
	    = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) * saturationSpan + saturationMin;
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
	const auto& picked = colors[std::uniform_int_distribution<size_t> (0, colors.size () - 1) (m_rng)];
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
    return
	[this, directionsValue, signValue, scaleValue, distanceValue, timeScaleValue, octaves] (ParticleInstance& p) {
	    const bool flat = !getScene ().getCamera ().isPerspective ();
	    const glm::vec3 directions = directionsValue != nullptr ? directionsValue->getVec3 ()
								    : (flat ? glm::vec3 (1, 1, 0) : glm::vec3 (1));
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
    SequenceCounter& counter
	= m_sequences.emplace_back (SequenceCounter { .step = step, .periodReset = (flags & 0x20) != 0 });

    // sub_14023B340 case 14: spread along the line between two control points, one step per particle
    return [this, start, end, flags, mirror, boundsMin, boundsRange, arcAmountValue, arcDirectionValue,
	    sizeReductionValue, &counter] (ParticleInstance& p) {
	float& sequence = counter.sequence;
	float& step = counter.step;
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
	case ParticleRemapValue::MaxLifetime:
	    return glm::vec3 (p.lifetime);
	case ParticleRemapValue::Size:
	    return glm::vec3 (p.initial.size);
	case ParticleRemapValue::Opacity:
	    return glm::vec3 (p.initial.alpha);
	case ParticleRemapValue::Speed:
	    return glm::vec3 (glm::length (p.velocity));
	case ParticleRemapValue::Rotation:
	    return glm::vec3 (p.rotation.z);
	case ParticleRemapValue::AngularSpeed:
	    return glm::vec3 (m_hasAngularVelocity ? p.angularVelocity.z : 0.0f);
	case ParticleRemapValue::DistanceToControlPoint:
	    return glm::vec3 (glm::length (flipY (p.position) - controlPoint (remap.inputControlPoint0)));
	case ParticleRemapValue::PositionBetweenTwoControlPoints:
	    {
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
	case ParticleRemapValue::Runtime:
	    return glm::vec3 (getScene ().getSceneClock ());
	case ParticleRemapValue::TimeOfDay:
	    return glm::vec3 (dayFraction ());
	case ParticleRemapValue::ParticleSystemTime:
	    return glm::vec3 (m_systemTime);
	case ParticleRemapValue::LayerTime:
	    return glm::vec3 (layerTime ());
	case ParticleRemapValue::Color:
	    return p.initial.color;
	case ParticleRemapValue::Position:
	    return flipY (p.position);
	case ParticleRemapValue::Velocity:
	    return flipY (p.velocity);
	case ParticleRemapValue::ControlPoint:
	case ParticleRemapValue::DeltaToControlPoint:
	case ParticleRemapValue::DirectionToControlPoint:
	    {
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
	case ParticleRemapValue::LayerOrigin:
	    return remapLayerOrigin ();
	default:
	    return glm::vec3 (0.0f);
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

	const auto apply
	    = [&remap] (float current, float target) { return remapApply (remap.operation, current, target); };
	// all three components, or the one outputcomponent names
	const auto applyVector = [&remap, &apply] (glm::vec3 current, const glm::vec3& target) {
	    switch (remap.outputComponent) {
		case ParticleRemapComponent::All:
		    for (int i = 0; i < 3; i++) {
			current[i] = apply (current[i], target[i]);
		    }
		    break;
		case ParticleRemapComponent::X:
		    current.x = apply (current.x, target.x);
		    break;
		case ParticleRemapComponent::Y:
		    current.y = apply (current.y, target.y);
		    break;
		case ParticleRemapComponent::Z:
		    current.z = apply (current.z, target.z);
		    break;
		default:
		    break;
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
	    case ParticleRemapValue::Speed:
		{
		    const float speed = glm::length (p.velocity);
		    float scale = apply (speed, value.x);
		    if (speed != 0.0f) {
			scale /= speed;
		    }
		    p.velocity *= scale;
		    break;
		}
	    case ParticleRemapValue::Rotation:
		p.rotation.z = apply (p.rotation.z, value.x);
		break;
	    case ParticleRemapValue::AngularSpeed:
		if (m_hasAngularVelocity) {
		    p.angularVelocity.z = apply (p.angularVelocity.z, value.x);
		}
		break;
	    case ParticleRemapValue::DistanceToControlPoint:
		{
		    const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		    glm::vec3 offset = flipY (p.position) - point;
		    const float distance = glm::length (offset);
		    if (distance != 0.0f) {
			offset /= distance;
		    }
		    p.position = flipY (point + offset * apply (distance, value.x));
		    break;
		}
	    case ParticleRemapValue::PositionBetweenTwoControlPoints:
		{
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
	    case ParticleRemapValue::Position:
		p.position = flipY (applyVector (flipY (p.position), value));
		break;
	    case ParticleRemapValue::Velocity:
		p.velocity = flipY (applyVector (flipY (p.velocity), value));
		break;
	    case ParticleRemapValue::ControlPoint:
		{
		    auto& point = m_controlPoints[remap.outputControlPoint0];
		    point.position = flipY (applyVector (flipY (point.position), value));
		    break;
		}
	    case ParticleRemapValue::DeltaToControlPoint:
		{
		    const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
		    p.position = flipY (point - applyVector (point - flipY (p.position), value));
		    break;
		}
	    case ParticleRemapValue::DirectionToControlPoint:
		{
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
	    default:
		break;
	}
    };
}

glm::vec3 CParticle::remapOperatorInput (const ParticleRemap& remap, const ParticleInstance& p) const {
    // sub_14023FBC0 case 19, scalars in x
    switch (remap.input) {
	case ParticleRemapValue::LifetimeFraction:
	    return glm::vec3 (p.age / p.lifetime, 0.0f, 0.0f);
	case ParticleRemapValue::MaxLifetime:
	    return glm::vec3 (p.lifetime, 0.0f, 0.0f);
	case ParticleRemapValue::Size:
	    return glm::vec3 (p.size, 0.0f, 0.0f);
	case ParticleRemapValue::Opacity:
	    return glm::vec3 (p.alpha, 0.0f, 0.0f);
	case ParticleRemapValue::Speed:
	    return glm::vec3 (glm::length (p.velocity), 0.0f, 0.0f);
	case ParticleRemapValue::Rotation:
	    return glm::vec3 (p.rotation.z, 0.0f, 0.0f);
	case ParticleRemapValue::AngularSpeed:
	    return glm::vec3 (m_hasAngularVelocity ? p.angularVelocity.z : 0.0f, 0.0f, 0.0f);
	case ParticleRemapValue::DistanceToControlPoint:
	    return glm::vec3 (glm::length (flipY (p.position) - controlPointWE (remap.inputControlPoint0)), 0.0f, 0.0f);
	case ParticleRemapValue::PositionBetweenTwoControlPoints:
	    {
		// the engine reads the output control points here, not the input ones
		const glm::vec3 first = controlPointWE (remap.outputControlPoint0);
		const glm::vec3 line = controlPointWE (remap.outputControlPoint1) - first;
		const float lengthSquared = glm::dot (line, line);
		return glm::vec3 (
		    lengthSquared > 0.0f ? glm::dot (flipY (p.position) - first, line) / lengthSquared : 0.0f, 0.0f,
		    0.0f
		);
	    }
	// runtime and particlesystemtime both read the renderer's scene clock here (+304)
	case ParticleRemapValue::Runtime:
	case ParticleRemapValue::ParticleSystemTime:
	    return glm::vec3 (getScene ().getSceneClock (), 0.0f, 0.0f);
	case ParticleRemapValue::TimeOfDay:
	    return glm::vec3 (dayFraction (), 0.0f, 0.0f);
	case ParticleRemapValue::LayerTime:
	    return glm::vec3 (layerTime (), 0.0f, 0.0f);
	case ParticleRemapValue::Color:
	    return p.color;
	case ParticleRemapValue::Position:
	    return flipY (p.position);
	case ParticleRemapValue::Velocity:
	    return flipY (p.velocity);
	case ParticleRemapValue::ControlPoint:
	    return controlPointWE (remap.inputControlPoint0);
	case ParticleRemapValue::DeltaToControlPoint:
	    return controlPointWE (remap.inputControlPoint0) - flipY (p.position);
	case ParticleRemapValue::DirectionToControlPoint:
	    {
		const glm::vec3 delta = controlPointWE (remap.inputControlPoint0) - flipY (p.position);
		const float length = glm::length (delta);
		return length > 0.0f ? delta / length : glm::vec3 (0.0f);
	    }
	case ParticleRemapValue::LayerOrigin:
	    return remapLayerOrigin ();
	default:
	    return glm::vec3 (0.0f);
    }
}

OperatorFunc CParticle::createRemapValueOperator (const RemapValueOperator& op) {
    const ParticleRemap remap = op.remap;
    const BlendWindow blend = makeBlendWindow (op.blend);
    const glm::vec3 inputRange = remapRange (remap.inputRangeMin, remap.inputRangeMax);
    const glm::vec3 outputRange = remap.outputRangeMax - remap.outputRangeMin;
    const float fbmAmplitude = hashedNoiseFbmNormalizer (remap.transformOctaves);

    // sub_14023FBC0 restores alpha or color when a remap writes them (system flags 0x10/8)
    if (remap.output == ParticleRemapValue::Opacity) {
	m_resetAlphaFromBase = true;
    } else if (remap.output == ParticleRemapValue::Color) {
	m_resetColorFromBase = true;
    }
    if (remap.transform == ParticleRemapTransform::SimplexNoise
	|| remap.transform == ParticleRemapTransform::FbmNoise) {
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
	    const auto applyVector
		= [&remap, &blend, &apply] (glm::vec3 current, const glm::vec3& target, bool setQuirk) {
		      switch (remap.outputComponent) {
			  case ParticleRemapComponent::All:
			      {
				  const bool useX
				      = setQuirk && blend.active && remap.operation == ParticleRemapOperation::Remap;
				  for (int c = 0; c < 3; c++) {
				      current[c] = apply (current[c], useX ? target.x : target[c]);
				  }
				  break;
			      }
			  case ParticleRemapComponent::X:
			      current.x = apply (current.x, target.x);
			      break;
			  case ParticleRemapComponent::Y:
			      current.y = apply (current.y, target.y);
			      break;
			  case ParticleRemapComponent::Z:
			      current.z = apply (current.z, target.z);
			      break;
			  default:
			      break;
		      }
		      return current;
		  };

	    switch (remap.output) {
		case ParticleRemapValue::MaxLifetime:
		    p.lifetime = apply (p.lifetime, value.x);
		    break;
		case ParticleRemapValue::Size:
		    p.size = apply (p.size, value.x);
		    break;
		case ParticleRemapValue::Opacity:
		    p.alpha = apply (p.alpha, value.x);
		    break;
		case ParticleRemapValue::Speed:
		    {
			const float speed = glm::length (p.velocity);
			const float target = apply (speed, value.x);
			p.velocity *= speed > 0.0f ? target / speed : target;
			break;
		    }
		case ParticleRemapValue::Rotation:
		    p.rotation.z = apply (p.rotation.z, value.x);
		    break;
		case ParticleRemapValue::AngularSpeed:
		    if (m_hasAngularVelocity) {
			p.angularVelocity.z = apply (p.angularVelocity.z, value.x);
		    }
		    break;
		case ParticleRemapValue::DistanceToControlPoint:
		    {
			const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
			const glm::vec3 offset = flipY (p.position) - point;
			const float distance = glm::length (offset);
			const glm::vec3 direction = distance != 0.0f ? offset / distance : glm::vec3 (0.0f);
			p.position = flipY (point + direction * apply (distance, value.x));
			break;
		    }
		case ParticleRemapValue::PositionBetweenTwoControlPoints:
		    {
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
		case ParticleRemapValue::Color:
		    p.color = applyVector (p.color, value, true);
		    break;
		case ParticleRemapValue::Position:
		    p.position = flipY (applyVector (flipY (p.position), value, true));
		    break;
		case ParticleRemapValue::Velocity:
		    p.velocity = flipY (applyVector (flipY (p.velocity), value, true));
		    break;
		case ParticleRemapValue::ControlPoint:
		    {
			// written per particle, the last one wins
			auto& point = m_controlPoints[remap.outputControlPoint0];
			point.position = flipY (applyVector (flipY (point.position), value, false));
			break;
		    }
		case ParticleRemapValue::DeltaToControlPoint:
		    {
			const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
			p.position = flipY (point - applyVector (point - flipY (p.position), value, false));
			break;
		    }
		case ParticleRemapValue::DirectionToControlPoint:
		    {
			const glm::vec3 point = controlPointWE (remap.outputControlPoint0);
			const glm::vec3 delta = point - flipY (p.position);
			const float distance = glm::length (delta);
			const glm::vec3 direction
			    = applyVector (distance > 0.0f ? delta / distance : glm::vec3 (0.0f), value, false);
			const float length = glm::length (direction);
			p.position = flipY (point - (length > 0.0f ? direction / length : glm::vec3 (0.0f)) * distance);
			break;
		    }
		default:
		    break;
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

	for (uint32_t block = getScene ().getFrameCounter () % stride; block < blocks; block += stride) {
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

		for (uint32_t other = (block * 4 + getScene ().getFrameCounter ()) % stride; other < blocks;
		     other += stride) {
		    // WE's alive mask (lifetime != 0) is taken from the neighbor block unshuffled, so it belongs to
		    // this lane's slot there, not to the shuffled neighbor it gets applied to
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

OperatorFunc
CParticle::createMaintainDistanceToControlPointOperator (const MaintainDistanceToControlPointOperator& op) {
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

OperatorFunc
CParticle::createReduceMovementNearControlPointOperator (const ReduceMovementNearControlPointOperator& op) {
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

namespace {
/** sub_1401D5880: a capsule along the largest extent, as thick as the second largest */
void capsuleShape (const glm::vec3& extents, glm::vec3& axis, float& halfLength, glm::vec3& radius) {
    if (glm::dot (extents, extents) < 0.0099999998f) {
	axis = glm::vec3 (0.0f, 1.0f, 0.0f);
	halfLength = 0.0f;
	radius = glm::vec3 (0.0f);
	return;
    }

    float smallX = extents.x;
    float smallY = extents.y;
    float z = extents.z;
    float largeX, largeY;
    if (smallY * smallY <= smallX * smallX) {
	largeX = extents.x;
	largeY = 0.0f;
	smallX = 0.0f;
    } else {
	largeY = extents.y;
	largeX = 0.0f;
	smallY = 0.0f;
    }

    float secondX, secondZ, axisY;
    if (z * z <= largeX * largeX + largeY * largeY) {
	secondZ = extents.z;
	axisY = largeY;
	z = 0.0f;
	largeY = 0.0f;
	secondX = 0.0f;
    } else {
	secondX = largeX;
	secondZ = 0.0f;
	largeX = 0.0f;
	axisY = 0.0f;
    }

    if (secondX * secondX + largeY * largeY + secondZ * secondZ <= smallX * smallX + smallY * smallY) {
	secondZ = 0.0f;
	largeY = smallY;
	secondX = smallX;
    }

    const float length = largeX + axisY + z;
    axis = glm::vec3 (largeX, axisY, z) / length;
    halfLength = length - (largeY + secondX + secondZ);
    radius = glm::vec3 (secondX, largeY, secondZ);
}
} // namespace

glm::mat4 CParticle::worldToParticles () const {
    // WE's world to this scene's space (see the bounds collision), then the system's unless it is world space
    glm::mat4 matrix = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
    if (!getScene ().getCamera ().isPerspective ()) {
	matrix
	    = glm::translate (
		  glm::mat4 (1.0f), glm::vec3 (-getScene ().getWidth () / 2.0f, getScene ().getHeight () / 2.0f, 0.0f)
	      )
	    * matrix;
    }
    if (!m_worldSpace) {
	matrix = glm::inverse (m_frame) * matrix;
    }
    return matrix;
}

const CObject* CParticle::componentDependency (const char* type, int index) const {
    // sub_14022AF30: the record naming the index-th component of that type
    for (const auto& cur : m_particle.componentDependencies) {
	if (cur.type == type && cur.index == index) {
	    return getScene ().getObject (cur.id);
	}
    }
    return nullptr;
}

std::vector<CParticle::CollisionCapsule> CParticle::collisionCapsules (int index) const {
    // sub_1401D4580
    const CObject* target = this->componentDependency ("collisionmodel", index);
    if (target == nullptr) {
	return {};
    }

    // capsule frames are in WE's world
    const glm::mat4 toParticles = this->worldToParticles ();

    std::vector<CollisionCapsule> capsules;
    const auto add = [&capsules, &toParticles] (const glm::mat4& frame, const glm::vec3& extents) {
	glm::vec3 axis, radius;
	float halfLength;
	capsuleShape (extents, axis, halfLength, radius);

	const glm::mat4 matrix = toParticles * frame;
	const glm::vec3 start = glm::vec3 (matrix * glm::vec4 (-halfLength * axis, 1.0f));
	const glm::vec3 end = glm::vec3 (matrix * glm::vec4 (halfLength * axis, 1.0f));
	const glm::vec3 direction = glm::mat3 (matrix) * axis;
	if (glm::dot (direction, direction) <= 0.0f) {
	    return;
	}
	capsules.push_back (
	    { .start = start,
	      .direction = glm::normalize (direction),
	      .length = glm::distance (start, end),
	      .radius = glm::length (glm::mat3 (matrix) * radius) }
	);
    };

    // puppets: a capsule per bone in bone space, following the pose
    if (target->is<CImage> ()) {
	const auto* image = target->as<CImage> ();
	if (!image->hasPuppetPose ()) {
	    return {};
	}
	const auto& bones = image->getPuppetBones ();
	for (int bone = 0; bone < static_cast<int> (bones.size ()); bone++) {
	    if (bones[bone].hasCapsule) {
		add (image->getPuppetBoneTransform (bone) * bones[bone].capsule, bones[bone].capsuleExtents);
	    }
	}
	return capsules;
    }

    // models without capsules: one around the bounds, half their size on each axis
    if (target->is<CMesh> ()) {
	const auto* mesh = target->as<CMesh> ();
	const glm::vec3& low = mesh->getBoundsMin ();
	const glm::vec3& high = mesh->getBoundsMax ();
	if (!(high.x > low.x)) {
	    return {};
	}
	const glm::vec3 extents = (high - low) * 0.5f;
	add (
	    getScene ().objectWorldMatrix (mesh->getMesh ()) * glm::translate (glm::mat4 (1.0f), low + extents), extents
	);
    }

    return capsules;
}

OperatorFunc CParticle::createCollisionModelOperator (const CollisionOperator& op) {
    const int index = m_collisionModels++;
    const ParticleCollisionBehavior behavior = op.behavior;
    DynamicValue* bounceValue = op.bounceFactor->value.get ();

    // sub_14023FBC0 case 26 with sub_1402508C0 (bounce), sub_140250E00 (slide), sub_140251320 (stop) and
    // sub_1402517D0 (delete): of the capsules a particle is inside the last one counts, it is pushed out along the
    // line from the capsule's segment. Unlike the other shapes flag 2 doesn't stop the spin
    return
	[this, index, behavior, bounceValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    const std::vector<CollisionCapsule> capsules = this->collisionCapsules (index);
	    if (capsules.empty ()) {
		return;
	    }
	    const float bounce = -1.0f - bounceValue->getFloat ();

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		bool hit = false;
		glm::vec3 normal (0.0f);
		float depth = 0.0f;

		for (const auto& capsule : capsules) {
		    const float along
			= std::clamp (glm::dot (p.position - capsule.start, capsule.direction), 0.0f, capsule.length);
		    const glm::vec3 offset = p.position - (capsule.start + along * capsule.direction);
		    const float distanceSquared = glm::dot (offset, offset);
		    // on the segment itself the engine's normal would be NaN
		    if (distanceSquared < capsule.radius * capsule.radius && distanceSquared > 0.0f) {
			const float distance = std::sqrt (distanceSquared);
			hit = true;
			normal = offset / distance;
			depth = distance - capsule.radius;
		    }
		}

		if (!hit) {
		    continue;
		}
		if (behavior == ParticleCollisionBehavior::Delete) {
		    p.age = p.lifetime;
		    continue;
		}
		p.position -= depth * normal;
		const float velocity = glm::dot (p.velocity, normal);
		switch (behavior) {
		    case ParticleCollisionBehavior::Bounce:
			p.velocity += (velocity * bounce) * normal;
			break;
		    case ParticleCollisionBehavior::Slide:
			p.velocity -= velocity * normal;
			break;
		    default:
			p.velocity = glm::vec3 (0.0f);
			break;
		}
	    }
	};
}

OperatorFunc CParticle::createCollisionOperator (const CollisionOperator& op) {
    if (op.shape == ParticleCollisionShape::Model) {
	return this->createCollisionModelOperator (op);
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
		    case ParticleCollisionBehavior::Bounce:
			p.velocity += (along * bounce) * normal;
			break;
		    case ParticleCollisionBehavior::Slide:
			p.velocity -= along * normal;
			break;
		    default:
			p.velocity = glm::vec3 (0.0f);
			break;
		}
	    }
	    if ((flags & 2) != 0) {
		p.angularVelocity = glm::vec3 (0.0f);
	    }
	};

	switch (shape) {
	    case ParticleCollisionShape::Plane:
		{
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
	    case ParticleCollisionShape::Sphere:
		{
		    glm::vec3 center = originValue != nullptr
			? flipY (originValue->getVec3 ())
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
	    case ParticleCollisionShape::Quad:
		{
		    glm::vec3 origin = originValue != nullptr
			? flipY (originValue->getVec3 ())
			: (flat ? glm::vec3 (0.0f, 150.0f, 0.0f) : glm::vec3 (0.0f));
		    const glm::vec2 halfSize = (sizeValue != nullptr ? sizeValue->getVec2 ()
								     : (flat ? glm::vec2 (200.0f) : glm::vec2 (1.0f)))
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
	    case ParticleCollisionShape::Bounds:
		{
		    // the scene's own orthographic size, 0 for automatic and perspective ones, as a box from (0, 0) up
		    // in WE's scene space, turned into this scene's space and then the system's
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
	    default:
		break;
	}
    };
}

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
	// WE's renderer always defines GS_ENABLED (sub_140110630 sets renderer flag 8, sub_140162AC0 adds the define):
	// each segment is one point the geometry shader turns into a strip, curved with TRAILSUBDIVISION midpoints
	// (sub_1401D2340 cases 3 and 4)
	m_passOverride->combos["GS_ENABLED"] = 1;
	m_passOverride->combos["TRAILSUBDIVISION"] = m_ropeSubdivision;
    }
    if (m_spritesheetFrames > 0) {
	m_passOverride->combos["SPRITESHEET"] = 1;
	m_passOverride->combos["SPRITESHEETBLEND"]
	    = (m_particle.flags & 2) == 0 && m_particle.animationMode != "randomframe" ? 1 : 0;
	if (const auto texture = getTexture ()) {
	    const glm::vec4* res = texture->getResolution ();
	    m_passOverride->combos["SPRITESHEETBLENDNPOT"] = res->z < res->x ? 1 : 0;
	}
    }
    if (m_useTrailRenderer) {
	m_passOverride->combos["TRAILRENDERER"] = 1;
    }
    if (m_useRopeRenderer && m_useTrailRenderer) {
	// sub_1401D2340 case 4. WE leaves THICKFORMAT off here and packs a single size and color per point, the
	// THICKFORMAT layout below repeats them as the end values, which the shader treats the same way
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
	getScene ().followOutputSize (m_refractFBO, 1);
    }

    m_pass = new Effects::CPass (*this, m_passFBOProvider, firstPass, *m_passOverride, m_passBinds, std::nullopt);
    m_geometryStage = m_useRopeRenderer && m_pass->hasGeometryStage ();

    m_pass->setDestination (getScene ().getFBO ());
    m_pass->setInput (getTexture ());

    // Set matrix pointers - CPass will dereference these each frame
    m_pass->setModelViewProjectionMatrix (&m_mvpMatrix);
    m_pass->setModelViewProjectionMatrixInverse (&m_mvpMatrixInverse);
    m_pass->setModelMatrix (&m_worldModelMatrix);
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
	// Draw geometry: a point per rope segment for the geometry shader, indexed quads otherwise
	[this] () {
	    if (m_geometryStage) {
		glDrawArrays (GL_POINTS, 0, m_activeIndexCount);
	    } else {
		glDrawElements (GL_TRIANGLES, m_activeIndexCount, GL_UNSIGNED_INT, nullptr);
	    }
	},
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
    // m_modelMatrix comes from draw (). The shaders' g_ModelMatrix is WE's world matrix: lighting, fog and the eye
    // (g_EyePosition) work in that space, y up from the scene's bottom left in 2D scenes, where m_modelMatrix goes to
    // the centered y down space the ortho projection here draws in
    m_worldModelMatrix = m_modelMatrix;
    if (m_drawFlipY) {
	const glm::vec3 center (getScene ().getWidth () / 2.0f, getScene ().getHeight () / 2.0f, 0.0f);
	m_worldModelMatrix = glm::translate (glm::mat4 (1.0f), center)
	    * glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f)) * m_modelMatrix;
    }
    m_modelMatrixInverse = glm::inverse (m_worldModelMatrix);

    this->updateParticleViewProjection ();
    m_mvpMatrix = m_viewProjectionMatrix * m_modelMatrix;
    m_mvpMatrixInverse = glm::inverse (m_mvpMatrix);

    this->updateOrientation ();

    // g_ViewRight / g_ViewUp (uniforms 0x1B / 0x1C, sub_1400D8300) are renderer +364 / +376: rows 0 and 1 of the view,
    // the camera's right and up in the world (sub_14017FA70 0x1401801a0)
    m_viewRight = glm::vec3 (1.0f, 0.0f, 0.0f);
    m_viewUp = glm::vec3 (0.0f, 1.0f, 0.0f);

    if (const auto& camera = getScene ().getCamera (); camera.isPerspective ()) {
	const glm::mat4& view = camera.getView ();
	m_viewRight = glm::vec3 (view[0][0], view[1][0], view[2][0]);
	m_viewUp = glm::vec3 (view[0][1], view[1][1], view[2][1]);
    }

    this->updateParticleRenderVars ();
}

void CParticle::updateOrientation () {
    // sub_1402298B0: the camera's forward and up (renderer +352 / +376, -row 2 and row 1 of the view), the
    // system's world matrix as WE has it (local axes in its rows, so "v * M" is W * v here and "M * v" is
    // transpose (W) * v), forward and up worked out in the world and taken into the system's own frame
    const auto& camera = getScene ().getCamera ();
    glm::vec3 cameraForward (0.0f, 0.0f, -1.0f);
    glm::vec3 cameraUp (0.0f, 1.0f, 0.0f);

    if (camera.isPerspective ()) {
	const glm::mat4& view = camera.getView ();
	cameraForward = -glm::vec3 (view[0][2], view[1][2], view[2][2]);
	cameraUp = glm::vec3 (view[0][1], view[1][1], view[2][1]);
    }

    static const ParticleRenderer screen {};
    const ParticleRenderer& renderer = m_particle.renderers.empty () ? screen : m_particle.renderers[0];
    const glm::mat3 world (m_worldModelMatrix);
    glm::vec3 forward;
    glm::vec3 up;

    switch (renderer.orientation) {
	case 1:
	    {
		// upright: up along the axis, facing the camera around it
		up = renderer.orientationFlag ? glm::vec3 (0.0f, 1.0f, 0.0f) : world * renderer.axis;
		const glm::vec3 right = glm::cross (cameraForward, up);
		forward = glm::cross (right, up);
		break;
	    }
	case 2:
	    // fixed: the axis as forward, in the system's frame unless flags & 1
	    forward = renderer.orientationFlag ? renderer.axis : world * renderer.axis;
	    up = renderer.orientationFlag ? renderer.axisUp : world * renderer.axisUp;
	    break;
	default:
	    // screen: facing the camera, up the system's y axis (or the camera's up) made perpendicular to it
	    forward = -cameraForward;
	    up = renderer.orientationFlag ? cameraUp : world[1];
	    up -= forward * glm::dot (forward, up);
	    break;
    }

    forward = glm::transpose (world) * forward;
    up = glm::transpose (world) * up;

    m_orientationForward = glm::normalize (forward);
    m_orientationRight = glm::normalize (glm::cross (up, forward));
    m_orientationUp = glm::normalize (up);
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
	m_viewProjectionMatrix
	    = perspective ? camera.getPerspectiveLayerViewProjection () : camera.getProjection () * camera.getLookAt ();
	// g_EyePosition is the renderer eye like for every other pass, in 2D scenes 2000 units out over the scene
	// center plus the camera (end of sub_1401891A0). The trail shader's ComputeParticleTrailTangents crosses the
	// eye direction with the velocity, LIGHTING takes it as the sprite's normal
	m_eyePosition = getScene ().getFog ().eyeWorld;
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

    if (m_spritesheetFrames > 0) {
	const auto texture = getTexture ();
	const auto& frame = *texture->getFrames ().front ();
	const float frameWidth = frame.width1 / static_cast<float> (texture->getTextureWidth (frame.frameNumber));
	const float frameHeight = frame.height1 / static_cast<float> (texture->getTextureHeight (frame.frameNumber));
	const glm::vec4* res = texture->getResolution ();
	m_renderVar1 = glm::vec4 (
	    frameWidth, frameHeight, static_cast<float> (m_spritesheetFrames),
	    res->y / res->x * (frameHeight / frameWidth)
	);
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

	const glm::vec3 position = this->drawVector (p.position);
	const glm::vec3 velocity = this->drawVector (p.velocity);

	auto addVertex = [&] (float u, float v) {
	    const uint32_t base = vertexIndex * SPRITE_FLOATS_PER_VERTEX;
	    // a_Position (vec3)
	    m_vertices[base + 0] = position.x;
	    m_vertices[base + 1] = position.y;
	    m_vertices[base + 2] = position.z;
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
	    m_vertices[base + 11] = velocity.x;
	    m_vertices[base + 12] = velocity.y;
	    m_vertices[base + 13] = velocity.z;
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

    copyRefractSource ();

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

float CParticle::ropeUVScale () const { return m_ropeUVScale != 0.0f ? m_ropeUVScale : 1.0f; }

void CParticle::copyRefractSource () const {
    if (!m_hasRefract || !m_refractFBO) {
	return;
    }

    // WE resolves whatever target is bound into _rt_FullFrameBuffer (sub_1402366F0 -> sub_1400D3310). Under a
    // passthrough layer that is the layer's buffer, and the copy only works when it has _rt_FullFrameBuffer's size
    // (the window's); both always share a color format (sub_1400D2A20: RGBA8, RGBA16F with renderer flag 0x2000).
    // Otherwise the texture keeps what it had, the scene as it was before the layer, which is our scene buffer now
    std::shared_ptr<const CFBO> source = getScene ().getFBO ();

    if (const auto* layer = getScene ().getLayerTarget (); layer != nullptr) {
	const glm::ivec2 layerSize { std::max (layer->fbo->getRealWidth (), 4u),
				     std::max (layer->fbo->getRealHeight (), 4u) };
	const glm::ivec2 outputSize = glm::max (getScene ().getOutputSize (), glm::ivec2 (2));

	if (layerSize == outputSize) {
	    source = layer->fbo;
	}
    }

    const GLint sw = static_cast<GLint> (source->getRealWidth ());
    const GLint sh = static_cast<GLint> (source->getRealHeight ());
    const GLint dw = static_cast<GLint> (m_refractFBO->getRealWidth ());
    const GLint dh = static_cast<GLint> (m_refractFBO->getRealHeight ());

    // our copy is scene sized, the shader samples it with normalized coordinates only
    glBindFramebuffer (GL_READ_FRAMEBUFFER, source->getFramebuffer ());
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_refractFBO->getFramebuffer ());
    glBlitFramebuffer (0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, sw == dw && sh == dh ? GL_NEAREST : GL_LINEAR);
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
	    const glm::vec3 start = this->drawVector (point (k));
	    const glm::vec3 end = this->drawVector (point (k + 1));
	    const glm::vec3 before = this->drawVector (point (std::max (k - 1, 0)));
	    const glm::vec3 after = this->drawVector (point (std::min (k + 2, segments)));
	    // with uvscrolling the length slot carries the segment index and positions move back with every push
	    const float lengthSlot = m_ropeUVScrolling ? static_cast<float> (k) : trailLength;
	    const float position = m_ropeUVScrolling ? static_cast<float> (k) - scroll : static_cast<float> (k);

	    // sub_1402308A0 with a geometry shader: the same record once per segment, without the corner
	    if (m_geometryStage) {
		float* v = &m_vertices[static_cast<size_t> (vertexIndex++) * ROPE_FLOATS_PER_VERTEX];
		const float values[ROPE_FLOATS_PER_VERTEX] = {
		    start.x,  start.y,  start.z,  p.size,  end.x,   end.y,   end.z,   lengthSlot, before.x,
		    before.y, before.z, position, after.x, after.y, after.z, p.size,  color.r,    color.g,
		    color.b,  color.a,  0.0f,     0.0f,    color.r, color.g, color.b, color.a,
		};
		std::copy (std::begin (values), std::end (values), v);
		continue;
	    }

	    const uint32_t baseVertex = vertexIndex;
	    for (const glm::vec2 uv :
		 { glm::vec2 (0.0f, 0.0f), glm::vec2 (1.0f, 0.0f), glm::vec2 (1.0f, 1.0f), glm::vec2 (0.0f, 1.0f) }) {
		float* v = &m_vertices[static_cast<size_t> (vertexIndex++) * ROPE_FLOATS_PER_VERTEX];
		const float values[ROPE_FLOATS_PER_VERTEX] = {
		    start.x,  start.y,  start.z,  p.size,  end.x,   end.y,   end.z,   lengthSlot, before.x,
		    before.y, before.z, position, after.x, after.y, after.z, p.size,  color.r,    color.g,
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

void CParticle::buildRopeSegments (uint32_t& vertexIndex, uint32_t& indexOffset) {
    // sub_1402308A0, the rope: a record per pair of neighbouring particles, their neighbours (clamped at the ends) as
    // the curve's control points, the end values in the THICKFORMAT slots. The UV length is the particle count over
    // uvscale (without the uvscrolling / uvsmoothing flags, not ported). The geometry shader gets it once per segment,
    // without one it is a quad of four corners like the ropetrail's
    const uint32_t count = m_particleCount;
    const float lengthSlot = static_cast<float> (count) / this->ropeUVScale ();
    const auto position = [this, count] (uint32_t index) {
	return this->drawVector (m_particles[std::min (index, count - 1)].position);
    };

    for (uint32_t k = 0; k + 1 < count; k++) {
	const auto& first = m_particles[k];
	const auto& second = m_particles[k + 1];
	const glm::vec3 start = position (k);
	const glm::vec3 end = position (k + 1);
	const glm::vec3 before = position (k == 0 ? 0 : k - 1);
	const glm::vec3 after = position (k + 2);

	const auto write = [&] (const glm::vec2& corner) {
	    float* v = &m_vertices[static_cast<size_t> (vertexIndex++) * ROPE_FLOATS_PER_VERTEX];
	    const float values[ROPE_FLOATS_PER_VERTEX] = {
		start.x,        start.y,      start.z,  first.size,  end.x,          end.y,
		end.z,          lengthSlot,   before.x, before.y,    before.z,       static_cast<float> (k),
		after.x,        after.y,      after.z,  second.size, second.color.r, second.color.g,
		second.color.b, second.alpha, corner.x, corner.y,    first.color.r,  first.color.g,
		first.color.b,  first.alpha,
	    };
	    std::copy (std::begin (values), std::end (values), v);
	};

	if (m_geometryStage) {
	    write (glm::vec2 (0.0f));
	    continue;
	}

	const uint32_t baseVertex = vertexIndex;
	for (const glm::vec2 corner :
	     { glm::vec2 (0.0f, 0.0f), glm::vec2 (1.0f, 0.0f), glm::vec2 (1.0f, 1.0f), glm::vec2 (0.0f, 1.0f) }) {
	    write (corner);
	}
	for (const uint32_t corner : { 0u, 1u, 2u, 2u, 3u, 0u }) {
	    m_indices[indexOffset++] = baseVertex + corner;
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
	this->buildRopeSegments (vertexIndex, indexOffset);
    }

    m_activeIndexCount = static_cast<GLsizei> (m_geometryStage ? vertexIndex : indexOffset);
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

    copyRefractSource ();

    glEnable (GL_DEPTH_CLAMP);
    m_pass->render ();
    glDisable (GL_DEPTH_CLAMP);

#if !NDEBUG
    glPopDebugGroup ();
#endif
}
