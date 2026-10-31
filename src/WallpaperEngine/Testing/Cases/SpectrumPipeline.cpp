#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "WallpaperEngine/Audio/SpectrumAnalyzer.h"
#include "WallpaperEngine/Audio/SpectrumProcessor.h"

using Catch::Approx;
using WallpaperEngine::Audio::SpectrumAnalyzer;
using WallpaperEngine::Audio::SpectrumProcessor;

namespace {
std::vector<float> stereoTone (int frames, float frequency, int rate, float leftAmplitude, float rightAmplitude) {
    std::vector<float> samples (frames * 2);

    for (int i = 0; i < frames; i++) {
	const float value
	    = std::sin (2.0f * 3.1415927f * frequency * static_cast<float> (i) / static_cast<float> (rate));

	samples[i * 2] = value * leftAmplitude;
	samples[i * 2 + 1] = value * rightAmplitude;
    }

    return samples;
}
}

TEST_CASE ("Spectrum analyzer uses WE's FFT and block sizes") {
    SpectrumAnalyzer at44 (44100);
    SpectrumAnalyzer at48 (48000);

    CHECK (at44.getFFTSize () == 1920);
    CHECK (at44.getBlockSize () == 1280);
    CHECK (at48.getFFTSize () == 2089);
    CHECK (at48.getBlockSize () == 1392);
}

TEST_CASE ("A low tone lands in the band of its bin, one bin per band at the bottom") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    // bin 10 of a 1920 point FFT at 44.1kHz, bins 1-30 get a band each (bin - 1)
    const auto samples = stereoTone (1280, 10.0f * 44100.0f / 1920.0f, 44100, 0.5f, 0.0f);

    REQUIRE (analyzer.feed (samples.data (), 1280, 2, bands));

    const auto loudest = std::max_element (bands, bands + 64) - bands;

    CHECK (loudest == 9);
    CHECK (bands[loudest] > 0.0f);
    CHECK (*std::max_element (bands + 64, bands + 128) < bands[loudest] * 0.001f);
}

TEST_CASE ("The packet completing a block is not carried into the next one") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    const auto samples = stereoTone (2000, 440.0f, 44100, 0.5f, 0.5f);

    CHECK (analyzer.feed (samples.data (), 2000, 2, bands));
    CHECK_FALSE (analyzer.feed (samples.data (), 1279, 2, bands));
    CHECK (analyzer.feed (samples.data (), 1, 2, bands));
}

TEST_CASE ("Mono input is copied to the right channel") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    std::vector<float> samples (1280);

    for (int i = 0; i < 1280; i++) {
	samples[i] = 0.5f * std::sin (2.0f * 3.1415927f * 1000.0f * static_cast<float> (i) / 44100.0f);
    }

    REQUIRE (analyzer.feed (samples.data (), 1280, 1, bands));

    for (int i = 0; i < 64; i++) {
	CHECK_THAT (bands[i], Catch::Matchers::WithinULP (bands[i + 64], 0));
    }
}

TEST_CASE ("Steady input settles at the level of its group") {
    SpectrumProcessor processor;
    float raw[128];

    std::fill_n (raw, 64, 0.5f);
    std::fill_n (raw + 64, 64, 0.25f);

    for (int frame = 0; frame < 600; frame++) {
	processor.update (raw, 1.0f / 60.0f);
    }

    CHECK_THAT (processor.getGroupLevels ()[0], Catch::Matchers::WithinRel (0.5f, 1.2e-5f));
    CHECK_THAT (processor.getGroupLevels ()[8], Catch::Matchers::WithinRel (0.25f, 1.2e-5f));
    CHECK_THAT (processor.audio64[0], Catch::Matchers::WithinAbs (1.0f, 0.01));
    CHECK_THAT (processor.audio64[64], Catch::Matchers::WithinAbs (1.0f, 0.01));
    CHECK_THAT (processor.audio64[128], Catch::Matchers::WithinAbs (1.0f, 0.01));
}

TEST_CASE ("Buffers are left, right and average, lower resolutions take pairwise maxima") {
    SpectrumProcessor processor;
    float raw[128] = {};

    for (int i = 0; i < 128; i++) {
	raw[i] = 0.1f + 0.005f * static_cast<float> (i);
    }

    for (int frame = 0; frame < 120; frame++) {
	processor.update (raw, 1.0f / 60.0f);
    }

    for (int i = 0; i < 64; i++) {
	CHECK_THAT (
	    processor.audio64[128 + i],
	    Catch::Matchers::WithinRel ((processor.audio64[i] + processor.audio64[64 + i]) * 0.5f, 1.2e-5f)
	);
    }

    for (int i = 0; i < 96; i++) {
	CHECK_THAT (
	    processor.audio32[i],
	    Catch::Matchers::WithinULP (std::max (processor.audio64[i * 2], processor.audio64[i * 2 + 1]), 0)
	);
    }

    for (int i = 0; i < 48; i++) {
	CHECK_THAT (
	    processor.audio16[i],
	    Catch::Matchers::WithinULP (std::max (processor.audio32[i * 2], processor.audio32[i * 2 + 1]), 0)
	);
    }
}

TEST_CASE ("Silence drops the spectrum to zero at once") {
    SpectrumProcessor processor;
    float raw[128];
    const float silence[128] = {};

    std::fill_n (raw, 128, 0.5f);

    for (int frame = 0; frame < 120; frame++) {
	processor.update (raw, 1.0f / 60.0f);
    }

    REQUIRE (processor.audio16[0] > 0.5f);

    processor.update (silence, 1.0f / 60.0f);

    CHECK_THAT (processor.audio16[0], Catch::Matchers::WithinULP (0.0f, 0));
    CHECK_THAT (processor.audio64[127], Catch::Matchers::WithinULP (0.0f, 0));
}

TEST_CASE ("The first frame only moves part of the way") {
    SpectrumProcessor processor;
    float raw[128];

    std::fill_n (raw, 128, 0.5f);
    processor.update (raw, 1.0f / 60.0f);

    // the level starts at 1 and has moved down by dt / 2, then 20 * dt of the way to raw / level
    const float level = 1.0f - 0.5f / 60.0f;

    CHECK_THAT (processor.audio64[0], Catch::Matchers::WithinRel (0.5f / level * (20.0f / 60.0f), 1.2e-5f));
}

namespace {
struct Golden {
    int index;
    float value;
};

std::vector<float> goldenSignal (int rate, int frames) {
    std::vector<float> samples (frames * 2);

    for (int i = 0; i < frames; i++) {
	const float t = static_cast<float> (i) / static_cast<float> (rate);

	samples[i * 2]
	    = 0.5f * std::sin (2.0f * 3.1415927f * 1234.5f * t) + 0.1f * std::sin (2.0f * 3.1415927f * 9000.0f * t);
	samples[i * 2 + 1] = 0.3f * std::sin (2.0f * 3.1415927f * 7000.5f * t);
    }

    return samples;
}

void checkGolden (int rate, const std::vector<Golden>& golden) {
    SpectrumAnalyzer analyzer (rate);
    const auto samples = goldenSignal (rate, analyzer.getBlockSize ());
    float bands[128];

    REQUIRE (analyzer.feed (samples.data (), analyzer.getBlockSize (), 2, bands));

    for (const auto& [index, value] : golden) {
	CAPTURE (rate, index);
	CHECK_THAT (
	    bands[index], Catch::Matchers::WithinRel (value, 3e-3f) || Catch::Matchers::WithinAbs (value, 2e-6)
	);
    }
}
}
TEST_CASE ("Spectrum analyzer bands match the reference run at 44100 Hz") {
    checkGolden (
	44100, { { 0, 0.013816501f },     { 3, 0.0137063116f },    { 6, 0.0140445577f },    { 9, 0.0147811491f },
		 { 12, 0.0158849545f },   { 15, 0.017343767f },    { 18, 0.0191728715f },   { 21, 0.0214217063f },
		 { 24, 0.024182912f },    { 27, 0.0276142899f },   { 30, 0.0686211064f },   { 33, 0.445890009f },
		 { 36, 0.183263794f },    { 39, 0.0723311678f },   { 42, 0.0518552884f },   { 45, 0.0425799638f },
		 { 48, 0.036621388f },    { 51, 0.0337999202f },   { 54, 0.0601809397f },   { 57, 0.141988665f },
		 { 60, 0.0299188718f },   { 63, 0.0251757037f },   { 64, 0.00170539541f },  { 66, 0.000128399188f },
		 { 69, 0.000133936948f }, { 72, 0.000144685968f }, { 75, 0.000158677605f }, { 78, 0.000176924805f },
		 { 81, 0.000200083159f }, { 84, 0.000225490745f }, { 87, 0.000257470936f }, { 90, 0.000291477743f },
		 { 93, 0.00342386612f },  { 96, 0.00487080123f },  { 99, 0.00684306072f },  { 102, 0.00988882966f },
		 { 105, 0.0145412395f },  { 108, 0.0229196548f },  { 111, 0.039545469f },   { 114, 0.0974140093f },
		 { 117, 10.6767969f },    { 120, 0.0820137486f },  { 123, 0.039690394f },   { 126, 0.0247494858f },
		 { 127, 0.0221142136f } }
    );
}
TEST_CASE ("Spectrum analyzer bands match the reference run at 48000 Hz") {
    checkGolden (
	48000, { { 0, 0.01251595f },      { 3, 0.0123210996f },    { 6, 0.0125339879f },    { 9, 0.0130981179f },
		 { 12, 0.0139824916f },   { 15, 0.0151748043f },   { 18, 0.0166882686f },   { 21, 0.0185694974f },
		 { 24, 0.0209062155f },   { 27, 0.0238455329f },   { 30, 0.067027919f },    { 33, 0.446046472f },
		 { 36, 0.184503287f },    { 39, 0.0739070252f },   { 42, 0.0531304479f },   { 45, 0.0431639627f },
		 { 48, 0.0357369445f },   { 51, 0.029087035f },    { 54, 0.040414378f },    { 57, 0.169221267f },
		 { 60, 0.047421094f },    { 63, 0.036861293f },    { 64, 0.00187733117f },  { 66, 4.40481163e-05f },
		 { 69, 4.71988315e-05f }, { 72, 5.13521845e-05f }, { 75, 6.12851945e-05f }, { 78, 7.25482969e-05f },
		 { 81, 8.48746859e-05f }, { 84, 0.000101791025f }, { 87, 0.000120402961f }, { 90, 0.000141368902f },
		 { 93, 0.00371621526f },  { 96, 0.0052506309f },   { 99, 0.00732420897f },  { 102, 0.0104678897f },
		 { 105, 0.0152218221f },  { 108, 0.0236617215f },  { 111, 0.040246468f },   { 114, 0.0976708531f },
		 { 117, 10.1804771f },    { 120, 0.0795953944f },  { 123, 0.0380289219f },  { 126, 0.0235706344f },
		 { 127, 0.0206185877f } }
    );
}

TEST_CASE ("The bands don't depend on how the samples are split into packets") {
    SpectrumAnalyzer whole (44100);
    SpectrumAnalyzer split (44100);
    const auto samples = goldenSignal (44100, 1280);
    float expected[128];
    float bands[128];

    REQUIRE (whole.feed (samples.data (), 1280, 2, expected));

    REQUIRE_FALSE (split.feed (samples.data (), 500, 2, bands));
    REQUIRE_FALSE (split.feed (samples.data () + 500 * 2, 400, 2, bands));
    REQUIRE (split.feed (samples.data () + 900 * 2, 380, 2, bands));

    for (int i = 0; i < 128; i++) {
	CHECK (bands[i] == expected[i]);
    }
}

TEST_CASE ("A mono signal in a stereo analyzer's packets reads the same as a stereo one with equal channels") {
    SpectrumAnalyzer mono (44100);
    SpectrumAnalyzer stereo (44100);
    std::vector<float> one (1280);
    std::vector<float> two (2560);

    for (int i = 0; i < 1280; i++) {
	one[i] = 0.4f * std::sin (2.0f * 3.1415927f * 3000.0f * static_cast<float> (i) / 44100.0f);
	two[i * 2] = one[i];
	two[i * 2 + 1] = one[i];
    }

    float a[128];
    float b[128];

    REQUIRE (mono.feed (one.data (), 1280, 1, a));
    REQUIRE (stereo.feed (two.data (), 1280, 2, b));

    for (int i = 0; i < 128; i++) {
	CHECK (a[i] == b[i]);
    }
}

TEST_CASE ("The channels are analyzed separately and written to their own halves") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    std::vector<float> samples (2560);

    for (int i = 0; i < 1280; i++) {
	const float t = static_cast<float> (i) / 44100.0f;

	samples[i * 2] = 0.5f * std::sin (2.0f * 3.1415927f * 10.0f * 44100.0f / 1920.0f * t);
	samples[i * 2 + 1] = 0.5f * std::sin (2.0f * 3.1415927f * 20.0f * 44100.0f / 1920.0f * t);
    }

    REQUIRE (analyzer.feed (samples.data (), 1280, 2, bands));

    CHECK (std::max_element (bands, bands + 64) - bands == 9);
    CHECK (std::max_element (bands + 64, bands + 128) - (bands + 64) == 19);
}

TEST_CASE ("Analyzing writes exactly the 128 bands") {
    SpectrumAnalyzer analyzer (44100);
    float bands[130];
    const auto samples = goldenSignal (44100, 1280);

    std::fill_n (bands, 130, -1.0f);
    REQUIRE (analyzer.feed (samples.data (), 1280, 2, bands));

    CHECK (bands[128] == -1.0f);
    CHECK (bands[129] == -1.0f);
}

TEST_CASE ("A feed that doesn't finish a block leaves the bands alone, reset starts the block over") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    const auto samples = goldenSignal (44100, 1280);

    std::fill_n (bands, 128, -1.0f);
    CHECK_FALSE (analyzer.feed (samples.data (), 1279, 2, bands));
    CHECK (bands[0] == -1.0f);
    analyzer.reset ();
    CHECK_FALSE (analyzer.feed (samples.data (), 1279, 2, bands));
    CHECK (analyzer.feed (samples.data (), 1, 2, bands));
}

TEST_CASE ("A channel count outside 1 to 2 is treated as the nearest") {
    SpectrumAnalyzer low (44100);
    SpectrumAnalyzer high (44100);
    const auto stereo = goldenSignal (44100, 1280);
    std::vector<float> mono (1280);
    float a[128];
    float b[128];

    for (int i = 0; i < 1280; i++) {
	mono[i] = stereo[i * 2];
    }

    SpectrumAnalyzer reference (44100);
    float c[128];

    REQUIRE (low.feed (mono.data (), 1280, 0, a));
    REQUIRE (reference.feed (mono.data (), 1280, 1, c));
    REQUIRE (high.feed (stereo.data (), 1280, 7, b));

    for (int i = 0; i < 128; i++) {
	CHECK (a[i] == c[i]);
    }

    SpectrumAnalyzer twoChannels (44100);
    REQUIRE (twoChannels.feed (stereo.data (), 1280, 2, c));

    for (int i = 0; i < 128; i++) {
	CHECK (b[i] == c[i]);
    }
}

namespace {
void rawFrame (float* raw, int frame, float scale) {
    for (int i = 0; i < 128; i++) {
	raw[i] = scale
	    * (0.05f
	       + 0.9f * std::fabs (std::sin (static_cast<float> (i) * 0.21f + static_cast<float> (frame) * 0.3f)));
    }
}

std::array<float, 16> picked (const SpectrumProcessor& p) {
    const auto* levels = p.getGroupLevels ();

    return { levels[0],      levels[5],      levels[15],     p.audio64[0],   p.audio64[7], p.audio64[64],
	     p.audio64[100], p.audio64[127], p.audio64[128], p.audio64[150], p.audio32[0], p.audio32[20],
	     p.audio32[90],  p.audio16[0],   p.audio16[10],  p.audio16[47] };
}
}

// values from this implementation (checked against WE's pipeline when it was written): steady input, a dt past the
// clamp, a dt under it, a level walking down, silence and sound coming back
TEST_CASE ("The processor follows a changing signal through its clamps and levels") {
    struct Phase {
	const char* name;
	std::array<float, 16> expected;
    };
    const Phase phases[] = {
	{ "steady",
	  { 0.750000238f, 0.750000238f, 0.750000238f, 0.595096827f, 0.385963172f, 0.30525741f, 0.579868853f,
	    0.404507458f, 0.450177133f, 0.50401473f, 0.595096827f, 0.615397453f, 0.549066186f, 0.595096827f,
	    0.646394849f, 0.444958538f } },
	{ "loud-bigdt",
	  { 1.50000024f, 1.50000024f, 1.50000024f, 0.413788795f, 1.89501667f, 1.66033268f, 1.41104877f, 1.88196278f,
	    1.03706074f, 1.51568687f, 0.77637738f, 1.45177674f, 1.50186968f, 1.39777255f, 1.45177674f, 1.66046953f } },
	{ "quiet-tinydt",
	  { 1.49985039f, 1.49985039f, 1.49985039f, 0.411575049f, 1.88396156f, 1.65075803f, 1.40271401f, 1.87097049f,
	    1.03116655f, 1.50680232f, 0.772038996f, 1.44320142f, 1.49306536f, 1.38976657f, 1.44320142f, 1.65084791f } },
	{ "quiet-bigdt",
	  { 0.874850392f, 0.874850392f, 0.874850392f, 0.0609150641f, 0.0865313858f, 0.0368292779f, 0.108580925f,
	    0.0906266719f, 0.0488721728f, 0.096643962f, 0.0609150641f, 0.108568691f, 0.0989293754f, 0.0609150641f,
	    0.108568691f, 0.0531657934f } },
	{ "silent",
	  { 0.866517067f, 0.866517067f, 0.866517067f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	    0.0f, 0.0f } },
	{ "back",
	  { 0.890013397f, 0.899850428f, 0.899850428f, 0.235214487f, 0.695510507f, 0.429795027f, 0.707561553f,
	    0.704267263f, 0.332504749f, 0.671587825f, 0.235214487f, 0.714107633f, 0.670821548f, 0.281311423f,
	    0.714107633f, 0.510251999f } },
	{ "fast",
	  { 0.876124561f, 0.885961592f, 0.885961592f, 0.354411781f, 0.75944066f, 0.641665459f, 0.579841137f,
	    0.74818176f, 0.49803862f, 0.631993949f, 0.354411781f, 0.595075428f, 0.62907964f, 0.538555384f, 0.595075428f,
	    0.662948668f } }
    };

    SpectrumProcessor processor;
    float raw[128];
    const float silence[128] = {};
    int frame = 0;
    size_t phase = 0;

    const auto check = [&] {
	const auto got = picked (processor);

	CAPTURE (phases[phase].name);

	for (size_t i = 0; i < got.size (); i++) {
	    CAPTURE (i);
	    CHECK_THAT (
		got[i],
		Catch::Matchers::WithinRel (phases[phase].expected[i], 2e-5f)
		    || Catch::Matchers::WithinAbs (phases[phase].expected[i], 1e-7)
	    );
	}

	phase++;
    };

    for (int i = 0; i < 30; i++) {
	rawFrame (raw, frame++, 0.6f);
	processor.update (raw, 1.0f / 60.0f);
    }

    check ();

    for (int i = 0; i < 3; i++) {
	rawFrame (raw, frame++, 3.0f);
	processor.update (raw, 0.3f);
    }

    check ();

    for (int i = 0; i < 3; i++) {
	rawFrame (raw, frame++, 0.1f);
	processor.update (raw, 0.0000001f);
    }

    check ();

    for (int i = 0; i < 5; i++) {
	rawFrame (raw, frame++, 0.1f);
	processor.update (raw, 0.5f);
    }

    check ();

    processor.update (silence, 1.0f / 60.0f);
    check ();

    rawFrame (raw, frame++, 1.0f);
    processor.update (raw, 1.0f / 30.0f);
    check ();

    for (int i = 0; i < 4; i++) {
	rawFrame (raw, frame++, 0.9f);
	processor.update (raw, 1.0f / 144.0f);
    }

    check ();
}

TEST_CASE ("A signal exactly at the silence threshold counts as sound") {
    SpectrumProcessor processor;
    float raw[128];

    std::fill_n (raw, 128, 0.0001f);
    processor.update (raw, 1.0f / 60.0f);

    CHECK (processor.getGroupLevels ()[0] > 0.9f);
    CHECK (processor.audio64[0] > 0.0f);

    std::fill_n (raw, 128, 0.00009f);

    SpectrumProcessor quiet;

    quiet.update (raw, 1.0f / 60.0f);
    CHECK (quiet.getGroupLevels ()[0] < 0.0001f);
    CHECK (quiet.audio64[0] == 0.0f);
}
