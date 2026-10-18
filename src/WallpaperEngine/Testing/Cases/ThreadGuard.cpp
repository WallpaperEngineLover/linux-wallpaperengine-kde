#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <thread>

#include "WallpaperEngine/Logging/ThreadGuard.h"

using WallpaperEngine::Logging::guardThread;

TEST_CASE ("A thread that throws ends alone instead of the process") {
    bool ran = false;

    std::thread failing ([&ran] {
	guardThread ("Test thread", [&ran] {
	    ran = true;
	    throw std::runtime_error ("boom");
	});
    });
    failing.join ();

    CHECK (ran);

    // not a std::exception either
    std::thread odd ([] { guardThread ("Test thread", [] { throw 42; }); });
    odd.join ();
}

TEST_CASE ("A thread that does not throw runs its body once") {
    int count = 0;

    std::thread quiet ([&count] { guardThread ("Test thread", [&count] { count++; }); });
    quiet.join ();

    CHECK (count == 1);
}
