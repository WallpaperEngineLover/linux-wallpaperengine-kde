#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <functional>

#include "WallpaperEngine/Data/Model/DynamicValue.h"

using WallpaperEngine::Data::Model::DynamicValue;

TEST_CASE ("A listener may remove itself while its value propagates") {
    DynamicValue source (1.0f);
    std::function<void ()> remove;
    int first = 0;
    int second = 0;

    remove = source.listen ([&] (const DynamicValue&, DynamicValue::UpdateSource) {
	first++;
	remove ();
    });
    source.listen ([&] (const DynamicValue&, DynamicValue::UpdateSource) { second++; });

    source.update (2.0f, DynamicValue::Script);
    source.update (3.0f, DynamicValue::Script);

    CHECK (first == 1);
    CHECK (second == 2);
}

TEST_CASE ("A connected value stops following once it is gone") {
    DynamicValue source (1.0f);

    {
	DynamicValue follower (0.0f);

	follower.connect (&source);
	CHECK_THAT (follower.getFloat (), Catch::Matchers::WithinULP (1.0f, 0));
    }

    source.update (5.0f, DynamicValue::User);
    CHECK_THAT (source.getFloat (), Catch::Matchers::WithinULP (5.0f, 0));
}
