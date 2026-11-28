#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Desktop/WindowState.h"

#include <map>
#include <string>
#include <vector>

using namespace WallpaperEngine::Desktop;

TEST_CASE ("A window is on the outputs it mostly covers, or the one it sits on", "[outputcoverage]") {
    const std::map<std::string, glm::ivec4> outputs
	= { { "HDMI-0", { 0, 370, 2560, 1080 } }, { "DP-0", { 2560, 0, 1080, 1920 } } };

    CHECK (outputsCoveredBy ({ 0, 370, 2560, 1080 }, outputs) == std::vector<std::string> { "HDMI-0" });
    CHECK (outputsCoveredBy ({ 2560, 0, 1080, 1920 }, outputs) == std::vector<std::string> { "DP-0" });
    CHECK (outputsCoveredBy ({ 0, 0, 3640, 1920 }, outputs) == std::vector<std::string> { "DP-0", "HDMI-0" });
    CHECK (outputsCoveredBy ({ 100, 400, 800, 600 }, outputs) == std::vector<std::string> { "HDMI-0" });
    CHECK (outputsCoveredBy ({ 2300, 500, 400, 300 }, outputs) == std::vector<std::string> { "HDMI-0" });
    CHECK (outputsCoveredBy ({ 2400, 500, 400, 300 }, outputs) == std::vector<std::string> { "DP-0" });
    CHECK (outputsCoveredBy ({ 5000, 5000, 100, 100 }, outputs).empty ());
}
