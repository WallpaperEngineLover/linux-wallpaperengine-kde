#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "WallpaperEngine/Application/ApplicationContext.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using WallpaperEngine::Application::ApplicationContext;
using Scaling = WallpaperEngine::Render::WallpaperState::TextureUVsScaling;

namespace {
// the context keeps pointers into argv, so the strings live as long as it does
struct Parsed {
    explicit Parsed (const std::vector<std::string>& arguments) : storage { "linux-wallpaperengine" } {
	storage.insert (storage.end (), arguments.begin (), arguments.end ());

	for (auto& argument : storage) {
	    argv.push_back (argument.data ());
	}

	context = std::make_unique<ApplicationContext> (static_cast<int> (argv.size ()), argv.data ());
	context->loadSettingsFromArgv ();
    }

    const auto& general () const { return context->settings.general; }
    const auto& render () const { return context->settings.render; }
    const auto& audio () const { return context->settings.audio; }

    std::vector<std::string> storage;
    std::vector<char*> argv;
    std::unique_ptr<ApplicationContext> context;
};

Parsed parse (std::vector<std::string> arguments) {
    arguments.insert (arguments.begin (), { "--assets-dir", "/lwe/assets" });

    return Parsed (arguments);
}

void refuses (const std::vector<std::string>& arguments, const std::string& message) {
    CAPTURE (arguments);
    CHECK_THROWS_WITH (parse (arguments), ContainsSubstring (message));
}
}

TEST_CASE ("A default background is required") { refuses ({}, "At least one background ID must be specified"); }

TEST_CASE ("A background path is taken as it is and the defaults stay") {
    const auto parsed = parse ({ "/lwe/wallpaper" });

    CHECK (parsed.general ().defaultBackground == "/lwe/wallpaper");
    CHECK (parsed.general ().assets == "/lwe/assets");
    CHECK (parsed.render ().mode == ApplicationContext::NORMAL_WINDOW);
    CHECK (parsed.render ().maximumFPS == 60);
    CHECK (parsed.render ().playbackSpeed == Approx (1.0f));
    CHECK (parsed.render ().hwdec == "auto");
    CHECK (parsed.render ().webFrames == "auto");
    CHECK (parsed.render ().frameLimiter == "budget");
    CHECK (parsed.render ().transition == "none");
    CHECK (parsed.render ().pauseOnFullscreen);
    CHECK (parsed.render ().pauseOnLock);
    CHECK_FALSE (parsed.render ().hdr);
    CHECK (parsed.render ().wayland.layerAuto);
    CHECK (parsed.render ().wayland.layer == ApplicationContext::WAYLAND_LAYER_BOTTOM);
    CHECK (parsed.audio ().volume == 15);
}

TEST_CASE ("An explicit window takes its geometry and the scaling options for itself") {
    const auto parsed = parse (
	{ "--window", "10x20x300x400", "--scaling", "fill", "--clamp", "repeat", "--zoom", "2", "--offset", "-1,2.5",
	  "/lwe/wallpaper" }
    );
    const auto& window = parsed.render ().window;

    CHECK (parsed.render ().mode == ApplicationContext::EXPLICIT_WINDOW);
    CHECK (window.geometry == glm::ivec4 (10, 20, 300, 400));
    CHECK (window.scalingMode == Scaling::ZoomFillUVs);
    CHECK (window.clamp == WallpaperEngine::Data::Assets::TextureFlags_NoFlags);
    CHECK (window.zoom == Approx (2.0f));
    CHECK (window.offset == glm::vec2 (-1.0f, 2.5f));
}

TEST_CASE ("Window options are checked") {
    refuses ({ "--window", "", "/lwe/wallpaper" }, "Window geometry cannot be empty");
    refuses ({ "--window", "10x20", "/lwe/wallpaper" }, "XxYxWxH");
    refuses ({ "--window", "1x2x3", "/lwe/wallpaper" }, "XxYxWxH");
    refuses ({ "--window", "1x2x3x4", "--window", "1x2x3x4", "/lwe/wallpaper" }, "Only one window at a time");
    refuses ({ "--window", "1x2x3x4", "--screen-root", "DP-1", "/lwe/wallpaper" }, "both background and window mode");
    refuses ({ "--scaling", "bogus", "/lwe/wallpaper" }, "bogus");
    refuses ({ "--clamp", "bogus", "/lwe/wallpaper" }, "bogus");
    refuses ({ "--zoom", "wide", "/lwe/wallpaper" }, "Invalid zoom value: wide");
    refuses ({ "--offset", "3", "/lwe/wallpaper" }, "Offset must be in the format: X,Y");
    refuses ({ "--offset", "a,b", "/lwe/wallpaper" }, "Invalid offset value: a,b");
}

TEST_CASE ("Every --screen-root has its own background and scaling and zoom and offset") {
    const auto parsed
	= parse ({ "--screen-root", "DP-1",          "--bg",     "/lwe/a",   "--scaling",      "fit",       "--zoom",
		   "1.5",           "--screen-root", "HDMI-1",   "--bg",     "/lwe/b",         "--scaling", "stretch",
		   "--clamp",       "clamp",         "--offset", "0.5,-0.5", "--render-scale", "0.5" });
    const auto& general = parsed.general ();

    CHECK (parsed.render ().mode == ApplicationContext::DESKTOP_BACKGROUND);
    CHECK (general.screenBackgrounds.at ("DP-1") == "/lwe/a");
    CHECK (general.screenBackgrounds.at ("HDMI-1") == "/lwe/b");
    CHECK (general.screenScalings.at ("DP-1") == Scaling::ZoomFitUVs);
    CHECK (general.screenScalings.at ("HDMI-1") == Scaling::StretchUVs);
    CHECK (general.screenZooms.at ("DP-1") == Approx (1.5f));
    CHECK_FALSE (general.screenZooms.contains ("HDMI-1"));
    CHECK (general.screenClamps.at ("HDMI-1") == WallpaperEngine::Data::Assets::TextureFlags_ClampUVs);
    CHECK (general.screenOffsets.at ("HDMI-1") == glm::vec2 (0.5f, -0.5f));
    CHECK (general.screenRenderScales.at ("HDMI-1") == Approx (0.5f));
    CHECK_FALSE (general.screenRenderScales.contains ("DP-1"));
    CHECK (parsed.context->isScreenRequested ("DP-1"));
    CHECK (parsed.context->isScreenRequested ("HDMI-1"));
    CHECK_FALSE (parsed.context->isScreenRequested ("eDP-1"));
}

TEST_CASE ("A screen can only be given once and the render scale stays between 0.1 and 1") {
    refuses ({ "--screen-root", "DP-1", "--screen-root", "DP-1", "--bg", "/lwe/a" }, "same screen more than once");
    refuses ({ "--screen-root", "DP-1", "--render-scale", "2", "--bg", "/lwe/a" }, "--render-scale must be between");
    refuses ({ "--screen-root", "DP-1", "--render-scale", "0.05", "--bg", "/lwe/a" }, "--render-scale must be between");
    refuses ({ "--screen-root", "DP-1", "--render-scale", "x", "--bg", "/lwe/a" }, "Invalid render scale value: x");

    CHECK (parse ({ "--render-scale", "0.5", "/lwe/a" }).general ().screenRenderScales.empty ());
}

TEST_CASE ("A span group shares one wallpaper and its options between its screens") {
    const auto parsed = parse (
	{ "--screen-span", "DP-1,,HDMI-1", "--bg", "/lwe/a", "--scaling", "fill", "--zoom", "2", "--clamp", "repeat",
	  "--offset", "1,1", "--render-scale", "0.5" }
    );
    const auto& general = parsed.general ();

    REQUIRE (general.spanGroups.size () == 1);

    const auto& group = general.spanGroups.front ();

    CHECK (group.screens == std::vector<std::string> { "DP-1", "HDMI-1" });
    CHECK (group.background == "/lwe/a");
    CHECK (group.scaling == Scaling::ZoomFillUVs);
    CHECK (group.zoom == Approx (2.0f));
    CHECK (group.clamp == WallpaperEngine::Data::Assets::TextureFlags_NoFlags);
    CHECK (group.offset == glm::vec2 (1.0f, 1.0f));
    CHECK (general.screenBackgrounds.at ("span:DP-1,,HDMI-1") == "/lwe/a");
    CHECK (general.screenRenderScales.at ("DP-1") == Approx (0.5f));
    CHECK (general.screenRenderScales.at ("HDMI-1") == Approx (0.5f));
}

TEST_CASE ("Span groups refuse screens that are taken") {
    refuses ({ "--screen-span", "DP-1", "--bg", "/lwe/a" }, "at least two");
    refuses ({ "--screen-span", "DP-1,DP-1", "--bg", "/lwe/a" }, "duplicate screen name 'DP-1'");
    refuses ({ "--screen-root", "DP-1", "--screen-span", "DP-1,HDMI-1", "--bg", "/lwe/a" }, "configured individually");
    refuses (
	{ "--screen-span", "DP-1,HDMI-1", "--screen-span", "HDMI-1,DP-2", "--bg", "/lwe/a" },
	"belongs to another span group"
    );
    refuses ({ "--screen-span", "DP-1,HDMI-1", "--screen-root", "DP-1", "--bg", "/lwe/a" }, "belongs to a span group");
    refuses ({ "--screen-slot", "DP-1", "--screen-span", "DP-1,HDMI-1", "--bg", "/lwe/a" }, "split with --screen-slot");
}

TEST_CASE ("Slots split a screen between several wallpapers") {
    const auto parsed = parse (
	{ "--screen-slot", "DP-1", "--bg", "/lwe/a", "--screen-slot", "DP-1", "--bg", "/lwe/b", "--scaling", "fit",
	  "--split-layout", "equal", "--screen-slot", "HDMI-1", "--bg", "/lwe/c", "--render-scale", "0.5" }
    );
    const auto& general = parsed.general ();

    REQUIRE (general.splitScreens.size () == 2);
    CHECK (general.splitScreens[0].screen == "DP-1");
    CHECK (general.splitScreens[0].slots == std::vector<std::string> { "DP-1#1", "DP-1#2" });
    CHECK (general.splitScreens[0].layout == ApplicationContext::SplitScreen::LAYOUT_EQUAL);
    CHECK (general.splitScreens[1].layout == ApplicationContext::SplitScreen::LAYOUT_ASPECT);
    CHECK (general.screenBackgrounds.at ("DP-1#2") == "/lwe/b");
    CHECK (general.screenScalings.at ("DP-1#2") == Scaling::ZoomFitUVs);
    CHECK (general.screenRenderScales.at ("HDMI-1") == Approx (0.5f));
    CHECK (parsed.context->findSplitScreen ("DP-1#2") == &general.splitScreens[0]);
    CHECK (parsed.context->findSplitScreen ("DP-1") == &general.splitScreens[0]);
    CHECK (parsed.context->findSplitScreen ("eDP-1") == nullptr);
    CHECK (parsed.context->isScreenRequested ("DP-1"));

    const auto center = parse ({ "--screen-slot", "DP-1", "--bg", "/lwe/a", "--split-layout", "center" });

    CHECK (center.general ().splitScreens[0].layout == ApplicationContext::SplitScreen::LAYOUT_CENTER);
}

TEST_CASE ("Slots refuse names and layouts that make no sense") {
    refuses ({ "--screen-slot", "", "--bg", "/lwe/a" }, "invalid screen name");
    refuses ({ "--screen-slot", "DP#1", "--bg", "/lwe/a" }, "invalid screen name");
    refuses ({ "--screen-slot", "span:a", "--bg", "/lwe/a" }, "invalid screen name");
    refuses ({ "--screen-root", "DP-1", "--screen-slot", "DP-1", "--bg", "/lwe/a" }, "configured with --screen-root");
    refuses ({ "--screen-span", "DP-1,HDMI-1", "--screen-slot", "DP-1", "--bg", "/lwe/a" }, "belongs to a span group");
    refuses ({ "--screen-root", "DP-1", "--bg", "/lwe/a", "--split-layout", "equal" }, "has to follow a --screen-slot");
    refuses ({ "--screen-slot", "DP-1", "--bg", "/lwe/a", "--split-layout", "bogus" }, "bogus");
    refuses ({ "--screen-slot", "DP-1", "--bg", "/lwe/a", "--screen-root", "DP-1" }, "split with --screen-slot");
}

TEST_CASE ("Alignment and corner color and color options go to the screen they follow") {
    const auto parsed = parse (
	{ "--screen-root",
	  "DP-1",
	  "--bg",
	  "/lwe/a",
	  "--alignment-x",
	  "25",
	  "--alignment-zoom",
	  "150",
	  "--corner-color",
	  "#ff0000",
	  "--brightness",
	  "60",
	  "--hue",
	  "10",
	  "--flip",
	  "on",
	  "--image-filter",
	  "sepia",
	  "--image-filter-strength",
	  "40",
	  "--screen-root",
	  "DP-2",
	  "--bg",
	  "/lwe/b",
	  "--image-filter",
	  "none",
	  "--color-options",
	  "off" }
    );
    const auto& general = parsed.general ();

    CHECK (general.screenAlignments.at ("DP-1").x == Approx (0.25f));
    CHECK (general.screenAlignments.at ("DP-1").zoom == Approx (1.5f));
    CHECK_FALSE (general.screenAlignments.contains ("DP-2"));
    CHECK (general.screenCornerColors.at ("DP-1").r == Approx (1.0f));
    CHECK (general.screenCornerColors.at ("DP-1").g == Approx (0.0f));

    const auto& first = general.screenImageAdjustments.at ("DP-1");

    CHECK (first.brightness == 60.0f);
    CHECK (first.hue == 10.0f);
    CHECK (first.colorEnabled == true);
    CHECK (first.flipHorizontal == true);
    CHECK (first.filter == "sepia");
    CHECK (first.filterStrength == 40.0f);

    const auto& second = general.screenImageAdjustments.at ("DP-2");

    CHECK (second.filter == "");
    CHECK (second.colorEnabled == false);
    CHECK_FALSE (second.brightness.has_value ());
}

TEST_CASE ("Sliders and alignments are checked against their ranges") {
    refuses ({ "--brightness", "101", "/lwe/a" }, "brightness must be between 0 and 100");
    refuses ({ "--contrast", "-1", "/lwe/a" }, "contrast must be between 0 and 100");
    refuses ({ "--saturation", "high", "/lwe/a" }, "Invalid saturation value: high");
    refuses ({ "--image-filter-strength", "200", "/lwe/a" }, "image filter strength must be between 0 and 100");
    refuses ({ "--alignment-x", "101", "/lwe/a" }, "must be between 0 and 100");
    refuses ({ "--alignment-zoom", "201", "/lwe/a" }, "must be between 0 and 200");
    refuses ({ "--alignment-y", "up", "/lwe/a" }, "Invalid --alignment-y value: up");
    refuses ({ "--corner-color", "not-a-color", "/lwe/a" }, "Invalid corner color: not-a-color");
}

TEST_CASE ("Performance options are read and checked") {
    const auto parsed = parse (
	{ "--fps",
	  "30",
	  "--fps-battery",
	  "15",
	  "--idle-pause",
	  "5",
	  "--no-lock-pause",
	  "--speed",
	  "1.5",
	  "--hwdec",
	  "no",
	  "--web-frames",
	  "shm",
	  "--frame-limiter",
	  "fixed",
	  "--transition",
	  "fade",
	  "--transition-duration",
	  "900",
	  "--no-fullscreen-pause",
	  "--fullscreen-pause-only-active",
	  "--pause-on-focus",
	  "--pause-on-maximized",
	  "--fullscreen-pause-ignore-appid",
	  "steam",
	  "--fullscreen-pause-ignore-appid",
	  "",
	  "--fullscreen-pause-ignore-appid",
	  "obs",
	  "--hdr",
	  "--layer",
	  "overlay",
	  "--window-background",
	  "--window-normal",
	  "/lwe/a" }
    );
    const auto& render = parsed.render ();

    CHECK (render.maximumFPS == 30);
    CHECK (render.batteryFPS == 15);
    CHECK (render.idlePauseMinutes == 5);
    CHECK_FALSE (render.pauseOnLock);
    CHECK (render.playbackSpeed == Approx (1.5f));
    CHECK (render.hwdec == "no");
    CHECK (render.webFrames == "shm");
    CHECK (render.frameLimiter == "fixed");
    CHECK (render.transition == "fade");
    CHECK (render.transitionMs == 900);
    CHECK_FALSE (render.pauseOnFullscreen);
    CHECK (render.pauseOnFullscreenOnlyWhenActive);
    CHECK (render.pauseOnFocus);
    CHECK (render.pauseOnMaximized);
    CHECK (render.fullscreenPauseIgnoreAppIds == std::vector<std::string> { "steam", "obs" });
    CHECK (render.hdr);
    CHECK_FALSE (render.wayland.layerAuto);
    CHECK (render.wayland.layer == ApplicationContext::WAYLAND_LAYER_OVERLAY);
    CHECK (render.window.asBackground);
    CHECK (render.window.regular);

    CHECK (parse ({ "--pause-on-battery", "/lwe/a" }).render ().batteryFPS == 0);
    CHECK (
	parse ({ "--layer", "background", "/lwe/a" }).render ().wayland.layer
	== ApplicationContext::WAYLAND_LAYER_BACKGROUND
    );
    CHECK (parse ({ "--layer", "top", "/lwe/a" }).render ().wayland.layer == ApplicationContext::WAYLAND_LAYER_TOP);
    CHECK (
	parse ({ "--layer", "bottom", "/lwe/a" }).render ().wayland.layer == ApplicationContext::WAYLAND_LAYER_BOTTOM
    );
}

TEST_CASE ("Performance options refuse bad values") {
    refuses ({ "--fps-battery", "x", "/lwe/a" }, "Invalid --fps-battery value: x");
    refuses ({ "--fps-battery", "-1", "/lwe/a" }, "--fps-battery can't be negative");
    refuses ({ "--idle-pause", "x", "/lwe/a" }, "Invalid --idle-pause value: x");
    refuses ({ "--idle-pause", "-5", "/lwe/a" }, "--idle-pause can't be negative");
    refuses ({ "--speed", "fast", "/lwe/a" }, "Invalid speed value: fast");
    refuses ({ "--speed", "0", "/lwe/a" }, "--speed must be a positive number");
    refuses ({ "--hwdec", "bogus!", "/lwe/a" }, "Invalid --hwdec value: bogus!");
    refuses ({ "--web-frames", "gpu", "/lwe/a" }, "Invalid --web-frames value: gpu");
    refuses ({ "--frame-limiter", "vsync", "/lwe/a" }, "Invalid --frame-limiter value: vsync");
    refuses ({ "--transition-duration", "soon", "/lwe/a" }, "Invalid --transition-duration value: soon");
    refuses ({ "--layer", "middle", "/lwe/a" }, "Invalid wlr-layer-shell layer");
}

TEST_CASE ("The transition duration is held between its limits") {
    CHECK (parse ({ "--transition-duration", "1", "/lwe/a" }).render ().transitionMs == 100);
    CHECK (parse ({ "--transition-duration", "999999", "/lwe/a" }).render ().transitionMs == 5000);
}

TEST_CASE ("The volume is held between 0 and 128 and silent turns the sound off") {
    CHECK (parse ({ "--volume", "200", "/lwe/a" }).audio ().volume == 128);
    CHECK (parse ({ "--volume", "-5", "/lwe/a" }).audio ().volume == 0);
    CHECK (parse ({ "--volume", "64", "/lwe/a" }).audio ().volume == 64);
    CHECK_FALSE (parse ({ "--silent", "/lwe/a" }).audio ().enabled);
    CHECK (parse ({ "--automute", "/lwe/a" }).audio ().automute);
    CHECK_FALSE (parse ({ "--noautomute", "/lwe/a" }).audio ().automute);
}

TEST_CASE ("The web host helper arguments are checked") {
    const auto parsed = parse (
	{ "--web-host", "--web-host-shm", "lwe-web-1", "--web-host-frames", "dmabuf", "--web-host-socket", "7",
	  "--web-host-width", "1920", "--web-host-height", "1080", "/lwe/a" }
    );

    CHECK (parsed.general ().webHost);
    CHECK (parsed.general ().webHostShm == "lwe-web-1");
    CHECK (parsed.general ().webHostFrames == "dmabuf");
    CHECK (parsed.general ().webHostSocket == 7);
    CHECK (parsed.general ().webHostWidth == 1920);
    CHECK (parsed.general ().webHostHeight == 1080);

    refuses ({ "--web-host-frames", "tcp", "/lwe/a" }, "Invalid --web-host-frames value: tcp");
    refuses ({ "--web-host-socket", "x", "/lwe/a" }, "Invalid --web-host-socket value: x");
    refuses ({ "--web-host-width", "x", "/lwe/a" }, "Invalid --web-host-width value: x");
    refuses ({ "--web-host-height", "x", "/lwe/a" }, "Invalid --web-host-height value: x");
}

TEST_CASE ("Audio options beyond the volume") {
    const auto parsed = parse (
	{ "--no-audio-processing", "--audio-screen", "DP-2", "--ambient-volume", "200", "--hrtf", "on", "/lwe/a" }
    );

    CHECK_FALSE (parsed.audio ().audioprocessing);
    CHECK (parsed.audio ().audioScreen == "DP-2");
    CHECK (parsed.audio ().ambientVolume == 128);
    CHECK (parsed.audio ().hrtf == ApplicationContext::HRTF_ON);
    CHECK (parse ({ "--hrtf", "off", "/lwe/a" }).audio ().hrtf == ApplicationContext::HRTF_OFF);
    CHECK (parse ({ "--hrtf", "auto", "/lwe/a" }).audio ().hrtf == ApplicationContext::HRTF_AUTO);
    CHECK (parse ({ "--ambient-volume", "-3", "/lwe/a" }).audio ().ambientVolume == 0);
    CHECK_FALSE (parse ({ "--audio-screen", "", "/lwe/a" }).audio ().audioScreen.has_value ());

    refuses ({ "--ambient-volume", "loud", "/lwe/a" }, "Invalid --ambient-volume value: loud");
    refuses ({ "--hrtf", "maybe", "/lwe/a" }, "Invalid --hrtf value: maybe");
    CHECK (ApplicationContext::parseHrtfMode ("on") == ApplicationContext::HRTF_ON);
    CHECK_FALSE (ApplicationContext::parseHrtfMode ("").has_value ());
}

TEST_CASE ("LED options turn the output on and read the server") {
    const auto defaults = parse ({ "/lwe/a" });

    CHECK_FALSE (defaults.context->settings.leds.enabled);
    CHECK (defaults.context->settings.leds.host == "127.0.0.1");
    CHECK (defaults.context->settings.leds.port == 6742);

    const auto flags
	= parse ({ "--leds", "--leds-no-boost", "--leds-reduce-fps", "--leds-brightness", "25", "/lwe/a" });
    const auto& leds = flags.context->settings.leds;

    CHECK (leds.enabled);
    CHECK_FALSE (leds.boost);
    CHECK (leds.reduceFps);
    CHECK (leds.brightness == Approx (0.25f));

    const auto hostOnly = parse ({ "--leds-server", "rgb.lan", "/lwe/a" });

    CHECK (hostOnly.context->settings.leds.enabled);
    CHECK (hostOnly.context->settings.leds.host == "rgb.lan");
    CHECK (hostOnly.context->settings.leds.port == 6742);

    const auto both = parse ({ "--leds-server", "192.168.1.5:7000", "/lwe/a" });

    CHECK (both.context->settings.leds.host == "192.168.1.5");
    CHECK (both.context->settings.leds.port == 7000);

    refuses ({ "--leds-server", "", "/lwe/a" }, "Invalid --leds-server value");
    refuses ({ "--leds-server", "host:abc", "/lwe/a" }, "Invalid port in --leds-server value: host:abc");
    refuses ({ "--leds-server", "host:0", "/lwe/a" }, "Invalid --leds-server value: host:0");
    refuses ({ "--leds-server", "host:65536", "/lwe/a" }, "Invalid --leds-server value: host:65536");
    refuses ({ "--leds-server", ":7000", "/lwe/a" }, "Invalid --leds-server value: :7000");
    refuses ({ "--leds-brightness", "bright", "/lwe/a" }, "Invalid --leds-brightness value: bright");
    refuses ({ "--leds-brightness", "101", "/lwe/a" }, "--leds-brightness must be between 0 and 100");
    refuses ({ "--leds-brightness", "-1", "/lwe/a" }, "--leds-brightness must be between 0 and 100");
}

TEST_CASE ("Screenshots need a known image format and a delay of at most 5000 frames") {
    const auto parsed = parse ({ "--screenshot", "/tmp/shot.png", "--screenshot-delay", "100", "/lwe/a" });

    CHECK (parsed.context->settings.screenshot.take);
    CHECK (parsed.context->settings.screenshot.path == "/tmp/shot.png");
    CHECK (parsed.context->settings.screenshot.delay == 100);
    CHECK (parse ({ "--screenshot", "/tmp/a.jpg", "/lwe/a" }).context->settings.screenshot.delay == 5);
    CHECK (
	parse ({ "--screenshot", "/tmp/a.jpeg", "--screenshot-delay", "99999", "/lwe/a" })
	    .context->settings.screenshot.delay
	== 5000
    );
    CHECK (parse ({ "--screenshot", "/tmp/a.bmp", "/lwe/a" }).context->settings.screenshot.take);
    CHECK_FALSE (parse ({ "/lwe/a" }).context->settings.screenshot.take);

    refuses ({ "--screenshot", "/tmp/shot", "/lwe/a" }, "Cannot determine screenshot format");
    refuses ({ "--screenshot", "/tmp/shot.gif", "/lwe/a" }, "unknown extension .gif");
}

TEST_CASE ("Video trimming reads times and segments") {
    const auto trimmed = parse ({ "--video-start", "1:30", "--video-end", "2:00", "/lwe/a" });

    REQUIRE (trimmed.render ().videoSegments.size () == 1);
    CHECK (trimmed.render ().videoSegments.front ().start == Approx (90.0));
    CHECK (trimmed.render ().videoSegments.front ().end == Approx (120.0));

    const auto parts = parse ({ "--video-segments", "0:10-0:20,1:00-1:30", "/lwe/a" });

    REQUIRE (parts.render ().videoSegments.size () == 2);
    CHECK (parts.render ().videoSegments[1].start == Approx (60.0));

    refuses ({ "--video-start", "soon", "/lwe/a" }, "Invalid --video-start value");
    refuses ({ "--video-end", "later", "/lwe/a" }, "Invalid --video-end value");
    refuses ({ "--video-segments", "nonsense", "/lwe/a" }, "Invalid --video-segments value");
}

TEST_CASE ("Rendering switches and quality levels") {
    const auto parsed = parse (
	{ "--control-file",
	  "/tmp/lwe-control",
	  "--disable-particles",
	  "--expand-canvas",
	  "--repair-assets",
	  "--post-processing",
	  "displayhdr",
	  "--volumetrics",
	  "ultra",
	  "--shadows",
	  "low",
	  "--msaa",
	  "x4",
	  "--ssaa",
	  "x3",
	  "--disable-animations",
	  "--disable-mouse",
	  "--input-file",
	  "/tmp/lwe-input",
	  "--disable-parallax",
	  "--allow-parallax-overflow",
	  "--no-shader-cache",
	  "/lwe/a" }
    );
    const auto& general = parsed.general ();

    CHECK (general.controlFile == "/tmp/lwe-control");
    CHECK (general.disableParticles);
    CHECK (general.expandCanvas);
    CHECK (general.repairAssets);
    CHECK_FALSE (general.postProcessingDisabled);
    CHECK (general.ultraPostProcessing);
    CHECK (general.displayHDR);
    CHECK (general.volumetricsQuality == 4);
    CHECK (general.shadowQuality == 1);
    CHECK (general.msaaSamples == 4);
    CHECK (general.supersampling == 3);
    CHECK (general.noShaderCache);
    CHECK (parsed.render ().freezeAnimations);
    CHECK_FALSE (parsed.context->settings.mouse.enabled);
    CHECK (parsed.context->settings.mouse.inputFile == "/tmp/lwe-input");
    CHECK (parsed.context->settings.mouse.disableparallax);
    CHECK_FALSE (parsed.context->settings.mouse.clampParallaxToImageSize);

    const auto disabled = parse (
	{ "--post-processing", "disabled", "--volumetrics", "disabled", "--shadows", "high", "--msaa", "x8", "--ssaa",
	  "x2", "/lwe/a" }
    );

    CHECK (disabled.general ().postProcessingDisabled);
    CHECK_FALSE (disabled.general ().ultraPostProcessing);
    CHECK (disabled.general ().volumetricsQuality == 0);
    CHECK (disabled.general ().shadowQuality == 3);
    CHECK (disabled.general ().msaaSamples == 8);
    CHECK (disabled.general ().supersampling == 2);

    const auto defaults = parse (
	{ "--post-processing", "enabled", "--volumetrics", "medium", "--msaa", "none", "--ssaa", "none", "/lwe/a" }
    );

    CHECK (defaults.general ().volumetricsQuality == 2);
    CHECK (defaults.general ().msaaSamples == 0);
    CHECK (defaults.general ().supersampling == 1);
    CHECK_FALSE (parse ({ "--post-processing", "ultra", "/lwe/a" }).general ().displayHDR);
    CHECK (parse ({ "/lwe/a" }).context->settings.mouse.enabled);

    CHECK_FALSE (parse ({ "--control-file", "", "/lwe/a" }).general ().controlFile.has_value ());
    refuses ({ "--post-processing", "max", "/lwe/a" }, "max");
    refuses ({ "--msaa", "x16", "/lwe/a" }, "x16");
}

TEST_CASE ("Listing and properties and per object switches are collected") {
    const auto parsed = parse (
	{ "--list-properties",
	  "--list-objects",
	  "--list-effects",
	  "--list-fonts",
	  "--list-audio-objects",
	  "--dump-structure",
	  "--set-property",
	  "speed=3",
	  "--property",
	  "flag",
	  "--set-property",
	  "text=a=b",
	  "--disable-object",
	  "12",
	  "--disable-object",
	  "stars",
	  "--enable-object",
	  "99",
	  "--disable-effect",
	  "blur",
	  "--enable-effect",
	  "glow",
	  "--audio-sensitivity",
	  "7=2.5",
	  "--audio-sensitivity",
	  "*=0.5",
	  "--sound-volume",
	  "rain=0.25",
	  "/lwe/a" }
    );
    const auto& general = parsed.general ();

    CHECK (general.onlyListProperties);
    CHECK (general.onlyListObjects);
    CHECK (general.onlyListEffects);
    CHECK (general.onlyListFonts);
    CHECK (general.onlyListAudioObjects);
    CHECK (general.dumpStructure);
    CHECK (general.properties.at ("speed") == "3");
    CHECK (general.properties.at ("flag") == "1");
    CHECK (general.properties.at ("text") == "a=b");
    CHECK (general.disabledObjects == std::vector<std::string> { "12", "stars" });
    CHECK (general.enabledObjects == std::vector<std::string> { "99" });
    CHECK (general.disabledEffects == std::vector<std::string> { "blur" });
    CHECK (general.enabledEffects == std::vector<std::string> { "glow" });

    const auto& context = *parsed.context;

    CHECK (context.resolveObjectVisibility (12, "anything") == false);
    CHECK (context.resolveObjectVisibility (1, "stars") == false);
    CHECK (context.resolveObjectVisibility (99, "other") == true);
    CHECK_FALSE (context.resolveObjectVisibility (5, "other").has_value ());
    CHECK (context.resolveEffectVisibility (1, "blur") == false);
    CHECK (context.resolveEffectVisibility (1, "glow") == true);
    CHECK_FALSE (context.resolveEffectVisibility (1, "other").has_value ());
    CHECK (context.resolveAudioSensitivity (7, "x") == Approx (2.5f));
    CHECK (context.resolveAudioSensitivity (8, "x") == Approx (0.5f));
    CHECK (context.resolveSoundVolume (3, "rain") == Approx (0.25f));
    CHECK_FALSE (context.resolveSoundVolume (3, "wind").has_value ());

    refuses ({ "--audio-sensitivity", "nope", "/lwe/a" }, "--audio-sensitivity expects <id-or-name>=<multiplier>");
    refuses ({ "--audio-sensitivity", "7=loud", "/lwe/a" }, "'loud' is not a valid number");
    refuses ({ "--sound-volume", "nope", "/lwe/a" }, "--sound-volume expects <id-or-name>=<volume>");
    refuses ({ "--sound-volume", "rain=quiet", "/lwe/a" }, "'quiet' is not a valid number");
}

TEST_CASE ("Render debug modes") {
    const auto parsed = parse (
	{ "--render-debug", "base-only", "--render-debug", "no-solid-final", "--render-debug", "pass-log",
	  "--render-debug", "brightness-log", "--render-debug", "no-puppet-animation", "--render-debug", "object=42",
	  "--render-debug", "skip-object=1", "--render-debug", "skip-object=2", "--render-debug", "skip-effect=9",
	  "/lwe/a" }
    );
    const auto& debug = parsed.render ().debug;

    CHECK (debug.baseOnly);
    CHECK (debug.noSolidFinal);
    CHECK (debug.passLog);
    CHECK (debug.brightnessLog);
    CHECK (debug.noPuppetAnimation);
    CHECK (debug.objectFilter == 42);
    CHECK (debug.skipObjects == std::vector<int> { 1, 2 });
    CHECK (debug.skipEffects == std::vector<int> { 9 });

    refuses ({ "--render-debug", "everything", "/lwe/a" }, "Invalid render debug mode: everything");
    refuses ({ "--render-debug", "object=x", "/lwe/a" }, "Invalid numeric value for --render-debug object=x");
    refuses ({ "--render-debug", "skip-object=99999999999", "/lwe/a" }, "Out-of-range numeric value");
}

TEST_CASE ("Turning desktop icons off is passed on through the environment") {
    unsetenv ("LWE_DESKTOP_ICONS");
    parse ({ "--no-desktop-icons", "/lwe/a" });

    const char* value = getenv ("LWE_DESKTOP_ICONS");

    CHECK (value != nullptr);
    CHECK (std::string (value != nullptr ? value : "") == "off");
    unsetenv ("LWE_DESKTOP_ICONS");
}

namespace {
struct FakeSteam {
    FakeSteam () :
	home (std::filesystem::temp_directory_path () / ("lwe-playlist-test-" + std::to_string (getpid ()))) {
	if (const char* previous = getenv ("HOME"); previous != nullptr) {
	    oldHome = previous;
	}

	std::filesystem::remove_all (home);
	std::filesystem::create_directories (steamApp ());
	std::filesystem::create_directories (home / "items/a");
	std::filesystem::create_directories (home / "items/b");
	std::ofstream (home / "items/a/project.json") << "{}";
	setenv ("HOME", home.c_str (), 1);
    }

    ~FakeSteam () {
	if (oldHome.empty ()) {
	    unsetenv ("HOME");
	} else {
	    setenv ("HOME", oldHome.c_str (), 1);
	}

	std::filesystem::remove_all (home);
    }

    [[nodiscard]] std::filesystem::path steamApp () const {
	return home / ".local/share/Steam/steamapps/common/wallpaper_engine";
    }

    void config (const std::string& content) const { std::ofstream (steamApp () / "config.json") << content; }

    [[nodiscard]] std::string item (const char* name) const { return (home / "items" / name).string (); }

    std::filesystem::path home;
    std::string oldHome;
};

std::string wholeConfig (const FakeSteam& steam) {
    return R"({"steamuser":{"general":{"playlists":[
	{"name":"mix","items":[")"
	+ steam.item ("a") + R"(/project.json",")" + steam.item ("b") + R"(","/lwe/missing",5],
	 "settings":{"delay":15,"mode":"timer","order":"random","updateonpause":true,"videosequence":true,
		     "beginfirst":true,"playintro":true}},
	{"name":"empty","items":[]},
	{"name":"nothing"}
    ]},"wallpaperconfig":{"selectedwallpapers":{
	"DP-1":{"playlist":{"items":[")"
	+ steam.item ("b") + R"("],"settings":{"mode":"ondemand","beginfirst":true}}},
	"HDMI-1":{}
    }}}})";
}
}

TEST_CASE ("Playlists are read from the config.json of the Steam install") {
    const FakeSteam steam;

    steam.config (wholeConfig (steam));

    const auto parsed = parse ({ "--playlist", "mix" });
    const auto& playlist = parsed.general ().defaultPlaylist;

    REQUIRE (playlist.has_value ());
    CHECK (playlist->name == "mix");
    CHECK (playlist->items == std::vector<std::filesystem::path> { steam.item ("a"), steam.item ("b") });
    CHECK (playlist->settings.delayMinutes == 15);
    CHECK (playlist->settings.mode == "timer");
    CHECK (playlist->settings.order == "random");
    CHECK (playlist->settings.updateOnPause);
    CHECK (playlist->settings.videoSequence);
    CHECK (playlist->settings.beginFirst);
    CHECK (playlist->settings.playIntro);
    CHECK (parsed.general ().defaultBackground == steam.item ("a"));
}

TEST_CASE ("A playlist can belong to a screen and playlists of selected wallpapers are found by their screen") {
    const FakeSteam steam;

    steam.config (wholeConfig (steam));

    const auto parsed
	= parse ({ "--screen-root", "eDP-1", "--playlist", "mix", "--screen-root", "DP-2", "--playlist", "DP-1" });
    const auto& general = parsed.general ();

    CHECK (general.screenPlaylists.at ("eDP-1").name == "mix");
    CHECK (general.screenBackgrounds.at ("eDP-1") == steam.item ("a"));
    CHECK (general.screenBackgrounds.at ("DP-2") == steam.item ("b"));
    CHECK (general.defaultBackground == steam.item ("a"));
    CHECK_FALSE (general.defaultPlaylist.has_value ());

    const auto& selected = general.screenPlaylists.at ("DP-2");

    CHECK (selected.settings.mode == "ondemand");
    CHECK (selected.settings.delayMinutes == 60);
    CHECK_FALSE (selected.settings.beginFirst);
    CHECK_FALSE (selected.settings.playIntro);
    CHECK (selected.settings.order == "sequential");
}

TEST_CASE ("Unknown and unusable playlists are refused and the others listed") {
    const FakeSteam steam;

    steam.config (wholeConfig (steam));

    refuses ({ "--playlist", "empty" }, "Playlist not found in config.json: empty. Available: DP-1, mix");
    refuses ({ "--playlist", "nothing" }, "Playlist not found in config.json: nothing");
    refuses ({ "--playlist", "HDMI-1" }, "Playlist not found in config.json: HDMI-1");
}

TEST_CASE ("A config.json that is missing or broken has no playlists") {
    {
	const FakeSteam steam;

	refuses ({ "--playlist", "mix" }, "Cannot open wallpaper engine config file");

	steam.config ("{ not json");
	refuses ({ "--playlist", "mix" }, "Playlist not found");

	steam.config (R"({"other":1})");
	refuses ({ "--playlist", "mix" }, "Cannot find steamuser section in config.json");
    }

    struct EmptyHome {
	std::filesystem::path path
	    = std::filesystem::temp_directory_path () / ("lwe-playlist-no-steam-" + std::to_string (getpid ()));
	std::optional<std::string> saved;

	EmptyHome () {
	    if (const char* previous = getenv ("HOME"); previous != nullptr) {
		saved = previous;
	    }

	    std::filesystem::create_directories (path);
	    setenv ("HOME", path.c_str (), 1);
	}

	~EmptyHome () {
	    if (saved) {
		setenv ("HOME", saved->c_str (), 1);
	    } else {
		unsetenv ("HOME");
	    }

	    std::filesystem::remove_all (path);
	}
    } emptyHome;

    refuses ({ "--playlist", "mix" }, "Cannot locate wallpaper engine installation");
}
