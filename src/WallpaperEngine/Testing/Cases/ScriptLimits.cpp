#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/PropertyAnimation.h"
#include "WallpaperEngine/Scripting/AnimationSystem.h"
#include "WallpaperEngine/Scripting/DecimalPrefix.h"
#include "WallpaperEngine/Scripting/LimitedAllocator.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Scripting;

namespace {
std::shared_ptr<const PropertyAnimation> ramp (float from, float to) {
    auto animation = std::make_shared<PropertyAnimation> ();

    animation->curves[0]
	= { AnimationKeyframe { .frame = 0.0f, .value = from }, AnimationKeyframe { .frame = 30.0f, .value = to } };
    animation->fps = 30.0f;
    animation->length = 30.0f;
    animation->mode = PropertyAnimation::Mode::Loop;

    return animation;
}
}

TEST_CASE ("A script's ArrayBuffers stop at the limit") {
    const auto allocator = makeLimitedAllocator (1000);

    void* first = allocator->Allocate (600);
    REQUIRE (first != nullptr);

    // 600 + 600 and 600 + 500 are both past it, from either allocation call
    CHECK (allocator->Allocate (600) == nullptr);
    CHECK (allocator->AllocateUninitialized (500) == nullptr);

    void* second = allocator->AllocateUninitialized (400);
    CHECK (second != nullptr);
    CHECK (allocator->Allocate (1) == nullptr);

    // freed memory counts again
    allocator->Free (first, 600);
    void* third = allocator->Allocate (600);
    CHECK (third != nullptr);

    allocator->Free (second, 400);
    allocator->Free (third, 600);
}

TEST_CASE ("No limit lets everything through") {
    const auto allocator = makeLimitedAllocator (0);
    void* big = allocator->Allocate (8 * 1024 * 1024);

    CHECK (big != nullptr);
    allocator->Free (big, 8 * 1024 * 1024);
}

TEST_CASE ("An allocation the system refuses gives its share back") {
    // within the limit but more than any machine has, calloc says no
    constexpr size_t huge = size_t (1) << 61;
    const auto allocator = makeLimitedAllocator (huge + 4096);

    CHECK (allocator->Allocate (huge) == nullptr);

    // the share of the refused one would still be held and the second of these would be past the limit
    void* first = allocator->Allocate (3000);
    void* second = allocator->Allocate (3000);

    CHECK (first != nullptr);
    CHECK (second != nullptr);

    allocator->Free (first, 3000);
    allocator->Free (second, 3000);
}

TEST_CASE ("Removing an object takes its timelines out of the animation system") {
    DynamicValue alpha (0.0f);
    DynamicValue effectConstant (0.0f);
    DynamicValue layerRate (0.0f);
    DynamicValue otherObject (0.0f);
    DynamicValue similarId (0.0f);
    DynamicValue similarLayer (0.0f);

    for (auto* value : { &alpha, &effectConstant, &layerRate, &otherObject, &similarId, &similarLayer }) {
	value->setAnimation (ramp (10.0f, 20.0f));
    }

    AnimationSystem system;

    system.add ("obj7", "alpha", alpha);
    system.add ("obj7/fx0.p0.", "amount", effectConstant);
    system.add ("animationlayer7[0]", "rate", layerRate);
    system.add ("obj8", "alpha", otherObject);
    // ids that only start like 7
    system.add ("obj70", "alpha", similarId);
    system.add ("animationlayer70[0]", "rate", similarLayer);

    system.tick (0.5f);

    for (const auto* value : { &alpha, &effectConstant, &layerRate, &otherObject, &similarId, &similarLayer }) {
	REQUIRE (system.clockOf (*value) != nullptr);
    }

    const float before = alpha.getFloat ();
    const float otherBefore = otherObject.getFloat ();

    system.removeObject (7);

    CHECK (system.clockOf (alpha) == nullptr);
    CHECK (system.clockOf (effectConstant) == nullptr);
    CHECK (system.clockOf (layerRate) == nullptr);
    CHECK (system.clockOf (otherObject) != nullptr);
    CHECK (system.clockOf (similarId) != nullptr);
    CHECK (system.clockOf (similarLayer) != nullptr);

    // what is left keeps running, what went isn't written any more
    system.tick (0.5f);

    CHECK_THAT (alpha.getFloat (), Catch::Matchers::WithinULP (before, 0));
    CHECK_THAT (otherObject.getFloat (), !Catch::Matchers::WithinULP (otherBefore, 0));

    // nothing registered under that id: nothing to do
    system.removeObject (999);
    CHECK (system.clockOf (otherObject) != nullptr);
}

TEST_CASE ("Removing a value from the animation system lets go of it everywhere") {
    DynamicValue origin (0.0f);
    DynamicValue scale (0.0f);
    DynamicValue other (0.0f);
    // before the system, which still holds it when the section ends
    DynamicValue replacement (0.0f);

    auto child = std::make_shared<PropertyAnimation> (*ramp (1.0f, 2.0f));

    child->parent = "origin";
    origin.setAnimation (ramp (10.0f, 20.0f));
    scale.setAnimation (child);
    other.setAnimation (ramp (5.0f, 6.0f));

    AnimationSystem system;

    system.add ("obj1", "origin", origin);
    system.add ("obj1", "scale", scale);
    system.add ("obj2", "alpha", other);
    system.tick (0.5f);

    REQUIRE (system.clockOf (origin) != nullptr);
    REQUIRE (system.clockOf (scale) == system.clockOf (origin));

    SECTION ("a root that goes leaves its dependents a timeline of their own") {
	system.remove (origin);
	system.tick (0.5f);

	CHECK (system.clockOf (origin) == nullptr);
	REQUIRE (system.clockOf (scale) != nullptr);
	CHECK (system.clockOf (other) != nullptr);

	// the value that replaces the root takes the key and gets its own timeline, the dependent keeps running on its
	// own
	replacement.setAnimation (ramp (30.0f, 40.0f));
	system.add ("obj1", "origin", replacement);
	system.tick (0.5f);

	const auto* replacementClock = system.clockOf (replacement);
	auto* scaleClock = system.clockOf (scale);

	REQUIRE (replacementClock != nullptr);
	REQUIRE (scaleClock != nullptr);
	CHECK (scaleClock != replacementClock);

	const float frame = scaleClock->getFrame ();

	system.tick (0.1f);
	CHECK (scaleClock->getFrame () != frame);
    }

    SECTION ("a root replaced before the timelines are linked again takes its dependents along") {
	system.remove (origin);

	replacement.setAnimation (ramp (30.0f, 40.0f));
	system.add ("obj1", "origin", replacement);

	const auto* replacementClock = system.clockOf (replacement);

	REQUIRE (replacementClock != nullptr);
	CHECK (system.clockOf (scale) == replacementClock);
	CHECK_THAT (replacementClock->getFrame (), Catch::Matchers::WithinULP (0.0f, 0));
    }

    SECTION ("the state a dependent takes over survives other removals before the next link") {
	system.remove (origin);
	system.remove (other);
	system.removeObject (2);
	system.removeObject (3);

	const auto* clock = system.clockOf (scale);

	REQUIRE (clock != nullptr);
	CHECK_THAT (clock->getFrame (), Catch::Matchers::WithinAbs (15.0f, 0.001f));
    }

    SECTION ("a dependent that goes leaves the root running") {
	system.remove (scale);
	system.tick (0.5f);

	CHECK (system.clockOf (scale) == nullptr);
	CHECK (system.clockOf (origin) != nullptr);
    }

    SECTION ("a dependent freed before the timelines are linked again leaves no state for its address") {
	system.remove (origin);
	system.remove (scale);

	system.add ("obj3", "scale", scale);

	const auto* clock = system.clockOf (scale);

	REQUIRE (clock != nullptr);
	CHECK_THAT (clock->getFrame (), Catch::Matchers::WithinULP (0.0f, 0));
    }

    SECTION ("an object removed before the timelines are linked again leaves no state either") {
	system.remove (origin);
	system.removeObject (1);

	system.add ("obj3", "scale", scale);

	const auto* clock = system.clockOf (scale);

	REQUIRE (clock != nullptr);
	CHECK_THAT (clock->getFrame (), Catch::Matchers::WithinULP (0.0f, 0));
    }

    SECTION ("a value nobody registered changes nothing") {
	DynamicValue stranger (0.0f);

	system.remove (stranger);
	CHECK (system.clockOf (origin) != nullptr);
	CHECK (system.clockOf (scale) != nullptr);
    }
}

TEST_CASE ("A timeline taken over from a longer one stays inside its own length") {
    DynamicValue value (0.0f);

    value.setAnimation (ramp (10.0f, 20.0f));

    AnimationClock clock (1, value, ramp (10.0f, 20.0f));

    clock.restore ({ .frame = 80.0f, .rate = 2.0f, .direction = 1.0f, .playing = true, .ended = false });

    CHECK_THAT (clock.getFrame (), Catch::Matchers::WithinULP (30.0f, 0));
    CHECK_THAT (clock.getRate (), Catch::Matchers::WithinULP (2.0f, 0));
    CHECK (clock.isPlaying ());
}

TEST_CASE ("A script's string for a vector is read like parseFloat") {
    const auto read = [] (const char* text, float& number, const char** rest = nullptr) {
	const char* position = text;
	const bool found = WallpaperEngine::Scripting::readDecimalPrefix (position, number);

	if (rest != nullptr) {
	    *rest = position;
	}

	return found;
    };

    float number = 0.0f;
    const char* rest = nullptr;

    CHECK (read ("1.5 2", number, &rest));
    CHECK (number == 1.5f);
    CHECK (std::string_view (rest) == " 2");

    CHECK (read ("  \t-3e2x", number, &rest));
    CHECK (number == -300.0f);
    CHECK (std::string_view (rest) == "x");

    CHECK (read ("\f\v 5", number));
    CHECK (number == 5.0f);
    CHECK (read ("+.5", number));
    CHECK (number == 0.5f);
    CHECK (read ("7e", number, &rest));
    CHECK (number == 7.0f);
    CHECK (std::string_view (rest) == "e");

    // hex, other words and nothing at all are no numbers
    CHECK (read ("0x10", number, &rest));
    CHECK (number == 0.0f);
    CHECK (std::string_view (rest) == "x10");
    CHECK_FALSE (read ("inf", number));
    CHECK_FALSE (read ("nan", number));
    CHECK_FALSE (read ("", number));
    CHECK_FALSE (read (".", number));
    CHECK_FALSE (read ("-", number));
    CHECK_FALSE (read ("+-5", number));

    CHECK (read ("Infinity 0 0", number, &rest));
    CHECK (std::isinf (number));
    CHECK (number > 0.0f);
    CHECK (std::string_view (rest) == " 0 0");
    CHECK (read ("-Infinity", number));
    CHECK (std::isinf (number));
    CHECK (number < 0.0f);

    // too big or too small for the type: Infinity or zero by the decimal magnitude, never the value the caller had
    number = 5.0f;
    CHECK (read ("1e999", number));
    CHECK (std::isinf (number));
    CHECK (number > 0.0f);
    CHECK (read ("-1e999", number));
    CHECK (std::isinf (number));
    CHECK (number < 0.0f);
    number = 5.0f;
    CHECK (read ("1e-999", number));
    CHECK (number == 0.0f);
    CHECK (read ("1e40", number));
    CHECK (std::isinf (number));

    const std::string zeros (400, '0');

    CHECK (read (("0." + zeros + "1").c_str (), number));
    CHECK (number == 0.0f);
    CHECK (read (("1" + zeros + "e-5").c_str (), number));
    CHECK (std::isinf (number));
    CHECK (read (("0." + zeros + "1e5").c_str (), number));
    CHECK (number == 0.0f);
}

TEST_CASE ("A timeline that takes over from a removed root starts where it was") {
    DynamicValue origin (0.0f);
    DynamicValue scale (0.0f);
    auto child = std::make_shared<PropertyAnimation> (*ramp (1.0f, 2.0f));

    child->parent = "origin";
    origin.setAnimation (ramp (10.0f, 20.0f));
    scale.setAnimation (child);

    AnimationSystem system;

    system.add ("obj1", "origin", origin);
    system.add ("obj1", "scale", scale);
    system.tick (0.5f);

    const float frame = system.clockOf (origin)->getFrame ();

    REQUIRE (frame > 0.0f);

    system.remove (origin);
    system.tick (0.0f);

    REQUIRE (system.clockOf (scale) != nullptr);
    CHECK (system.clockOf (scale)->getFrame () == frame);
}
