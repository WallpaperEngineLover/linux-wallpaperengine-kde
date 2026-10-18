#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/PropertyAnimation.h"
#include "WallpaperEngine/Scripting/AnimationSystem.h"
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
} // namespace

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

    CHECK (alpha.getFloat () == before);
    CHECK (otherObject.getFloat () != otherBefore);

    // nothing registered under that id: nothing to do
    system.removeObject (999);
    CHECK (system.clockOf (otherObject) != nullptr);
}
