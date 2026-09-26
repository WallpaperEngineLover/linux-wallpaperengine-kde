#include "AnimationSystem.h"

#include <algorithm>
#include <cmath>

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Scripting;

namespace {
int NextSystemId = 0;
std::map<int, AnimationSystem*> Systems;

int componentCount (DynamicValue::UnderlyingType type) {
    switch (type) {
	case DynamicValue::Float:
	    return 1;
	case DynamicValue::Vec2:
	    return 2;
	case DynamicValue::Vec3:
	    return 3;
	case DynamicValue::Vec4:
	    return 4;
	default:
	    return 0;
    }
}

glm::vec4 readComponents (const DynamicValue& value) {
    switch (value.getType ()) {
	case DynamicValue::Float:
	    return { value.getFloat (), 0.0f, 0.0f, 0.0f };
	case DynamicValue::Vec2:
	    return { value.getVec2 (), 0.0f, 0.0f };
	case DynamicValue::Vec3:
	    return { value.getVec3 (), 0.0f };
	case DynamicValue::Vec4:
	    return value.getVec4 ();
	default:
	    return glm::vec4 (0.0f);
    }
}

void writeComponents (DynamicValue& value, const glm::vec4& components) {
    const auto source = DynamicValue::UpdateSource::Script;

    switch (value.getType ()) {
	case DynamicValue::Float:
	    value.update (components.x, source);
	    break;
	case DynamicValue::Vec2:
	    value.update (glm::vec2 (components), source);
	    break;
	case DynamicValue::Vec3:
	    value.update (glm::vec3 (components), source);
	    break;
	case DynamicValue::Vec4:
	    value.update (components, source);
	    break;
	default:
	    break;
    }
}

float cubicBezier (float p0, float p1, float p2, float p3, float t) {
    const float u = 1.0f - t;
    return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t * p3;
}
} // namespace

float WallpaperEngine::Scripting::sampleAnimationFrame (const std::vector<AnimationKeyframe>& keys, const int frame) {
    if (keys.empty ()) {
	return 0.0f;
    }

    if (static_cast<float> (frame) <= keys.front ().frame) {
	return keys.front ().value;
    }

    for (size_t i = 1; i < keys.size (); i++) {
	const auto& previous = keys[i - 1];
	const auto& current = keys[i];
	const int start = static_cast<int> (previous.frame);
	const int end = static_cast<int> (current.frame);

	if (frame < start || frame >= end) {
	    continue;
	}

	if (start == frame || current.step) {
	    return previous.value;
	}

	// handle x is in half segments. The curve parameter is found by halving steps from an integer
	// division guess, like the 0.01 frame tolerance and 1000 step limit of sub_1401A9BC0
	const float half = static_cast<float> (end - start) * 0.5f;
	const float x1 = half * previous.front.x + static_cast<float> (start);
	const float x2 = half * current.back.x + static_cast<float> (end);
	const float target = static_cast<float> (frame);
	float t = static_cast<float> ((frame - start) / (end - start));
	float step = 0.999f;

	for (int iteration = 0; iteration < 1000; iteration++) {
	    const float x = cubicBezier (static_cast<float> (start), x1, x2, static_cast<float> (end), t);

	    if (std::fabs (x - target) < 0.01f) {
		break;
	    }

	    step *= 0.5f;
	    t = x <= target ? t + step : t - step;
	}

	t = std::clamp (t, 0.0f, 1.0f);

	return cubicBezier (
	    previous.value, previous.value + previous.front.y, current.value + current.back.y, current.value, t
	);
    }

    return keys.back ().value;
}

float WallpaperEngine::Scripting::evaluateAnimationCurve (
    const std::vector<AnimationKeyframe>& keys, const float frame, const float fps, const int frameCount
) {
    if (keys.empty ()) {
	return 0.0f;
    }

    if (frameCount <= 0 || fps <= 0.0f) {
	return sampleAnimationFrame (keys, static_cast<int> (frame));
    }

    // whole frames get sampled and blended, the blend comes from the time in seconds (sub_140171440)
    const float frameTime = 1.0f / fps;
    const float seconds = frame * frameTime;
    const int whole = static_cast<int> (seconds / frameTime);
    const int first = std::clamp (whole, 0, frameCount - 1);
    const int second = std::min (first + 1, frameCount);
    const float blend = std::fmod (seconds, frameTime) / frameTime;

    return sampleAnimationFrame (keys, first) * (1.0f - blend) + sampleAnimationFrame (keys, second) * blend;
}

AnimationClock::AnimationClock (
    int id, DynamicValue& rootValue, std::shared_ptr<const PropertyAnimation> definition
) : m_id (id), m_rootValue (&rootValue), m_definition (std::move (definition)) {
    m_playing = !m_definition->startPaused;
}

void AnimationClock::addBinding (DynamicValue& value, std::shared_ptr<const PropertyAnimation> data) {
    m_bindings.push_back (Binding { .value = &value, .data = std::move (data) });
}

void AnimationClock::stop () {
    m_playing = false;
    this->setFrame (0.0f);
}

void AnimationClock::setFrame (float frame) {
    m_frame = std::clamp (frame, 0.0f, std::max (m_definition->length, 0.0f));
    this->applyCurrentFrame ();
}

void AnimationClock::applyCurrentFrame () {
    for (auto& binding : m_bindings) {
	this->applyBinding (binding);
    }
}

void AnimationClock::applyBinding (Binding& binding) const {
    const int count = componentCount (binding.value->getType ());

    if (count == 0) {
	return;
    }

    const glm::vec4 current = readComponents (*binding.value);
    glm::vec4 target = current;

    for (int component = 0; component < count; component++) {
	const auto& keys = binding.data->curves[component];

	if (keys.empty ()) {
	    continue;
	}

	const float sampled = evaluateAnimationCurve (
	    keys, m_frame, m_definition->fps, static_cast<int> (m_definition->length)
	);

	if (binding.data->relative) {
	    // fold the offset in on top of whatever the value is now, so a script moving the base
	    // value around keeps working
	    target[component] = current[component] - binding.lastOffset[component] + sampled;
	    binding.lastOffset[component] = sampled;
	} else {
	    target[component] = sampled;
	}
    }

    if (target != current) {
	writeComponents (*binding.value, target);
    }
}

void AnimationClock::collectEvents (float from, float to, bool includeFrom, std::vector<FiredEvent>& out) const {
    const bool forward = to >= from;
    std::vector<const AnimationEvent*> hits;

    for (const auto& event : m_definition->events) {
	const bool inside = forward ? (includeFrom ? event.frame >= from : event.frame > from) && event.frame <= to
				    : (includeFrom ? event.frame <= from : event.frame < from) && event.frame >= to;

	if (inside) {
	    hits.push_back (&event);
	}
    }

    std::ranges::stable_sort (hits, [forward] (const AnimationEvent* a, const AnimationEvent* b) {
	return forward ? a->frame < b->frame : a->frame > b->frame;
    });

    for (const auto* hit : hits) {
	out.push_back (FiredEvent { .name = hit->name, .frame = hit->frame });
    }
}

std::vector<AnimationClock::FiredEvent> AnimationClock::tick (float deltaSeconds) {
    std::vector<FiredEvent> fired;
    const float length = m_definition->length;

    if (!m_playing || length <= 0.0f) {
	return fired;
    }

    const float step = deltaSeconds * m_definition->fps * m_rate * m_direction;

    if (step == 0.0f) {
	return fired;
    }

    float from = m_frame;
    float to = from + step;
    bool forward = step > 0.0f;
    bool includeFrom = false;

    // a long frame can cross the end several times; anything past this is just phase
    for (int wraps = 0; wraps < 4; wraps++) {
	const bool pastEnd = forward ? to >= length : to <= 0.0f;

	if (!pastEnd) {
	    break;
	}

	const float edge = forward ? length : 0.0f;
	this->collectEvents (from, edge, includeFrom, fired);
	includeFrom = true;

	if (m_definition->mode == PropertyAnimation::Mode::Single) {
	    m_frame = edge;
	    m_playing = false;
	    this->applyCurrentFrame ();
	    return fired;
	}

	if (m_definition->mode == PropertyAnimation::Mode::Loop) {
	    to = forward ? to - length : to + length;
	    from = forward ? 0.0f : length;
	} else {
	    to = forward ? 2.0f * length - to : -to;
	    from = edge;
	    forward = !forward;
	    m_direction = -m_direction;
	}
    }

    to = std::clamp (to, 0.0f, length);
    this->collectEvents (from, to, includeFrom, fired);
    m_frame = to;
    this->applyCurrentFrame ();

    return fired;
}

AnimationSystem::AnimationSystem () : m_id (++NextSystemId) { Systems.emplace (m_id, this); }

AnimationSystem::~AnimationSystem () { Systems.erase (m_id); }

AnimationSystem* AnimationSystem::find (int id) {
    const auto it = Systems.find (id);

    return it == Systems.end () ? nullptr : it->second;
}

void AnimationSystem::add (const std::string& group, const std::string& key, DynamicValue& value) {
    if (value.getAnimation () == nullptr) {
	return;
    }

    m_pending.push_back (Entry { .group = group, .key = key, .value = &value });
    m_linked = false;
}

void AnimationSystem::remove (const DynamicValue& value) {
    std::erase_if (m_pending, [&value] (const Entry& entry) { return entry.value == &value; });
}

AnimationClock* AnimationSystem::clockOf (const DynamicValue& value) {
    if (!m_linked) {
	this->link ();
    }

    const auto it = m_byValue.find (&value);

    return it == m_byValue.end () ? nullptr : it->second;
}

AnimationClock* AnimationSystem::clock (int id) {
    for (const auto& clock : m_clocks) {
	if (clock->getId () == id) {
	    return clock.get ();
	}
    }

    return nullptr;
}

void AnimationSystem::link () {
    m_linked = true;

    for (const auto& entry : m_pending) {
	m_groups[entry.group].emplace (entry.key, entry.value);
    }

    // follows parent links to the property that owns the playback state
    const auto resolveRoot = [this] (const Entry& entry) {
	DynamicValue* current = entry.value;

	for (int depth = 0; depth < 8; depth++) {
	    const auto& parentKey = current->getAnimation ()->parent;

	    if (!parentKey.has_value ()) {
		break;
	    }

	    const auto& siblings = m_groups[entry.group];
	    const auto parent = siblings.find (*parentKey);

	    if (parent == siblings.end () || parent->second == current || parent->second->getAnimation () == nullptr) {
		break;
	    }

	    current = parent->second;
	}

	return current;
    };

    static int nextClockId = 0;

    for (const auto& entry : m_pending) {
	DynamicValue* root = resolveRoot (entry);

	if (m_byValue.contains (root)) {
	    continue;
	}

	auto clock = std::make_unique<AnimationClock> (++nextClockId, *root, root->getAnimation ());
	m_byValue.emplace (root, clock.get ());
	m_clocks.push_back (std::move (clock));
    }

    std::set<AnimationClock*> touched;

    for (const auto& entry : m_pending) {
	if (!m_bound.insert (entry.value).second) {
	    continue;
	}

	auto* clock = m_byValue.at (resolveRoot (entry));
	clock->addBinding (*entry.value, entry.value->getAnimation ());
	m_byValue.emplace (entry.value, clock);
	touched.insert (clock);
    }

    // paused timelines still show their first frame, which is what hides collapsed UI at startup
    for (auto* clock : touched) {
	clock->applyCurrentFrame ();
    }

    m_pending.clear ();
}

void AnimationSystem::tick (float deltaSeconds) {
    if (!m_linked) {
	this->link ();
    }

    for (const auto& clock : m_clocks) {
	for (auto& event : clock->tick (deltaSeconds)) {
	    m_events.push_back (PendingEvent { .clock = clock->getId (), .name = std::move (event.name), .frame = event.frame });
	}
    }
}

std::vector<AnimationSystem::PendingEvent> AnimationSystem::takeEvents () {
    std::vector<PendingEvent> events;
    events.swap (m_events);

    return events;
}
