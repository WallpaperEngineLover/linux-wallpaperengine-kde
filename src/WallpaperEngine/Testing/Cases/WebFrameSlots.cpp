#include "WallpaperEngine/WebBrowser/CEF/SharedMemoryRenderHandler.h"
#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"

#undef CHECK
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace WallpaperEngine::WebBrowser;

namespace {
constexpr uint32_t WIDTH = 8;
constexpr uint32_t HEIGHT = 4;

// both mappings of the segment, like CWeb and runWebHost
struct Segment {
    Segment () {
	main = IPC::createSharedMemory (name, WIDTH, HEIGHT);
	host = IPC::attachSharedMemory (name, WIDTH, HEIGHT);
    }

    ~Segment () {
	IPC::closeSharedMemory (host, name, WIDTH, HEIGHT, false);

	if (main != nullptr) {
	    IPC::closeSharedMemory (main, name, WIDTH, HEIGHT, true);
	}
    }

    void releaseMain () {
	IPC::closeSharedMemory (main, name, WIDTH, HEIGHT, true);
	main = nullptr;
    }

    std::string name;
    IPC::WebHostSharedMemory* main = nullptr;
    IPC::WebHostSharedMemory* host = nullptr;
};

void paint (CEF::SharedMemoryRenderHandler& handler, uint8_t value, int width = WIDTH, int height = HEIGHT) {
    const std::vector<uint8_t> pixels (static_cast<size_t> (width) * height * 4, value);
    handler.OnPaint (nullptr, PET_VIEW, {}, pixels.data (), width, height);
}
} // namespace

TEST_CASE ("Web frames hand over through three slots that are never shared") {
    Segment segment;
    REQUIRE (segment.main != nullptr);
    REQUIRE (segment.host != nullptr);

    CefRefPtr<CEF::SharedMemoryRenderHandler> handler = new CEF::SharedMemoryRenderHandler (segment.host);
    // CWeb starts on slot 2, the host on 0, slot 1 sits in between
    uint32_t front = 2;

    CHECK_FALSE (segment.main->takeFrame (front).has_value ());

    paint (*handler, 10);

    auto taken = segment.main->takeFrame (front);
    REQUIRE (taken.has_value ());
    front = *taken;
    CHECK (segment.main->frameBuffer (front)[0] == 10);
    CHECK (segment.main->slotWidth[front].load () == WIDTH);
    CHECK_FALSE (segment.main->takeFrame (front).has_value ());

    // two frames published: the newer one is taken, the older slot returns to the host
    paint (*handler, 20);
    paint (*handler, 30);

    taken = segment.main->takeFrame (front);
    REQUIRE (taken.has_value ());
    front = *taken;
    CHECK (segment.main->frameBuffer (front)[0] == 30);

    for (uint8_t value = 40; value < 100; value += 10) {
	paint (*handler, value);

	// the host never paints into the held front slot
	CHECK (segment.main->frameBuffer (front)[0] == 30);
    }

    taken = segment.main->takeFrame (front);
    REQUIRE (taken.has_value ());
    CHECK (segment.main->frameBuffer (*taken)[0] == 90);

    const uint32_t between = segment.main->frameSlot.load () & IPC::WebHostSharedMemory::FRAME_SLOT_MASK;
    CHECK (between == front);
    CHECK (between != *taken);
}

TEST_CASE ("Web paints that don't fit the segment are dropped") {
    Segment segment;
    CefRefPtr<CEF::SharedMemoryRenderHandler> handler = new CEF::SharedMemoryRenderHandler (segment.host);
    const uint32_t before = segment.main->frameSlot.load ();

    paint (*handler, 1, WIDTH + 1, HEIGHT);
    paint (*handler, 1, WIDTH, HEIGHT * 2);
    paint (*handler, 1, 0, HEIGHT);

    const std::vector<uint8_t> pixels (WIDTH * HEIGHT * 4, 1);
    handler->OnPaint (nullptr, PET_POPUP, {}, pixels.data (), WIDTH, HEIGHT);

    CHECK (segment.main->frameSlot.load () == before);

    // sizes must be nonzero and fit the segment
    CefRect rect;
    segment.main->desiredWidth = 0;
    segment.main->desiredHeight = 0;
    handler->GetViewRect (nullptr, rect);
    CHECK (rect.width == 1);
    CHECK (rect.height == 1);

    segment.main->desiredWidth = 4000;
    segment.main->desiredHeight = 3;
    handler->GetViewRect (nullptr, rect);
    CHECK (rect.width == static_cast<int> (WIDTH));
    CHECK (rect.height == 3);
}

TEST_CASE ("The web host keeps painting after the main process let go of the segment") {
    Segment segment;
    CefRefPtr<CEF::SharedMemoryRenderHandler> handler = new CEF::SharedMemoryRenderHandler (segment.host);

    segment.main->desiredWidth = WIDTH;
    segment.main->desiredHeight = HEIGHT;
    // CEF may still paint after CWeb unmaps and unlinks
    segment.releaseMain ();

    paint (*handler, 50);
    paint (*handler, 60);

    CefRect rect;
    handler->GetViewRect (nullptr, rect);
    CHECK (rect.width == static_cast<int> (WIDTH));

    const auto slot = segment.host->takeFrame (2);
    REQUIRE (slot.has_value ());
    CHECK (segment.host->frameBuffer (*slot)[0] == 60);
}

TEST_CASE ("The main process takes frame limits from its own sizes, not the segment's header") {
    Segment segment;
    REQUIRE (segment.main != nullptr);

    using Shm = IPC::WebHostSharedMemory;

    CHECK (Shm::frameFits (0, WIDTH, HEIGHT, WIDTH, HEIGHT));
    CHECK (Shm::frameFits (Shm::FRAME_SLOTS - 1, 1, 1, WIDTH, HEIGHT));
    CHECK_FALSE (Shm::frameFits (Shm::FRAME_SLOTS, WIDTH, HEIGHT, WIDTH, HEIGHT));
    CHECK_FALSE (Shm::frameFits (3, 1, 1, WIDTH, HEIGHT));
    CHECK_FALSE (Shm::frameFits (0, WIDTH + 1, 1, WIDTH, HEIGHT));
    CHECK_FALSE (Shm::frameFits (0, 1, HEIGHT + 1, WIDTH, HEIGHT));
    CHECK_FALSE (Shm::frameFits (0, 0xFFFFFFFFu, 0xFFFFFFFFu, WIDTH, HEIGHT));

    const uint8_t* const honest = segment.main->frameBuffer (2, WIDTH, HEIGHT);
    segment.host->maxWidth = 0x40000000;
    segment.host->maxHeight = 0x40000000;
    CHECK (segment.main->frameBuffer (2, WIDTH, HEIGHT) == honest);
}
