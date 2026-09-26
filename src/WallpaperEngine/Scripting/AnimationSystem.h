#pragma once

#include <glm/vec4.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/PropertyAnimation.h"

namespace WallpaperEngine::Scripting {
using namespace WallpaperEngine::Data::Model;

/** A keyframe curve at a whole frame, wallpaper64.exe's sub_1401A9BC0 */
float sampleAnimationFrame (const std::vector<AnimationKeyframe>& keys, int frame);
/** A keyframe curve at a fractional frame: the two whole frames around it, blended (sub_140171440) */
float evaluateAnimationCurve (const std::vector<AnimationKeyframe>& keys, float frame, float fps, int frameCount);

/**
 * One playback timeline shared by a root animated property and every property that lists it as
 * its parent (e.g. an origin animation driving the scale one, or a shader's Radius driving Size).
 */
class AnimationClock {
public:
    struct Binding {
	DynamicValue* value;
	std::shared_ptr<const PropertyAnimation> data;
	glm::vec4 lastOffset { 0.0f };
    };

    struct FiredEvent {
	std::string name;
	float frame;
    };

    AnimationClock (int id, DynamicValue& rootValue, std::shared_ptr<const PropertyAnimation> definition);

    [[nodiscard]] int getId () const { return m_id; }
    [[nodiscard]] DynamicValue& getRootValue () const { return *m_rootValue; }
    [[nodiscard]] const PropertyAnimation& getDefinition () const { return *m_definition; }
    [[nodiscard]] float getFrame () const { return m_frame; }
    [[nodiscard]] bool isPlaying () const { return m_playing; }
    [[nodiscard]] float getRate () const { return m_rate; }

    void addBinding (DynamicValue& value, std::shared_ptr<const PropertyAnimation> data);
    void play () { m_playing = true; }
    void pause () { m_playing = false; }
    void stop ();
    void setRate (float rate) { m_rate = rate; }
    void setFrame (float frame);
    /** Advances the timeline, applies it to every bound value and returns the events it crossed */
    std::vector<FiredEvent> tick (float deltaSeconds);
    void applyCurrentFrame ();

private:
    void collectEvents (float from, float to, bool includeFrom, std::vector<FiredEvent>& out) const;
    void applyBinding (Binding& binding) const;

    int m_id;
    DynamicValue* m_rootValue;
    std::shared_ptr<const PropertyAnimation> m_definition;
    std::vector<Binding> m_bindings;
    float m_frame = 0.0f;
    float m_rate = 1.0f;
    // only ever flips for mirror mode
    float m_direction = 1.0f;
    bool m_playing = true;
};

class AnimationSystem {
public:
    struct PendingEvent {
	int clock;
	std::string name;
	float frame;
    };

    AnimationSystem ();
    ~AnimationSystem ();
    AnimationSystem (const AnimationSystem&) = delete;
    AnimationSystem& operator= (const AnimationSystem&) = delete;

    [[nodiscard]] int getId () const { return m_id; }
    static AnimationSystem* find (int id);

    /**
     * Registers an animated property. `group` scopes the parent/children links between siblings
     * (one object's transform properties, or one effect pass's constants).
     */
    void add (const std::string& group, const std::string& key, DynamicValue& value);
    /** Drops a not-yet-linked registration, used when a later registration supersedes it */
    void remove (const DynamicValue& value);
    void tick (float deltaSeconds);
    std::vector<PendingEvent> takeEvents ();
    AnimationClock* clockOf (const DynamicValue& value);
    AnimationClock* clock (int id);

private:
    struct Entry {
	std::string group;
	std::string key;
	DynamicValue* value;
    };

    void link ();

    int m_id;
    bool m_linked = false;
    std::vector<Entry> m_pending;
    std::map<std::string, std::map<std::string, DynamicValue*>> m_groups;
    std::set<const DynamicValue*> m_bound;
    std::vector<std::unique_ptr<AnimationClock>> m_clocks;
    std::map<const DynamicValue*, AnimationClock*> m_byValue;
    std::vector<PendingEvent> m_events;
};
} // namespace WallpaperEngine::Scripting
