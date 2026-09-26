#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "DynamicValue.h"
#include "Effect.h"
#include "Material.h"
#include "Model.h"
#include "PropertyAnimation.h"
#include "Types.h"
#include "UserSetting.h"
#include "WallpaperEngine/Data/Utils/TypeCaster.h"
#include <memory>

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;

struct ObjectData {
    int id;
    std::string name;
    std::vector<int> dependencies;
    std::optional<int> parent;
    /** Name of a named attachment point on the parent's puppet rig to follow, if any */
    std::optional<std::string> attachment;
    /** Explicit paint-order override (scene.json's "sortorder") - lower draws first (further back).
     *  Falls back to this object's array position when absent. */
    std::optional<int> sortOrder;
    UserSettingUniquePtr origin;
    /** Transform fields for generic scene/group objects. Typed objects keep their own transform fields. */
    UserSettingUniquePtr groupScale;
    UserSettingUniquePtr groupAngles;
    UserSettingUniquePtr groupVisible;
    UserSettingUniquePtr groupParallaxDepth;
    /** Cursor events skip the object entirely when off */
    UserSettingUniquePtr solid;
    /** A visible object hit by the cursor keeps the objects below it from getting cursor events */
    UserSettingUniquePtr disablePropagation;
    /** Drawn with a perspective camera in 2D scenes (general.perspectiveoverridefov), only the object's own flag counts */
    UserSettingUniquePtr perspective;
};

class Object : public TypeCaster, public ObjectData {
public:
    explicit Object (ObjectData data) noexcept : TypeCaster (), ObjectData (std::move (data)) { };
    ~Object () override = default;
};

struct ImageEffectPassOverride {
    int id;
    ComboMap combos;
    ShaderConstantMap constants;
    TextureMap textures;
    TextureMap usertextures;
    std::optional<std::string> shaderOverride; // Overrides MaterialPass::shader when set
};

struct ImageEffect {
    /** Not sure what it's used for */
    int id;
    /** Effect's name for the editor */
    std::string name;
    UserSettingUniquePtr visible;
    std::vector<ImageEffectPassOverrideUniquePtr> passOverrides;
    EffectUniquePtr effect;
};

struct ImageAnimationLayer {
    int id;
    /** Matches the name of a baked animation clip stored in the puppet .mdl's MDLA section */
    std::string name;
    UserSettingUniquePtr rate;
    UserSettingUniquePtr visible;
    UserSettingUniquePtr blend;
    UserSettingUniquePtr animation;
};

enum ImageAlignment {
    ImageAlignment_Center = 0,
    ImageAlignment_Top = 1,
    ImageAlignment_Bottom = 2,
    ImageAlignment_Left = 4,
    ImageAlignment_Right = 8,
};

struct ImageData {
    UserSettingUniquePtr scale;
    UserSettingUniquePtr angles;
    UserSettingUniquePtr visible;
    UserSettingUniquePtr alpha;
    UserSettingUniquePtr color;
    uint32_t alignment;
    /** In pixels */
    glm::vec2 size;
    UserSettingUniquePtr parallaxDepth;
    UserSettingUniquePtr colorBlendMode;
    UserSettingUniquePtr brightness;
    /** Forces UV clamping on this object's composite buffers regardless of the base texture's own flags */
    bool clampUVs;
    /** Passthrough layers only: start from a copy of the scene behind them, otherwise from a transparent buffer */
    UserSettingUniquePtr copyBackground;
    ModelUniquePtr model;
    /** Applied after the material is rendered */
    std::vector<ImageEffectUniquePtr> effects;
    std::vector<ImageAnimationLayerUniquePtr> animationLayers;
};

class Image : public Object, public ImageData {
public:
    explicit Image (ObjectData data, ImageData imageData) noexcept :
	Object (std::move (data)), ImageData (std::move (imageData)) { };
    ~Image () override = default;
};

enum SoundPlaybackMode { PlaybackMode_Single = 0, PlaybackMode_Loop = 1, PlaybackMode_Random = 2 };

struct SoundData {
    SoundPlaybackMode playbackmode;
    std::vector<std::string> sounds;
    /** Per-object volume (0-1), independent of the global volume - lets a wallpaper with several
     *  Sound objects (e.g. alternate music tracks) mute all but one via --set-property */
    UserSettingUniquePtr volume;
    /** Sound stays muted until a script calls play() on its layer */
    std::optional<bool> startsilent;
};

class Sound : public Object, public SoundData {
public:
    explicit Sound (ObjectData data, SoundData soundData) noexcept :
	Object (std::move (data)), SoundData (std::move (soundData)) { };
    ~Sound () override = default;
};

/**
 * Particle control points for forces and positions
 */
struct ParticleControlPoint {
    int id;
    /**
     * 1 follows the cursor, 2 offset is in world space, 4 follows the parent system's control point parentControlPoint,
     * 8 (with 4) copies that control point as it is instead of moving it into this system's space
     */
    uint32_t flags;
    glm::vec3 offset;
    bool lockToPointer;
    int parentControlPoint;
};

/**
 * Particle emitter configuration
 */
struct ParticleEmitter {
    int id;
    std::string name;
    glm::vec3 directions;
    glm::vec3 distanceMin;
    glm::vec3 distanceMax;
    glm::vec3 origin;
    glm::vec3 sign;
    uint32_t instantaneous;
    float speedMin;
    float speedMax;
    float rate;
    int controlPoint;
    uint32_t flags;
    float cone;
    float delay;
    float duration;
    glm::vec2 audioProcessingBounds;
    float audioProcessingExponent;
    int audioProcessingFrequencyStart;
    int audioProcessingFrequencyEnd;
    int audioProcessingMode;
    float minPeriodicDelay;
    float maxPeriodicDelay;
    float minPeriodicDuration;
    float maxPeriodicDuration;
};

/**
 * Particle initializer base and implementations
 */
class ParticleInitializerBase : public TypeCaster {
public:
    virtual ~ParticleInitializerBase () = default;
};

class ColorRandomInitializer : public ParticleInitializerBase {
public:
    ColorRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class SizeRandomInitializer : public ParticleInitializerBase {
public:
    SizeRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class AlphaRandomInitializer : public ParticleInitializerBase {
public:
    AlphaRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class LifetimeRandomInitializer : public ParticleInitializerBase {
public:
    LifetimeRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class VelocityRandomInitializer : public ParticleInitializerBase {
public:
    VelocityRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class RotationRandomInitializer : public ParticleInitializerBase {
public:
    RotationRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class AngularVelocityRandomInitializer : public ParticleInitializerBase {
public:
    AngularVelocityRandomInitializer (
	UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent
    ) : min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class TurbulentVelocityRandomInitializer : public ParticleInitializerBase {
public:
    TurbulentVelocityRandomInitializer (
	UserSettingUniquePtr speedMin, UserSettingUniquePtr speedMax, UserSettingUniquePtr scale,
	UserSettingUniquePtr offset, UserSettingUniquePtr forward, UserSettingUniquePtr timeScale,
	UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax, UserSettingUniquePtr right,
	UserSettingUniquePtr audioProcessingMode, UserSettingUniquePtr audioProcessingBounds,
	UserSettingUniquePtr audioProcessingExponent, UserSettingUniquePtr audioProcessingFrequencyStart,
	UserSettingUniquePtr audioProcessingFrequencyEnd
    ) :
	speedMin (std::move (speedMin)), speedMax (std::move (speedMax)), scale (std::move (scale)),
	offset (std::move (offset)), forward (std::move (forward)), timeScale (std::move (timeScale)),
	phaseMin (std::move (phaseMin)), phaseMax (std::move (phaseMax)), right (std::move (right)),
	audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)),
	audioProcessingExponent (std::move (audioProcessingExponent)),
	audioProcessingFrequencyStart (std::move (audioProcessingFrequencyStart)),
	audioProcessingFrequencyEnd (std::move (audioProcessingFrequencyEnd)) { }
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
    UserSettingUniquePtr scale;
    UserSettingUniquePtr offset;
    UserSettingUniquePtr forward;
    UserSettingUniquePtr timeScale;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr right;
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
    UserSettingUniquePtr audioProcessingExponent;
    UserSettingUniquePtr audioProcessingFrequencyStart;
    UserSettingUniquePtr audioProcessingFrequencyEnd;
};

class MapSequenceAroundControlPointInitializer : public ParticleInitializerBase {
public:
    MapSequenceAroundControlPointInitializer (
	UserSettingUniquePtr controlPoint, UserSettingUniquePtr count, UserSettingUniquePtr speedMin,
	UserSettingUniquePtr speedMax
    ) :
	controlPoint (std::move (controlPoint)), count (std::move (count)), speedMin (std::move (speedMin)),
	speedMax (std::move (speedMax)) { }
    UserSettingUniquePtr controlPoint;
    UserSettingUniquePtr count;
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
};

/** What inheritvaluefromevent/inheritinitialvaluefromevent take from the event's particle (WE's order) */
enum class ParticleEventInput {
    SetColor,
    MultiplyColor,
    SetOpacity,
    MultiplyOpacity,
    SetColorOpacity,
    MultiplyColorOpacity,
    SetVelocity,
    AddVelocity,
    SetSize,
    MultiplySize,
    SetRotation,
    AddRotation,
    SetAngularVelocity,
    AddAngularVelocity,
};

class InheritInitialValueFromEventInitializer : public ParticleInitializerBase {
public:
    explicit InheritInitialValueFromEventInitializer (ParticleEventInput input) : input (input) { }
    ParticleEventInput input;
};

class InheritControlPointVelocityInitializer : public ParticleInitializerBase {
public:
    InheritControlPointVelocityInitializer (int controlPoint, UserSettingUniquePtr min, UserSettingUniquePtr max) :
	controlPoint (controlPoint), min (std::move (min)), max (std::move (max)) { }
    int controlPoint;
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class HsvColorRandomInitializer : public ParticleInitializerBase {
public:
    HsvColorRandomInitializer (
	UserSettingUniquePtr hueMin, UserSettingUniquePtr hueMax, int hueSteps, UserSettingUniquePtr saturationMin,
	UserSettingUniquePtr saturationMax, UserSettingUniquePtr valueMin, UserSettingUniquePtr valueMax
    ) :
	hueMin (std::move (hueMin)), hueMax (std::move (hueMax)), hueSteps (hueSteps),
	saturationMin (std::move (saturationMin)), saturationMax (std::move (saturationMax)),
	valueMin (std::move (valueMin)), valueMax (std::move (valueMax)) { }
    UserSettingUniquePtr hueMin;
    UserSettingUniquePtr hueMax;
    int hueSteps;
    UserSettingUniquePtr saturationMin;
    UserSettingUniquePtr saturationMax;
    UserSettingUniquePtr valueMin;
    UserSettingUniquePtr valueMax;
};

class ColorListInitializer : public ParticleInitializerBase {
public:
    ColorListInitializer (
	std::vector<glm::vec3> colors, UserSettingUniquePtr hueNoise, UserSettingUniquePtr saturationNoise,
	UserSettingUniquePtr valueNoise
    ) :
	colors (std::move (colors)), hueNoise (std::move (hueNoise)), saturationNoise (std::move (saturationNoise)),
	valueNoise (std::move (valueNoise)) { }
    /** RGB as written in the file, entries that don't parse as three numbers are left out like WE does */
    std::vector<glm::vec3> colors;
    UserSettingUniquePtr hueNoise;
    UserSettingUniquePtr saturationNoise;
    UserSettingUniquePtr valueNoise;
};

/** Settings whose default depends on the scene: null means the key wasn't in the file */
class PositionOffsetRandomInitializer : public ParticleInitializerBase {
public:
    PositionOffsetRandomInitializer (
	UserSettingUniquePtr directions, UserSettingUniquePtr sign, UserSettingUniquePtr scale,
	UserSettingUniquePtr distance, UserSettingUniquePtr timeScale, int octaves
    ) :
	directions (std::move (directions)), sign (std::move (sign)), scale (std::move (scale)),
	distance (std::move (distance)), timeScale (std::move (timeScale)), octaves (octaves) { }
    UserSettingUniquePtr directions;
    UserSettingUniquePtr sign;
    UserSettingUniquePtr scale;
    UserSettingUniquePtr distance;
    UserSettingUniquePtr timeScale;
    int octaves;
};

class MapSequenceBetweenControlPointsInitializer : public ParticleInitializerBase {
public:
    MapSequenceBetweenControlPointsInitializer (
	float count, glm::vec2 bounds, bool mirror, int controlPointStart, int controlPointEnd, uint32_t flags,
	UserSettingUniquePtr arcAmount, UserSettingUniquePtr arcDirection, UserSettingUniquePtr sizeReductionAmount
    ) :
	count (count), bounds (bounds), mirror (mirror), controlPointStart (controlPointStart),
	controlPointEnd (controlPointEnd), flags (flags), arcAmount (std::move (arcAmount)),
	arcDirection (std::move (arcDirection)), sizeReductionAmount (std::move (sizeReductionAmount)) { }
    float count;
    glm::vec2 bounds;
    /** limitbehavior "mirror", anything else repeats */
    bool mirror;
    int controlPointStart;
    int controlPointEnd;
    /** 1 shrink the offset from the line, 2 slow down, 4 shrink, 8 arc, 0x10 count follows instanceoverride count */
    uint32_t flags;
    UserSettingUniquePtr arcAmount;
    UserSettingUniquePtr arcDirection;
    UserSettingUniquePtr sizeReductionAmount;
};

/** remapvalue/remapinitialvalue values, WE's enum order */
enum class ParticleRemapValue {
    LifetimeFraction,
    MaxLifetime,
    Size,
    Opacity,
    Speed,
    Rotation,
    AngularSpeed,
    DistanceToControlPoint,
    PositionBetweenTwoControlPoints,
    Runtime,
    TimeOfDay,
    ParticleSystemTime,
    LayerTime,
    Color,
    Position,
    Velocity,
    ControlPoint,
    DeltaToControlPoint,
    DirectionToControlPoint,
    LayerOrigin,
    Unknown,
};

enum class ParticleRemapOperation { Remap, Multiply, Add, Subtract, Unknown };

enum class ParticleRemapComponent { All, X, Y, Z, Sum, Average, Max, Min, Unknown };

enum class ParticleRemapTransform { Linear, Sine, Square, Saw, Triangle, SimplexNoise, FbmNoise, Unknown };

struct ParticleRemap {
    ParticleRemapOperation operation;
    ParticleRemapValue input;
    ParticleRemapValue output;
    ParticleRemapComponent inputComponent;
    ParticleRemapComponent outputComponent;
    ParticleRemapTransform transform;
    /** 1 clamp the input to 0..1, 2 clamp the output to 0..1 */
    uint32_t flags;
    glm::vec3 inputRangeMin;
    glm::vec3 inputRangeMax;
    glm::vec3 outputRangeMin;
    glm::vec3 outputRangeMax;
    int inputControlPoint0;
    int inputControlPoint1;
    int outputControlPoint0;
    int outputControlPoint1;
    float transformInputScale;
    int transformOctaves;
};

class RemapInitialValueInitializer : public ParticleInitializerBase {
public:
    explicit RemapInitialValueInitializer (ParticleRemap remap) : remap (remap) { }
    ParticleRemap remap;
};

using ParticleInitializerUniquePtr = std::unique_ptr<ParticleInitializerBase>;

/**
 * Particle operator base and implementations
 */
class ParticleOperatorBase : public TypeCaster {
public:
    virtual ~ParticleOperatorBase () = default;
};

class MovementOperator : public ParticleOperatorBase {
public:
    MovementOperator (UserSettingUniquePtr drag, UserSettingUniquePtr gravity, uint32_t flags) :
	drag (std::move (drag)), gravity (std::move (gravity)), flags (flags) { }
    UserSettingUniquePtr drag;
    UserSettingUniquePtr gravity;
    /** bit 0: gravity is turned by the system's frame (local systems only) */
    uint32_t flags;
};

class AngularMovementOperator : public ParticleOperatorBase {
public:
    AngularMovementOperator (UserSettingUniquePtr drag, UserSettingUniquePtr force) :
	drag (std::move (drag)), force (std::move (force)) { }
    UserSettingUniquePtr drag;
    UserSettingUniquePtr force;
};

class AlphaFadeOperator : public ParticleOperatorBase {
public:
    AlphaFadeOperator (UserSettingUniquePtr fadeInTime, UserSettingUniquePtr fadeOutTime) :
	fadeInTime (std::move (fadeInTime)), fadeOutTime (std::move (fadeOutTime)) { }
    UserSettingUniquePtr fadeInTime;
    UserSettingUniquePtr fadeOutTime;
};

class SizeChangeOperator : public ParticleOperatorBase {
public:
    SizeChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class AlphaChangeOperator : public ParticleOperatorBase {
public:
    AlphaChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class ColorChangeOperator : public ParticleOperatorBase {
public:
    ColorChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class TurbulenceOperator : public ParticleOperatorBase {
public:
    TurbulenceOperator (
	UserSettingUniquePtr scale, UserSettingUniquePtr speedMin, UserSettingUniquePtr speedMax,
	UserSettingUniquePtr timeScale, UserSettingUniquePtr mask, UserSettingUniquePtr phaseMin,
	UserSettingUniquePtr phaseMax, UserSettingUniquePtr audioProcessingMode,
	UserSettingUniquePtr audioProcessingBounds, UserSettingUniquePtr audioProcessingExponent,
	UserSettingUniquePtr audioProcessingFrequencyStart, UserSettingUniquePtr audioProcessingFrequencyEnd
    ) :
	scale (std::move (scale)), speedMin (std::move (speedMin)), speedMax (std::move (speedMax)),
	timeScale (std::move (timeScale)), mask (std::move (mask)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)), audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)),
	audioProcessingExponent (std::move (audioProcessingExponent)),
	audioProcessingFrequencyStart (std::move (audioProcessingFrequencyStart)),
	audioProcessingFrequencyEnd (std::move (audioProcessingFrequencyEnd)) { }
    UserSettingUniquePtr scale;
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
    UserSettingUniquePtr timeScale;
    UserSettingUniquePtr mask;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
    UserSettingUniquePtr audioProcessingExponent;
    UserSettingUniquePtr audioProcessingFrequencyStart;
    UserSettingUniquePtr audioProcessingFrequencyEnd;
};

class VortexOperator : public ParticleOperatorBase {
public:
    VortexOperator (
	int controlPoint, int flags, UserSettingUniquePtr axis, UserSettingUniquePtr offset,
	UserSettingUniquePtr distanceInner, UserSettingUniquePtr distanceOuter, UserSettingUniquePtr speedInner,
	UserSettingUniquePtr speedOuter, UserSettingUniquePtr centerForce, UserSettingUniquePtr ringRadius,
	UserSettingUniquePtr ringWidth, UserSettingUniquePtr ringPullDistance, UserSettingUniquePtr ringPullForce,
	UserSettingUniquePtr audioProcessingMode, UserSettingUniquePtr audioProcessingBounds,
	UserSettingUniquePtr audioProcessingExponent, UserSettingUniquePtr audioProcessingFrequencyStart,
	UserSettingUniquePtr audioProcessingFrequencyEnd
    ) :
	controlPoint (controlPoint), flags (flags), axis (std::move (axis)), offset (std::move (offset)),
	distanceInner (std::move (distanceInner)), distanceOuter (std::move (distanceOuter)),
	speedInner (std::move (speedInner)), speedOuter (std::move (speedOuter)), centerForce (std::move (centerForce)),
	ringRadius (std::move (ringRadius)), ringWidth (std::move (ringWidth)),
	ringPullDistance (std::move (ringPullDistance)), ringPullForce (std::move (ringPullForce)),
	audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)),
	audioProcessingExponent (std::move (audioProcessingExponent)),
	audioProcessingFrequencyStart (std::move (audioProcessingFrequencyStart)),
	audioProcessingFrequencyEnd (std::move (audioProcessingFrequencyEnd)) { }
    int controlPoint;
    int flags; // 1 = infinite axis, 2 = maintain distance to center, 4 = ring shape
    UserSettingUniquePtr axis;
    UserSettingUniquePtr offset;
    UserSettingUniquePtr distanceInner; // Standard vortex inner radius
    UserSettingUniquePtr distanceOuter; // Standard vortex outer radius
    UserSettingUniquePtr speedInner;
    UserSettingUniquePtr speedOuter;
    UserSettingUniquePtr centerForce; // Strength to pull particles toward center
    UserSettingUniquePtr ringRadius; // Ring mode: radius of the ring
    UserSettingUniquePtr ringWidth; // Ring mode: width of the ring
    UserSettingUniquePtr ringPullDistance; // Ring mode: distance at which ring attracts particles
    UserSettingUniquePtr ringPullForce; // Ring mode: strength of ring attraction
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
    UserSettingUniquePtr audioProcessingExponent;
    UserSettingUniquePtr audioProcessingFrequencyStart;
    UserSettingUniquePtr audioProcessingFrequencyEnd;
};

class InheritValueFromEventOperator : public ParticleOperatorBase {
public:
    InheritValueFromEventOperator (ParticleEventInput input, glm::vec4 blend) : input (input), blend (blend) { }
    ParticleEventInput input;
    /** blendinstart, blendinend, blendoutstart, blendoutend over the particle's life */
    glm::vec4 blend;
};

class ControlPointAttractOperator : public ParticleOperatorBase {
public:
    ControlPointAttractOperator (
	int controlPoint, UserSettingUniquePtr origin, UserSettingUniquePtr scale, UserSettingUniquePtr threshold
    ) :
	controlPoint (controlPoint), origin (std::move (origin)), scale (std::move (scale)),
	threshold (std::move (threshold)) { }
    int controlPoint;
    UserSettingUniquePtr origin;
    UserSettingUniquePtr scale;
    UserSettingUniquePtr threshold;
};

class OscillateAlphaOperator : public ParticleOperatorBase {
public:
    OscillateAlphaOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
};

class OscillateSizeOperator : public ParticleOperatorBase {
public:
    OscillateSizeOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
};

class OscillatePositionOperator : public ParticleOperatorBase {
public:
    OscillatePositionOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax,
	UserSettingUniquePtr mask
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)), mask (std::move (mask)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr mask;
};

/** blendinstart, blendinend, blendoutstart, blendoutend: how much of an operator applies over the particle's life */
struct ParticleBlendWindow {
    float inStart;
    float inEnd;
    float outStart;
    float outEnd;
};

class CapVelocityOperator : public ParticleOperatorBase {
public:
    CapVelocityOperator (UserSettingUniquePtr maxSpeed, ParticleBlendWindow blend) :
	maxSpeed (std::move (maxSpeed)), blend (blend) { }
    UserSettingUniquePtr maxSpeed;
    ParticleBlendWindow blend;
};

class BoidsOperator : public ParticleOperatorBase {
public:
    BoidsOperator (
	UserSettingUniquePtr separationThreshold, UserSettingUniquePtr neighborThreshold, UserSettingUniquePtr maxSpeed,
	UserSettingUniquePtr separationFactor, UserSettingUniquePtr alignmentFactor,
	UserSettingUniquePtr cohesionFactor, uint32_t flags
    ) :
	separationThreshold (std::move (separationThreshold)), neighborThreshold (std::move (neighborThreshold)),
	maxSpeed (std::move (maxSpeed)), separationFactor (std::move (separationFactor)),
	alignmentFactor (std::move (alignmentFactor)), cohesionFactor (std::move (cohesionFactor)), flags (flags) { }
    UserSettingUniquePtr separationThreshold;
    UserSettingUniquePtr neighborThreshold;
    UserSettingUniquePtr maxSpeed;
    UserSettingUniquePtr separationFactor;
    UserSettingUniquePtr alignmentFactor;
    UserSettingUniquePtr cohesionFactor;
    /** 1 cap the speed at maxspeed */
    uint32_t flags;
};

class RemapValueOperator : public ParticleOperatorBase {
public:
    RemapValueOperator (ParticleRemap remap, ParticleBlendWindow blend) : remap (remap), blend (blend) { }
    ParticleRemap remap;
    ParticleBlendWindow blend;
};

class MaintainDistanceToControlPointOperator : public ParticleOperatorBase {
public:
    MaintainDistanceToControlPointOperator (
	int controlPoint, UserSettingUniquePtr distance, UserSettingUniquePtr variableStrength,
	ParticleBlendWindow blend
    ) :
	controlPoint (controlPoint), distance (std::move (distance)), variableStrength (std::move (variableStrength)),
	blend (blend) { }
    int controlPoint;
    UserSettingUniquePtr distance;
    UserSettingUniquePtr variableStrength;
    ParticleBlendWindow blend;
};

class MaintainDistanceBetweenControlPointsOperator : public ParticleOperatorBase {
public:
    MaintainDistanceBetweenControlPointsOperator (
	int controlPointStart, int controlPointEnd, ParticleBlendWindow blend
    ) : controlPointStart (controlPointStart), controlPointEnd (controlPointEnd), blend (blend) { }
    int controlPointStart;
    int controlPointEnd;
    ParticleBlendWindow blend;
};

class ReduceMovementNearControlPointOperator : public ParticleOperatorBase {
public:
    ReduceMovementNearControlPointOperator (
	int controlPoint, UserSettingUniquePtr distanceInner, UserSettingUniquePtr distanceOuter,
	UserSettingUniquePtr reductionInner, UserSettingUniquePtr reductionOuter, ParticleBlendWindow blend
    ) :
	controlPoint (controlPoint), distanceInner (std::move (distanceInner)),
	distanceOuter (std::move (distanceOuter)), reductionInner (std::move (reductionInner)),
	reductionOuter (std::move (reductionOuter)), blend (blend) { }
    int controlPoint;
    UserSettingUniquePtr distanceInner;
    UserSettingUniquePtr distanceOuter;
    UserSettingUniquePtr reductionInner;
    UserSettingUniquePtr reductionOuter;
    ParticleBlendWindow blend;
};

enum class ParticleCollisionShape { Plane, Sphere, Box, Bounds, Quad, Model };

enum class ParticleCollisionBehavior { Bounce, Slide, Stop, Delete };

class CollisionOperator : public ParticleOperatorBase {
public:
    CollisionOperator (
	ParticleCollisionShape shape, ParticleCollisionBehavior behavior, UserSettingUniquePtr bounceFactor,
	uint32_t flags, int controlPoint, UserSettingUniquePtr plane, UserSettingUniquePtr distance,
	UserSettingUniquePtr origin, UserSettingUniquePtr radius, UserSettingUniquePtr forward,
	UserSettingUniquePtr size
    ) :
	shape (shape), behavior (behavior), bounceFactor (std::move (bounceFactor)), flags (flags),
	controlPoint (controlPoint), plane (std::move (plane)), distance (std::move (distance)),
	origin (std::move (origin)), radius (std::move (radius)), forward (std::move (forward)),
	size (std::move (size)) { }
    ParticleCollisionShape shape;
    ParticleCollisionBehavior behavior;
    UserSettingUniquePtr bounceFactor;
    /** 1 the shape moves with the control point, 2 colliding particles stop spinning */
    uint32_t flags;
    int controlPoint;
    /** Shape settings, null when the file doesn't set them (some defaults depend on the scene) */
    UserSettingUniquePtr plane;
    UserSettingUniquePtr distance;
    UserSettingUniquePtr origin;
    UserSettingUniquePtr radius;
    UserSettingUniquePtr forward;
    UserSettingUniquePtr size;
};

using ParticleOperatorUniquePtr = std::unique_ptr<ParticleOperatorBase>;

/**
 * Particle renderer configuration
 */
struct ParticleRenderer {
    std::string name;
    float length;
    float maxLength;
    float minLength;
    float subdivision;
    float segments; // ropetrail: number of history segments per particle
    float uvScale;
    bool uvScrolling;
    bool uvSmoothing; // rope only: reduces flickering when lifetimes are identical
    bool fadeAlpha; // ropetrail: fade alpha along trail
    bool fadeSize; // ropetrail: fade size along trail
};

enum class ParticleChildType {
    /** Always running, placed relative to the parent system */
    Static,
    /** Spawned on a particle's birth and moved along with it until it dies */
    EventFollow,
    /** Spawned where a particle is born */
    EventSpawn,
    /** Spawned where a particle dies */
    EventDeath,
};

/**
 * Child particle system
 */
struct ParticleChild {
    ParticleChildType type;
    /** Particle file the child system is loaded from */
    std::string name;
    /** Most instances of an event child alive at once (static children ignore it) */
    int maxCount;
    /** Chance of an event spawning an instance, rolled per event */
    float probability;
    /** Bit 0: the parent's particles drive the child's control points, from controlPointStartIndex up */
    uint32_t flags;
    int controlPointStartIndex;
    /** Placement relative to the spawning particle (static: the parent system), in the parent's particle space */
    glm::mat4 transform;
    /** null if the file couldn't be loaded */
    ParticleUniquePtr particle;
};

/**
 * Instance override values
 */
struct ParticleInstanceOverride {
    UserSettingUniquePtr enabled;
    UserSettingUniquePtr alpha;
    UserSettingUniquePtr size;
    UserSettingUniquePtr lifetime;
    UserSettingUniquePtr rate;
    UserSettingUniquePtr speed;
    UserSettingUniquePtr count;
    UserSettingUniquePtr color; // Replaces particle color
    UserSettingUniquePtr colorn; // Multiplies particle color
    /** colorn (or the legacy color) was given, WE keeps -1 in it otherwise */
    bool hasColor = false;
};

struct ParticleData {
    UserSettingUniquePtr scale;
    UserSettingUniquePtr angles;
    UserSettingUniquePtr visible;

    UserSettingUniquePtr parallaxDepth;

    std::string particleFile;

    std::string animationMode;
    float sequenceMultiplier;
    uint32_t maxCount;
    float startTime;
    uint32_t flags;

    ModelUniquePtr material;

    std::vector<ParticleEmitter> emitters;
    std::vector<ParticleInitializerUniquePtr> initializers;
    std::vector<ParticleOperatorUniquePtr> operators;
    std::vector<ParticleRenderer> renderers;
    std::vector<ParticleControlPoint> controlPoints;
    std::vector<ParticleChild> children;
    /**
     * Bit per control point slot that a remapvalue/remapinitialvalue writes (output "controlpoint",
     * outputcontrolpoint0). WE's loader flags those 0x10000: the engine stops moving them itself
     */
    uint8_t remapOutputControlPoints = 0;
    /**
     * The color the particle file's own color initializer (or colorchange) centers on, and whether it has one.
     * An instanceoverride color is compared against it (wallpaper64.exe sub_1401C45F0)
     */
    glm::vec3 colorReference = glm::vec3 (1.0f);
    bool hasColor = false;
    /** The override color multiplies the spawn color: no color initializer or a scene older than version 5 */
    bool overrideColorTints = true;

    ParticleInstanceOverride instanceOverride;
};

class Particle : public Object, public ParticleData {
public:
    explicit Particle (ObjectData data, ParticleData particleData) noexcept :
	Object (std::move (data)), ParticleData (std::move (particleData)) { };
    ~Particle () override = default;
};

/**
 * Text object data. Phase 1 of text support covers only static text;
 * dynamic (script-driven) text captures the script source for a future
 * pass but renders whatever initial value the scene provides.
 */
struct TextData {
    /** Initial text content to render (for scripted text, this is the `value` placeholder) */
    UserSettingUniquePtr text;
    /** Font reference from scene (e.g. "fonts/VCR_OSD_MONO.ttf" or "systemfont_arial") */
    std::string font;
    /** Font size in points, optionally bound to a user setting or script */
    UserSettingUniquePtr pointSize;
    /** Bounding box size */
    glm::vec2 size;
    /** Scale (x, y, z) */
    UserSettingUniquePtr scale;
    /** Text color as linear-space RGB */
    UserSettingUniquePtr color;
    UserSettingUniquePtr alpha;
    UserSettingUniquePtr visible;
    UserSettingUniquePtr parallaxDepth;
    /** "left", "center", "right" */
    UserSettingUniquePtr horizontalAlign;
    /** "top", "center", "bottom" */
    UserSettingUniquePtr verticalAlign;
    /** Keeps the text at its distance from a visible screen edge or corner: "none", "center", "top", "topright", ... */
    UserSettingUniquePtr anchor;
    /** Image blend mode, the text is composited through effectpassthrough with BLENDMODE (31 is additive) */
    UserSettingUniquePtr colorBlendMode;
    /** Room around the text box for effects and the opaque background (x = horizontal, y = vertical) */
    UserSettingUniquePtr padding;
    /** Added to every glyph's advance (x) and to the line pitch (y), raster pixels */
    UserSettingUniquePtr spacing;
    /** Applied after the glyphs are rendered */
    std::vector<ImageEffectUniquePtr> effects;
    /** Wrap lines wider than maxWidth (layout pixels, before the object's scale) */
    UserSettingUniquePtr limitWidth;
    UserSettingUniquePtr maxWidth;
    /** Cut the text after maxRows lines, optionally marking the cut with an ellipsis */
    UserSettingUniquePtr limitRows;
    UserSettingUniquePtr maxRows;
    UserSettingUniquePtr limitUseEllipsis;
    /** Justifies wrapped lines to maxWidth by widening their spaces */
    UserSettingUniquePtr blockAlign;
    UserSettingUniquePtr opaqueBackground;
    UserSettingUniquePtr backgroundColor;
    /** Only used by WE's HDR scene rendering */
    UserSettingUniquePtr backgroundBrightness;
    /** Scales the text color, only in HDR scene rendering */
    UserSettingUniquePtr brightness;
    /** Glyphs as multi-channel signed distance fields, forced on by any of the outline/blur/drop shadow effects */
    UserSettingUniquePtr msdf;
    UserSettingUniquePtr outline;
    UserSettingUniquePtr outlineThickness;
    UserSettingUniquePtr outlineColor;
    UserSettingUniquePtr blur;
    UserSettingUniquePtr blurSize;
    UserSettingUniquePtr dropShadow;
    UserSettingUniquePtr dropShadowSize;
    UserSettingUniquePtr dropShadowOpacity;
    UserSettingUniquePtr dropShadowOffset;
    UserSettingUniquePtr dropShadowColor;
};

class Text : public Object, public TextData {
public:
    explicit Text (ObjectData data, TextData textData) noexcept :
	Object (std::move (data)), TextData (std::move (textData)) { };
    ~Text () override = default;
};
/**
 * "point" is the old light that feeds the four fixed slots of genericimage2 and friends
 * (g_LightsPosition/g_LightsColorPremultiplied), the "l" prefixed ones are the newer LightingV1 lights
 */
enum class LightType { Legacy, Point, Spot, Tube, Directional };

struct LightData {
    LightType type;
    UserSettingUniquePtr color;
    UserSettingUniquePtr intensity;
    UserSettingUniquePtr radius;
    UserSettingUniquePtr visible;
    /** Light volumes glowing in the air around the light (LightingV1 lights) */
    bool castVolumetrics = false;
    bool castShadow = false;
    UserSettingUniquePtr density;
    UserSettingUniquePtr volumetricsExponent;
};

class Light : public Object, public LightData {
public:
    explicit Light (ObjectData data, LightData lightData) noexcept :
	Object (std::move (data)), LightData (std::move (lightData)) { };
    ~Light () override = default;
};
/**
 * One entry of a camera object's "path" file: keyframed eye/center/up (per component, missing ones keep the
 * camera's own transform), zoom (2D scenes) and fov (3D scenes), played like a property animation
 */
struct CameraTimeline {
    std::string name;
    bool visible = true;
    float fps = 30.0f;
    /** Whole frames */
    float length = 0.0f;
    PropertyAnimation::Mode mode = PropertyAnimation::Mode::Single;
    bool startPaused = false;
    std::array<std::vector<AnimationKeyframe>, 3> center;
    std::array<std::vector<AnimationKeyframe>, 3> eye;
    std::array<std::vector<AnimationKeyframe>, 3> up;
    std::vector<AnimationKeyframe> zoom;
    std::vector<AnimationKeyframe> fov;
};

enum class CameraQueueMode { Random = 0, Sequential = 1 };

/** A "camera" object, the last visible one drives the scene's view (fov only in 3D scenes) */
struct SceneCameraData {
    UserSettingUniquePtr fov;
    UserSettingUniquePtr zoom;
    std::vector<CameraTimeline> timelines;
    CameraQueueMode queueMode = CameraQueueMode::Random;
};

class SceneCamera : public Object, public SceneCameraData {
public:
    explicit SceneCamera (ObjectData data, SceneCameraData cameraData) noexcept :
	Object (std::move (data)), SceneCameraData (std::move (cameraData)) { };
    ~SceneCamera () override = default;
};
/** A 3D model object ("model" pointing at a .mdl), placed with the base object's origin/scale/angles */
struct MeshData {
    std::string model;
};

class Mesh : public Object, public MeshData {
public:
    explicit Mesh (ObjectData data, MeshData meshData) noexcept :
	Object (std::move (data)), MeshData (std::move (meshData)) { };
    ~Mesh () override = default;
};
} // namespace WallpaperEngine::Data::Model
