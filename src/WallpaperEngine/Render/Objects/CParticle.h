#pragma once

#include "CRenderable.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include "WallpaperEngine/Data/Utils/FloatCompare.h"
#include <array>
#include <deque>
#include <functional>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <optional>
#include <random>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

namespace WallpaperEngine::Render::Objects {
class CImage;
class PuppetRig;

constexpr uint32_t DEFAULT_MAX_PARTICLES = 1000;
// no installed system has more than 100000, a count a file claims past this would only be an allocation
constexpr uint32_t MAX_PARTICLES = 1000000;
// the vertex data a system may hold (400 MB), a rope trail keeps several quads per particle
constexpr size_t MAX_VERTEX_FLOATS = 100000000;

struct ParticleInstance {
    glm::vec3 position { 0.0f };
    /** Where the particle was when this frame's operators started, collisionquad tests the step in between */
    glm::vec3 previousPosition { 0.0f };
    glm::vec3 velocity { 0.0f };
    glm::vec3 acceleration { 0.0f };

    glm::vec3 rotation { 0.0f };
    glm::vec3 angularVelocity { 0.0f };
    glm::vec3 angularAcceleration { 0.0f };

    glm::vec3 color { 1.0f };
    float alpha { 1.0f };
    float size { 20.0f };
    float frame { 0.0f };

    float lifetime { 1.0f };
    float age { 0.0f };

    struct {
	glm::vec3 color { 1.0f };
	float alpha { 1.0f };
	float size { 20.0f };
	float lifetime { 1.0f };
    } initial;

    /** Random 0..1 picked at spawn, WE's turbulence operator uses it for the particle's phase and speed */
    float seed { 0.0f };

    /** Unique per system, eventfollow children find the particle they follow by it (indices shift on compaction) */
    uint32_t id { 0 };
    /** WE's pool slot: the lowest one free at spawn, what orders particles handed to child control points */
    uint32_t slot { 0 };

    bool alive { false };

    float getLifetimePos () const { return lifetime > 0.0f ? (age / lifetime) : 1.0f; }

    // sub_140236CD0 kills a particle once lifetime < age, so it is still drawn on the frame it reaches its lifetime
    bool isAlive () const { return alive && WallpaperEngine::Data::Utils::isNonZero (lifetime) && !(lifetime < age); }
};

struct ControlPointData {
    glm::vec3 position { 0.0f };
    glm::mat3 orientation { 1.0f };
    glm::vec3 offset { 0.0f };
    bool linkMouse { false };
    bool worldSpace { false };
    bool followParent { false };
    /** Flag 8: takes the parent's control point as it is, without moving it into this system's space */
    bool copyUntransformed { false };
    /** Written by a remap component, the engine leaves it alone (WE's loader flag 0x10000) */
    bool remapOutput { false };
    int parentIndex { 0 };
    glm::vec3 velocity { 0.0f };
    /** How far it moved since the last update (wallpaper64.exe keeps last frame's matrix at +64) */
    glm::vec3 movement { 0.0f };
    glm::vec3 previousPosition { 0.0f };
    bool hasPreviousPosition { false };
};

using EmitterFunc = std::function<void (std::vector<ParticleInstance>&, uint32_t&, float)>;

/** An emitter's timers, the runtime part of its record in wallpaper64.exe sub_1402378A0 */
struct EmitterClock {
    float delay { 0.0f };
    /** Counts down while > 0, -1 once it ran out */
    float duration { 0.0f };
    float accumulator { 0.0f };
    /** Periodic emitters: > 0 emitting for that long, < 0 waiting */
    float period { 0.0f };
    int emittedInPeriod { 0 };
    uint32_t pendingBurst { 0 };
    bool finished { false };
};

using InitializerFunc = std::function<void (ParticleInstance&)>;

using OperatorFunc = std::function<
    void (std::vector<ParticleInstance>&, uint32_t, const std::vector<ControlPointData>&, float, float)>;

class CParticle final : public CRenderable, public Scripting::ScriptableObject {
    friend CObject;

public:
    CParticle (Wallpapers::CScene& scene, const Particle& particle);
    ~CParticle ();

    void setup () override;
    void render () override;
    /** sub_140230650, before scripts */
    void simulate ();
    void update (float dt);

    [[nodiscard]] const Particle& getParticle () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;
    [[nodiscard]] bool isPlaying () const override;
    void applyPlayback (Playback playback) override;
    /** IParticleSystem.emitParticles (sub_14024CAC0): every emitter spawns count more right away, 0 is one */
    void emitParticles (int count);
    /** Emitter clocks finished (sub_14022F640) or running again (sub_14022F5B0), static children too */
    void finishEmitters ();
    void resumeEmitters ();

    // IParticleSystemInstance (instance override at +1912, sub_14024D940) beyond the scene's instanceoverride: colorn
    // -1 and control point x FLT_MAX until written (sub_14024D760); angle picks controlpointangleN
    [[nodiscard]] glm::vec3 getInstanceColor () const;
    void setInstanceColor (const glm::vec3& color);
    [[nodiscard]] glm::vec3 getInstanceControlPoint (size_t index, bool angle) const;
    void setInstanceControlPoint (size_t index, bool angle, const glm::vec3& value);

protected:
    void setupEmitters ();
    void setupInitializers ();
    void setupOperators ();

    EmitterFunc createBoxEmitter (const ParticleEmitter& emitter);
    EmitterFunc createSphereEmitter (const ParticleEmitter& emitter);
    uint32_t emitCount (EmitterClock& clock, const ParticleEmitter& emitter, float dt);
    void placeSpawn (
	ParticleInstance& p, const ControlPointData* cp, int controlPointIndex, const glm::vec3& origin,
	glm::vec3& offset
    );

    [[nodiscard]] float
    sampleAudio (int mode, const glm::vec2& bounds, float exponent, int frequencyStart, int frequencyEnd) const;

    InitializerFunc createColorRandomInitializer (const ColorRandomInitializer& init);
    InitializerFunc createSizeRandomInitializer (const SizeRandomInitializer& init);
    InitializerFunc createAlphaRandomInitializer (const AlphaRandomInitializer& init);
    InitializerFunc createLifetimeRandomInitializer (const LifetimeRandomInitializer& init);
    InitializerFunc createVelocityRandomInitializer (const VelocityRandomInitializer& init);
    InitializerFunc createRotationRandomInitializer (const RotationRandomInitializer& init);
    InitializerFunc createAngularVelocityRandomInitializer (const AngularVelocityRandomInitializer& init);
    InitializerFunc createTurbulentVelocityRandomInitializer (const TurbulentVelocityRandomInitializer& init);
    InitializerFunc
    createMapSequenceAroundControlPointInitializer (const MapSequenceAroundControlPointInitializer& init);
    InitializerFunc createInheritInitialValueFromEventInitializer (const InheritInitialValueFromEventInitializer& init);
    InitializerFunc createInheritControlPointVelocityInitializer (const InheritControlPointVelocityInitializer& init);
    InitializerFunc createHsvColorRandomInitializer (const HsvColorRandomInitializer& init);
    InitializerFunc createColorListInitializer (const ColorListInitializer& init);
    InitializerFunc createPositionOffsetRandomInitializer (const PositionOffsetRandomInitializer& init);
    InitializerFunc
    createMapSequenceBetweenControlPointsInitializer (const MapSequenceBetweenControlPointsInitializer& init);
    InitializerFunc createRemapInitialValueInitializer (const RemapInitialValueInitializer& init);

    OperatorFunc createMovementOperator (const MovementOperator& op);
    OperatorFunc createAngularMovementOperator (const AngularMovementOperator& op);
    OperatorFunc createAlphaFadeOperator (const AlphaFadeOperator& op);
    OperatorFunc createSizeChangeOperator (const SizeChangeOperator& op);
    OperatorFunc createAlphaChangeOperator (const AlphaChangeOperator& op);
    OperatorFunc createColorChangeOperator (const ColorChangeOperator& op);
    OperatorFunc createTurbulenceOperator (const TurbulenceOperator& op);
    OperatorFunc createVortexOperator (const VortexOperator& op);
    OperatorFunc createControlPointAttractOperator (const ControlPointAttractOperator& op);
    OperatorFunc createOscillateAlphaOperator (const OscillateAlphaOperator& op);
    OperatorFunc createOscillateSizeOperator (const OscillateSizeOperator& op);
    OperatorFunc createOscillatePositionOperator (const OscillatePositionOperator& op);
    OperatorFunc createInheritValueFromEventOperator (const InheritValueFromEventOperator& op);
    OperatorFunc createCapVelocityOperator (const CapVelocityOperator& op);
    OperatorFunc createBoidsOperator (const BoidsOperator& op);
    OperatorFunc createRemapValueOperator (const RemapValueOperator& op);
    OperatorFunc createMaintainDistanceToControlPointOperator (const MaintainDistanceToControlPointOperator& op);
    OperatorFunc
    createMaintainDistanceBetweenControlPointsOperator (const MaintainDistanceBetweenControlPointsOperator& op);
    OperatorFunc createReduceMovementNearControlPointOperator (const ReduceMovementNearControlPointOperator& op);
    OperatorFunc createCollisionOperator (const CollisionOperator& op);

    /** A remap input as wallpaper64.exe reads it during the simulation (sub_14023FBC0 case 19), in its y-up space */
    [[nodiscard]] glm::vec3 remapOperatorInput (const ParticleRemap& remap, const ParticleInstance& p) const;
    /** The same for remapinitialvalue (sub_14023B340 case 15), which reads the particle's base values */
    [[nodiscard]] glm::vec3 remapInitialInput (const ParticleRemap& remap, ParticleInstance& p);
    [[nodiscard]] glm::vec3 remapLayerOrigin () const;
    [[nodiscard]] glm::mat4 localControlPointMatrix (size_t index) const;
    /** Operators scale some forces by how long frames take (sub_140236CD0): dt * min(1, 0.025 / frame time)^0.7 */
    [[nodiscard]] float frameScaledDelta (float dt) const;
    /** wallpaper64.exe's time since the layer became visible (+1904), the remap input layertime */
    [[nodiscard]] float layerTime () const;

    void renderSprites ();
    void renderRope ();
    /** REFRACT: fills the _rt_FullFrameBuffer copy right before drawing, like WE's copy of the bound target */
    void copyRefractSource () const;
    void buildRopeTrail (uint32_t& vertexIndex, uint32_t& indexOffset);
    void buildRopeSegments (uint32_t& vertexIndex, uint32_t& indexOffset);

    struct CollisionCapsule {
	glm::vec3 start;
	glm::vec3 direction;
	float length;
	float radius;
    };
    [[nodiscard]] std::vector<CollisionCapsule> collisionCapsules (int index) const;
    [[nodiscard]] glm::mat4 worldToParticles () const;
    [[nodiscard]] const CObject* componentDependency (const char* type, int index) const;

    /** A layerimage pixel: its colour and its place in layer units around the layer's center, y down */
    struct ImagePixel {
	uint8_t r = 0, g = 0, b = 0;
	int16_t x = 0, y = 0;
	/** puppet layers, 0xFF none */
	uint8_t bone = 0xFF;
    };
    /** sub_1401D3AE0 */
    struct ImageEmitter {
	int index = 0;
	std::vector<ImagePixel> pixels;
	/** first call only sets layer flags 0x1010 */
	bool flagged { false };
	bool requested { false };
	uint32_t requestFrame { 0 };
	/** 0 nothing, 1 downsampled into staging, 2 pixels ready */
	int stage { 0 };
	std::vector<ImagePixel> staged;
	glm::ivec2 size { 0 };
	glm::ivec2 stagedSize { 0 };
	float refreshTimer { 0.0f };
	glm::mat4 world { 1.0f };
	glm::mat4 previousWorld { 1.0f };
	/** only if already loaded at setup */
	std::shared_ptr<const TextureProvider> mask;
	std::vector<glm::mat4> boneInverseBind;
    };
    std::vector<ImageEmitter> m_imageEmitters;
    EmitterFunc createImageEmitter (const ParticleEmitter& emitter);
    bool readImagePixels (ImageEmitter& state, const CObject& layer);
    void downsampleLayerBuffer (ImageEmitter& state, const CFBO& buffer);
    static void assignImagePixelBones (ImageEmitter& state, const PuppetRig& rig);
    OperatorFunc createCollisionModelOperator (const CollisionOperator& op);
    /** collisionmodel components set up so far, each one reads the dependency record with its index */
    int m_collisionModels { 0 };
    [[nodiscard]] float ropeUVScale () const;
    void setupPass ();
    void setupGeometryCallbacks ();
    void setupParticleUniforms ();
    void updateMatrices ();
    void updateParticleViewProjection ();
    /** g_OrientationForward/Right/Up from the renderer's orientation (sub_1402298B0) */
    void updateOrientation ();
    void updateParticleRenderVars ();

    void prewarm (double now);
    /** base is the matrix the parent leaves for its children, identity for top level systems */
    void draw (const glm::mat4& base);

    /** The object's transform (origin, parallax, angles, scale), what WE keeps at +928 for top level systems */
    [[nodiscard]] glm::mat4 objectMatrix () const;
    /** WE's world to the camera's space: the 2D scene space, identity in 3D */
    [[nodiscard]] glm::mat4 sceneMatrix () const;
    [[nodiscard]] bool isShown () const;
    /** The matrix stack top WE simulates this system with (sub_140229760 / sub_140229810) */
    void updateFrame ();
    /** wallpaper64.exe sub_14022E3E0 */
    void updateControlPoints ();
    [[nodiscard]] DynamicValue* emitterCountOverride () const;
    /** wallpaper64.exe sub_14022BD40 / sub_14022F890: whether the instanceoverride color applies, and how */
    void refreshColorOverride ();
    /** wallpaper64.exe sub_14022A360: where an event child sits for one of this system's particles */
    [[nodiscard]] glm::mat4 eventPlacement (const ParticleChild& child, const glm::vec3& position) const;

    CParticle (CParticle& parent, const ParticleChild& child);
    void setupChildren ();
    void updateChildren (float dt);
    void spawnEventChildren (ParticleChildType type, const ParticleInstance& particle, bool alive);
    void clearEventChildren ();
    void placeChild (const glm::mat4& placement);
    [[nodiscard]] const ParticleInstance* findParticle (uint32_t id) const;
    void passControlPoints (CParticle& child, const ParticleChild& definition) const;
    void restart ();
    /** sub_14022F6C0 + sub_14022F5B0: timers, sequences and emitters back to their start, particles are kept */
    void restartEmission ();
    /** sub_14022F790, a periodic emitter starts a new period */
    void startEmitterPeriod ();
    [[nodiscard]] bool isFinished () const;
    [[nodiscard]] bool emittersExhausted () const;

private:
    const Particle& m_particle;

    std::vector<ParticleInstance> m_particles;
    uint32_t m_particleCount { 0 };
    uint32_t m_maxParticles { DEFAULT_MAX_PARTICLES };

    std::vector<EmitterFunc> m_emitters;
    std::vector<InitializerFunc> m_initializers;
    std::vector<OperatorFunc> m_operators;

    std::vector<ControlPointData> m_controlPoints;

    std::vector<float> m_vertices;
    std::vector<uint32_t> m_indices;

    double m_time { 0.0 };
    bool m_prewarmed { false };

    // Mouse-linked systems run on unscaled real time so cursor trails track 1:1 regardless of --speed
    bool m_hasMouseControlPoint { false };

    Effects::CPass* m_pass { nullptr };
    std::unique_ptr<ImageEffectPassOverride> m_passOverride;
    std::shared_ptr<FBOProvider> m_passFBOProvider;
    TextureMap m_passBinds;
    GLsizei m_activeIndexCount { 0 };

    // REFRACT: copy of the scene FBO, it can't be read while drawn into
    bool m_hasRefract { false };
    std::shared_ptr<CFBO> m_refractFBO;

    GLuint m_vao { 0 };
    GLuint m_vbo { 0 };
    GLuint m_ebo { 0 };
    GLint m_prevVAO { 0 };

    // m_worldModelMatrix in sceneMatrix () space, CPass points at these
    glm::mat4 m_modelMatrix { 1.0f };
    glm::mat4 m_worldModelMatrix { 1.0f };
    glm::mat4 m_modelMatrixInverse { 1.0f };
    glm::mat4 m_mvpMatrix { 1.0f };
    glm::mat4 m_mvpMatrixInverse { 1.0f };
    glm::mat4 m_viewProjectionMatrix { 1.0f };
    glm::vec3 m_orientationUp { 0.0f, 1.0f, 0.0f };
    glm::vec3 m_orientationRight { 1.0f, 0.0f, 0.0f };
    glm::vec3 m_orientationForward { 0.0f, 0.0f, 1.0f };
    glm::vec3 m_viewUp { 0.0f, 1.0f, 0.0f };
    glm::vec3 m_viewRight { 1.0f, 0.0f, 0.0f };
    glm::vec3 m_eyePosition { 0.0f, 0.0f, 1000.0f };
    glm::vec4 m_renderVar0 { 0.0f };
    glm::vec4 m_renderVar1 { 0.0f };

    int m_spritesheetFrames { 0 };

    float m_overbright { 1.0f };
    float m_refractAmount { 0.05f };

    bool m_useTrailRenderer { false };
    float m_trailLength { 0.05f };
    float m_trailMaxLength { 10.0f };
    float m_trailMinLength { 0.0f };
    bool m_useRopeRenderer { false };
    int m_ropeSubdivision { 4 };
    int m_ropeSegments { 4 };
    float m_ropeUVScale { 1.0f };
    bool m_ropeUVScrolling { false };
    uint32_t m_ropeDeaths { 0 };
    bool m_trailFadeAlpha { false };
    bool m_trailFadeSize { false };
    /** ropetrail: m_ropeSegments past positions per particle slot, newest first, compacted with the particles */
    std::vector<glm::vec3> m_trailHistory;
    std::vector<uint16_t> m_trailCount;
    std::vector<uint16_t> m_trailScroll;
    /** Counts down to the next history push, WE starts it at 0 so the first update pushes */
    float m_trailTimer { 0.0f };
    float m_trailInterval { 1.0f };

    static constexpr int SPRITE_FLOATS_PER_VERTEX = 17;
    static constexpr int ROPE_FLOATS_PER_VERTEX = 26;
    bool m_geometryStage { false };

    std::mt19937 m_rng;
    /** Only drawn when an operator needs it, so systems without one keep the same random sequence */
    bool m_usesParticleSeed = false;

    bool m_initialized { false };

    struct EventChildSlot {
	const ParticleChild* child;
	std::vector<std::unique_ptr<CParticle>> active;
	std::vector<std::unique_ptr<CParticle>> pool;
    };

    CParticle* m_parent { nullptr };
    const ParticleChild* m_childDefinition { nullptr };
    /**
     * WE's +928 matrix. Top level: the object's transform. Static children: the child transform. Event children:
     * the spawning particle's position and the child transform, in the parent's space or the world (eventPlacement)
     */
    glm::mat4 m_placement { 1.0f };
    glm::mat4 m_frame { 1.0f };
    bool m_worldSpace { false };
    glm::mat3 m_emitOrientation { 1.0f };
    std::optional<uint32_t> m_eventId;
    ParticleInstance m_eventParticle;
    bool m_hasEventParticle { false };
    bool m_following { false };
    bool m_emissionStopped { false };
    /** Particles emitParticles () adds to what each emitter spawns, sub_1402378A0's count argument */
    uint32_t m_forcedEmission { 0 };
    bool m_scriptColor { false };
    std::array<std::optional<glm::vec3>, 8> m_scriptControlPoints {};
    std::array<std::optional<glm::vec3>, 8> m_scriptControlPointAngles {};

    void registerNewParticles (uint32_t firstNew);
    /** Child systems inherit the root's pause (wallpaper64.exe copies +0x3F7 down) */
    [[nodiscard]] bool emissionPaused () const;
    std::vector<EmitterClock> m_emitterClocks;

    /** Where a mapsequence initializer is in its sequence, WE keeps it in the initializer's record (+4, +8) */
    struct SequenceCounter {
	float step = 0.0f;
	float sequence { 0.0f };
	/** Starts over with every new emitter period (mapsequencebetweencontrolpoints flag 0x20) */
	bool periodReset { false };
    };
    std::deque<SequenceCounter> m_sequences;

    std::vector<std::unique_ptr<CParticle>> m_staticChildren;
    std::vector<EventChildSlot> m_eventChildren;
    bool m_hasBirthEvents { false };
    bool m_hasDeathEvents { false };
    /** No events while prewarming, like WE */
    bool m_prewarming { false };
    uint32_t m_nextParticleId { 0 };
    std::vector<uint8_t> m_slotUsed;
    std::vector<uint32_t> m_births;
    std::vector<ParticleInstance> m_deaths;

    /** Slots handed out since the last restart, wallpaper64.exe's particle count (+832), the boids sample stride */
    uint32_t m_slotExtent { 0 };
    /**
     * wallpaper64.exe never clears a dead pool slot: movement keeps integrating it and boids still reads it as a
     * neighbor (see createBoidsOperator). Kept per slot for systems with boids, the only operator that reads others
     */
    std::vector<ParticleInstance> m_ghosts;
    std::vector<uint8_t> m_ghostUsed;
    bool m_hasBoids { false };
    /** wallpaper64.exe +1008: simulated time since the system started, the remapinitialvalue particlesystemtime */
    float m_systemTime { 0.0f };
    float m_layerTime { 0.0f };
    float m_frameDelta { 0.0f };
    float m_lastRealTime { 0.0f };
    struct ColorOverride {
	/** override color given and further than 0.9/255 from the particle file's reference color */
	bool active = false;
	/** spawn color every particle starts from (sub_1401D15A0), the override color itself only in tint mode */
	glm::vec3 tint = glm::vec3 (1.0f);
	glm::vec3 hsv = glm::vec3 (0.0f);
	glm::vec3 shift = glm::vec3 (0.0f);
    };
    ColorOverride m_colorOverride {};
    /** System flag 2 in wallpaper64.exe: angularvelocityrandom or angularmovement, angular speed means something */
    bool m_hasAngularVelocity { false };
    /** sub_14023FBC0 restores these from the spawn values before the operators when a remapvalue writes them */
    bool m_resetSizeFromBase { true };
    bool m_resetAlphaFromBase { false };
    bool m_resetColorFromBase { false };
    bool m_tracksPreviousPosition { false };
};
}
