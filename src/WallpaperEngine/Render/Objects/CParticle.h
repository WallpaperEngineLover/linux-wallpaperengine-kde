#pragma once

#include "CRenderable.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

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

constexpr uint32_t DEFAULT_MAX_PARTICLES = 1000;

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
    float frame { 0.0f }; // Current animation frame

    float lifetime { 1.0f }; // Total lifetime in seconds
    float age { 0.0f }; // Current age in seconds

    // Oscillator state (per-particle random values)
    // base is updated by alphafade/sizechange operators so oscillation combines properly
    struct {
	float frequency { 0.0f };
	float scale { 1.0f };
	float phase { 0.0f };
	float base { 1.0f };
	bool initialized { false };
    } oscillateAlpha, oscillateSize;

    struct {
	glm::vec3 frequency { 0.0f };
	glm::vec3 scale { 1.0f };
	glm::vec3 phase { 0.0f };
	bool initialized { false };
    } oscillatePosition;

    // Initial values for resets/multipliers
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
    bool isAlive () const { return alive && lifetime != 0.0f && !(lifetime < age); }
};

struct ControlPointData {
    glm::vec3 position { 0.0f };
    /** Rotation and scale of the control point's matrix, emitters orient their shape and velocities with it */
    glm::mat3 orientation { 1.0f };
    glm::vec3 offset { 0.0f };
    bool linkMouse { false };
    bool worldSpace { false };
    /** Follows the parent system's control point parentIndex (child systems only) */
    bool followParent { false };
    /** Flag 8: takes the parent's control point as it is, without moving it into this system's space */
    bool copyUntransformed { false };
    /** Written by a remap component, the engine leaves it alone (WE's loader flag 0x10000) */
    bool remapOutput { false };
    int parentIndex { 0 };
    /** Movement per second over the last update, for inheritcontrolpointvelocity */
    glm::vec3 velocity { 0.0f };
    /** How far it moved since the last update (wallpaper64.exe keeps last frame's matrix at +64) */
    glm::vec3 movement { 0.0f };
    glm::vec3 previousPosition { 0.0f };
    bool hasPreviousPosition { false };
};

using EmitterFunc = std::function<void (std::vector<ParticleInstance>&, uint32_t&, float)>;

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
    void update (float dt);

    [[nodiscard]] const Particle& getParticle () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;
    [[nodiscard]] bool isPlaying () const override;

protected:
    void setupEmitters ();
    void setupInitializers ();
    void setupOperators ();

    EmitterFunc createBoxEmitter (const ParticleEmitter& emitter);
    EmitterFunc createSphereEmitter (const ParticleEmitter& emitter);
    /** Spawn position from the emitter's shape offset and control point, sets m_emitOrientation */
    void placeSpawn (
	ParticleInstance& p, const ControlPointData* cp, int controlPointIndex, const glm::vec3& origin,
	glm::vec3& offset
    );

    [[nodiscard]] float sampleAudio (
	int mode, const glm::vec2& bounds, float exponent, int frequencyStart, int frequencyEnd
    ) const;

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
    /** Layer transform translation in wallpaper64.exe's scene coordinates (remap input layerorigin) */
    [[nodiscard]] glm::vec3 remapLayerOrigin () const;
    /** Control point position in wallpaper64.exe's y-up particle space */
    [[nodiscard]] glm::vec3 controlPointWE (int index) const;
    /** Operators scale some forces by how long frames take (sub_140236CD0): dt * min(1, 0.025 / frame time)^0.7 */
    [[nodiscard]] float frameScaledDelta (float dt) const;
    /** wallpaper64.exe's time since the layer became visible (+1904), the remap input layertime */
    [[nodiscard]] float layerTime () const;

    void renderSprites ();
    void renderRope ();
    void buildRopeTrail (uint32_t& vertexIndex, uint32_t& indexOffset);
    [[nodiscard]] float ropeUVScale () const;
    void setupPass ();
    void setupGeometryCallbacks ();
    void setupParticleUniforms ();
    void updateMatrices ();
    /** The system's origin in the space it's drawn in (centered and y-down for 2D scenes, world for 3D ones) */
    [[nodiscard]] glm::vec3 sceneOrigin () const;
    void syncTransformedOrigin ();
    void updateParticleViewProjection ();
    void updateParticleRenderVars ();

    void prewarm (double now);
    /** base is the matrix the parent leaves for its children, identity for top level systems */
    void draw (const glm::mat4& base);

    /** The object's transform (origin, parallax, angles, scale), what WE keeps at +928 for top level systems */
    [[nodiscard]] glm::mat4 objectMatrix () const;
    /** The matrix stack top WE simulates this system with (sub_140229760 / sub_140229810) */
    void updateFrame ();
    /** wallpaper64.exe sub_14022E3E0 */
    void updateControlPoints ();
    [[nodiscard]] DynamicValue* emitterCountOverride () const;
    /** wallpaper64.exe sub_14022BD40 / sub_14022F890: whether the instanceoverride color applies, and how */
    void refreshColorOverride ();
    /** wallpaper64.exe sub_14022A360: where an event child sits for one of this system's particles */
    [[nodiscard]] glm::mat4 eventPlacement (const ParticleChild& child, const glm::vec3& position) const;

    /** A child system: owned and driven by its parent, placed in the parent's particle space */
    CParticle (CParticle& parent, const ParticleChild& child);
    void setupChildren ();
    void updateChildren (float dt);
    void spawnEventChildren (ParticleChildType type, const ParticleInstance& particle, bool alive);
    void clearEventChildren ();
    void placeChild (const glm::mat4& placement);
    [[nodiscard]] const ParticleInstance* findParticle (uint32_t id) const;
    void passControlPoints (CParticle& child, const ParticleChild& definition) const;
    /** Starts over as a freshly spawned system, for pooled event children */
    void restart ();
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

    // Reused across frames (resized, not reallocated) so renderRope() doesn't heap-allocate every frame
    std::vector<glm::vec3> m_splinePositions;
    std::vector<float> m_splineSizes;
    std::vector<glm::vec4> m_splineColors;
    std::vector<float> m_cumulativeArcLength;

    double m_time { 0.0 };
    bool m_prewarmed { false };

    // Mouse-linked systems run on unscaled real time so cursor trails track 1:1 regardless of --speed
    bool m_hasMouseControlPoint { false };

    Effects::CPass* m_pass { nullptr };
    std::unique_ptr<ImageEffectPassOverride> m_passOverride;
    std::shared_ptr<FBOProvider> m_passFBOProvider;
    TextureMap m_passBinds;
    GLsizei m_activeIndexCount { 0 };

    // REFRACT: copy of scene FBO to avoid read/write conflict
    bool m_hasRefract { false };
    std::shared_ptr<CFBO> m_refractFBO;

    GLuint m_vao { 0 };
    GLuint m_vbo { 0 };
    GLuint m_ebo { 0 };
    GLint m_prevVAO { 0 };

    // Particle-specific uniform data (stored here, pointed to by CPass)
    glm::mat4 m_modelMatrix { 1.0f };
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

    int m_spritesheetCols { 0 };
    int m_spritesheetRows { 0 };
    int m_spritesheetFrames { 0 };
    float m_spritesheetDuration { 1.0f };

    float m_overbright { 1.0f };
    float m_refractAmount { 0.05f }; // Default from shader annotation

    bool m_useTrailRenderer { false };
    float m_trailLength { 0.05f };
    float m_trailMaxLength { 10.0f };
    float m_trailMinLength { 0.0f };
    // Rope renderer (rope + ropetrail both use genericropeparticle shader)
    bool m_useRopeRenderer { false };
    int m_ropeSubdivision { 4 }; // Catmull-Rom subdivisions between points (smoothing)
    int m_ropeSegments { 4 }; // ropetrail: historical position snapshots per particle
    float m_ropeUVScale { 1.0f };
    bool m_ropeUVScrolling { false };
    bool m_ropeUVSmoothing { true }; // rope only
    bool m_uniformLifetimes { false }; // true when lifetime min==max (enables UV smoothing)
    bool m_trailFadeAlpha { false };
    bool m_trailFadeSize { false };
    /** ropetrail: m_ropeSegments past positions per particle slot, newest first, compacted with the particles */
    std::vector<glm::vec3> m_trailHistory;
    /** History entries filled so far and pushes since spawn (uvscrolling) per particle slot */
    std::vector<uint16_t> m_trailCount;
    std::vector<uint16_t> m_trailScroll;
    /** Counts down to the next history push, WE starts it at 0 so the first update pushes */
    float m_trailTimer { 0.0f };
    float m_trailInterval { 1.0f };

    static constexpr int SPRITE_FLOATS_PER_VERTEX = 17;
    static constexpr int ROPE_FLOATS_PER_VERTEX = 26;

    // Screen space to centered space conversion
    glm::vec3 m_transformedOrigin { 0.0f };

    // Last known resolution for detecting changes
    float m_lastScreenWidth { 0.0f };
    float m_lastScreenHeight { 0.0f };

    std::mt19937 m_rng;
    /** Only drawn when an operator needs it, so systems without one keep the same random sequence */
    bool m_usesParticleSeed = false;

    bool m_initialized { false };
    Playback m_lastPlayback { Playback::Playing };

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
    /** The full transform particles are simulated in, the world for world space systems */
    glm::mat4 m_frame { 1.0f };
    /** Particle flags bit 0: particles are kept in world space, moving the system leaves them behind */
    bool m_worldSpace { false };
    /** Orientation of the control point the current emitter spawns from, the velocity initializers apply it */
    glm::mat3 m_emitOrientation { 1.0f };
    /** Event children: the parent's particle that spawned it, while it lives (eventfollow ones move with it) */
    std::optional<uint32_t> m_eventId;
    /** That particle as last seen, what the inherit components read */
    ParticleInstance m_eventParticle;
    bool m_hasEventParticle { false };
    bool m_following { false };
    bool m_emissionStopped { false };
    /** Emission time since (re)start, for telling when every emitter is done */
    float m_emitterTime { 0.0f };

    std::vector<std::unique_ptr<CParticle>> m_staticChildren;
    std::vector<EventChildSlot> m_eventChildren;
    bool m_hasBirthEvents { false };
    bool m_hasDeathEvents { false };
    /** No events while prewarming, like WE */
    bool m_prewarming { false };
    uint32_t m_nextParticleId { 0 };
    /** Pool slots taken by live particles, see ParticleInstance::slot */
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
    /** Top level systems only, see layerTime () */
    float m_layerTime { 0.0f };
    /** Real time between the last two rendered frames and the frame count, for frameScaledDelta and boids */
    float m_frameDelta { 0.0f };
    float m_lastRealTime { 0.0f };
    uint32_t m_frameCounter { 0 };
    struct ColorOverride {
	/** override color given and further than 0.9/255 from the particle file's reference color */
	bool active = false;
	/** spawn color every particle starts from (sub_1401D15A0), the override color itself only in tint mode */
	glm::vec3 tint = glm::vec3 (1.0f);
	glm::vec3 hsv = glm::vec3 (0.0f);
	/** override HSV minus the reference HSV, what colorrandom and colorchange shift their colors by */
	glm::vec3 shift = glm::vec3 (0.0f);
    };
    ColorOverride m_colorOverride {};
    /** System flag 2 in wallpaper64.exe: angularvelocityrandom or angularmovement, angular speed means something */
    bool m_hasAngularVelocity { false };
    /** sub_14023FBC0 restores these from the spawn values before the operators when a remapvalue writes them */
    bool m_resetSizeFromBase { false };
    bool m_resetAlphaFromBase { false };
    bool m_resetColorFromBase { false };
    /** collisionquad needs where particles were before this frame's operators */
    bool m_tracksPreviousPosition { false };
};
} // namespace WallpaperEngine::Render::Objects
