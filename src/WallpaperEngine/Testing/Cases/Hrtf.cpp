#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <vector>

#include "WallpaperEngine/Audio/Hrtf.h"

using namespace WallpaperEngine::Audio::Hrtf;

namespace {
constexpr float HalfPi = std::numbers::pi_v<float> / 2.0f;
constexpr float Tau = std::numbers::pi_v<float> * 2.0f;

const Store& store48k () {
    static const auto store = loadDefault (48000);
    return *store;
}

Filter filterFor (float azimuth, float spread = 0.0f) {
    Filter filter;

    getCoeffs (store48k (), 0.0f, azimuth, 1.0f, spread, filter);
    filter.gain = 1.0f;

    return filter;
}

struct Energy {
    double left = 0.0;
    double right = 0.0;
};

Energy energyOf (const Filter& filter) {
    Energy energy;

    for (const auto& tap : filter.coeffs) {
	energy.left += static_cast<double> (tap[0]) * tap[0];
	energy.right += static_cast<double> (tap[1]) * tap[1];
    }

    return energy;
}

std::vector<std::array<float, 2>> impulseResponse (const Filter& filter, float height, unsigned frames = 1024) {
    Mixer mixer (store48k ().irSize);
    Channel channel;
    std::vector<float> samples (512, 0.0f);
    std::vector<float> left (frames), right (frames);

    samples[0] = height;
    mixer.mix (channel, filter, samples.data (), samples.size (), 0);
    mixer.finish (left.data (), right.data (), frames);

    std::vector<std::array<float, 2>> out (frames);

    for (unsigned i = 0; i < frames; i++) {
	out[i] = { left[i], right[i] };
    }

    return out;
}
}

TEST_CASE ("The built-in HRTF data set is resampled to the device rate") {
    for (const unsigned rate : { 44100u, 48000u, 96000u }) {
	const auto store = loadDefault (rate);

	REQUIRE (store);
	CHECK (store->sampleRate == rate);
	CHECK (store->irSize > 0);
	CHECK (store->irSize <= HrirLength);
	CHECK_FALSE (store->fields.empty ());
	CHECK_FALSE (store->elevations.empty ());
	CHECK (store->coeffs.size () == store->delays.size ());
	CHECK_FALSE (store->coeffs.empty ());

	for (const auto& elevation : store->elevations) {
	    CHECK (elevation.azCount > 0);
	    CHECK (static_cast<size_t> (elevation.irOffset) + elevation.azCount <= store->coeffs.size ());
	}
    }
}

TEST_CASE ("The field layout adds up to the stored elevations") {
    const auto& store = store48k ();
    size_t elevations = 0;

    for (const auto& field : store.fields) {
	CHECK (field.evCount > 0);
	elevations += field.evCount;
    }

    CHECK (elevations == store.elevations.size ());
}

TEST_CASE ("A source on one side is louder in the ear on that side, mirrored on the other") {
    const auto side = energyOf (filterFor (HalfPi));
    const auto otherSide = energyOf (filterFor (-HalfPi));
    const auto ratio = std::max (side.left, side.right) / std::min (side.left, side.right);

    CHECK (ratio > 1.5);
    CHECK ((side.left > side.right) != (otherSide.left > otherSide.right));
    CHECK (side.left == Catch::Approx (otherSide.right).epsilon (0.3));
    CHECK (side.right == Catch::Approx (otherSide.left).epsilon (0.3));
}

TEST_CASE ("A source in front reaches both ears about equally and with about the same delay") {
    const auto front = filterFor (0.0f);
    const auto energy = energyOf (front);

    CHECK (energy.left == Catch::Approx (energy.right).epsilon (0.15));
    CHECK (std::abs (static_cast<int> (front.delay[0]) - static_cast<int> (front.delay[1])) <= 2);
}

TEST_CASE ("The far ear hears a source on the side later than the near one") {
    const auto side = filterFor (HalfPi);

    CHECK (side.delay[0] != side.delay[1]);
    CHECK (std::max (side.delay[0], side.delay[1]) - std::min (side.delay[0], side.delay[1]) > 5);
}

TEST_CASE ("A full spread leaves only the pass-through tap") {
    const auto spread = filterFor (HalfPi, Tau);

    CHECK (spread.coeffs[0][0] == Catch::Approx (0.707106781187f));
    CHECK (spread.coeffs[0][1] == Catch::Approx (0.707106781187f));

    for (size_t k = 1; k < HrirLength; k++) {
	CHECK (spread.coeffs[k][0] == 0.0f);
	CHECK (spread.coeffs[k][1] == 0.0f);
    }
}

TEST_CASE ("The azimuth wraps around a full turn") {
    const auto a = filterFor (0.5f);
    const auto b = filterFor (0.5f + Tau);
    const auto c = filterFor (0.5f - Tau);

    CHECK (a.delay == b.delay);
    CHECK (a.delay == c.delay);
    CHECK (energyOf (a).left == Catch::Approx (energyOf (b).left).epsilon (1e-3));
    CHECK (energyOf (a).right == Catch::Approx (energyOf (c).right).epsilon (1e-3));
}

TEST_CASE ("Nothing comes out before the direct delay") {
    const auto out = impulseResponse (filterFor (0.0f), 1.0f);

    for (unsigned i = 0; i < DirectDelay; i++) {
	REQUIRE (out[i][0] == 0.0f);
	REQUIRE (out[i][1] == 0.0f);
    }

    const double energy = std::accumulate (out.begin (), out.end (), 0.0, [] (double sum, const auto& frame) {
	return sum + static_cast<double> (frame[0]) * frame[0] + static_cast<double> (frame[1]) * frame[1];
    });

    CHECK (energy > 0.0);
}

TEST_CASE ("The mix is linear in the input and silent for silence") {
    const auto filter = filterFor (0.7f);
    const auto one = impulseResponse (filter, 1.0f);
    const auto two = impulseResponse (filter, 2.0f);
    const auto none = impulseResponse (filter, 0.0f);

    bool anyNonZero = false;

    for (size_t i = 0; i < one.size (); i++) {
	CHECK (two[i][0] == Catch::Approx (one[i][0] * 2.0f).margin (1e-6));
	CHECK (two[i][1] == Catch::Approx (one[i][1] * 2.0f).margin (1e-6));
	CHECK (none[i][0] == 0.0f);
	CHECK (none[i][1] == 0.0f);
	anyNonZero = anyNonZero || one[i][0] != 0.0f || one[i][1] != 0.0f;
    }

    CHECK (anyNonZero);
}

TEST_CASE ("A filter with no gain is silent") {
    auto filter = filterFor (0.7f);
    filter.gain = 0.0f;

    for (const auto& frame : impulseResponse (filter, 1.0f)) {
	REQUIRE (frame[0] == 0.0f);
	REQUIRE (frame[1] == 0.0f);
    }
}

TEST_CASE ("Finishing in pieces gives the same output as finishing at once") {
    const auto filter = filterFor (-0.4f);
    const auto whole = impulseResponse (filter, 1.0f, 1024);

    Mixer mixer (store48k ().irSize);
    Channel channel;
    std::vector<float> samples (512, 0.0f);
    samples[0] = 1.0f;
    mixer.mix (channel, filter, samples.data (), samples.size (), 0);

    std::vector<std::array<float, 2>> pieces;

    for (const unsigned part : { 300u, 300u, 424u }) {
	std::vector<float> left (part), right (part);

	mixer.finish (left.data (), right.data (), part);

	for (unsigned i = 0; i < part; i++) {
	    pieces.push_back ({ left[i], right[i] });
	}
    }

    REQUIRE (pieces.size () == whole.size ());

    for (size_t i = 0; i < whole.size (); i++) {
	CHECK (pieces[i][0] == Catch::Approx (whole[i][0]).margin (1e-7));
	CHECK (pieces[i][1] == Catch::Approx (whole[i][1]).margin (1e-7));
    }
}

TEST_CASE ("Clearing the mixer drops what has not been taken out yet") {
    Mixer mixer (store48k ().irSize);
    Channel channel;
    std::vector<float> samples (256, 1.0f);
    const auto filter = filterFor (0.0f);

    mixer.mix (channel, filter, samples.data (), samples.size (), 0);
    mixer.clear ();

    std::vector<float> left (1024), right (1024);

    mixer.finish (left.data (), right.data (), 1024);

    CHECK (std::ranges::all_of (left, [] (float v) { return v == 0.0f; }));
    CHECK (std::ranges::all_of (right, [] (float v) { return v == 0.0f; }));
}

TEST_CASE ("A channel remembers its last samples for the next pass") {
    Mixer mixer (store48k ().irSize);
    Channel channel;
    std::vector<float> samples (HistoryLength, 0.25f);
    const auto filter = filterFor (0.0f);

    mixer.mix (channel, filter, samples.data (), samples.size (), 0);

    CHECK (std::ranges::all_of (channel.history, [] (float v) { return v == 0.25f; }));
    CHECK (mixer.irSize () == store48k ().irSize);
}

// the values below come from this implementation, which tools/wecompare/hrtf checks bit for bit against OpenAL Soft
// 1.21.1; they pin the resampling, the interpolation and the mixing against changes
TEST_CASE ("The resampled data sets keep their shape and content") {
    struct Expected {
	unsigned rate;
	unsigned irSize;
	double sum;
	double weighted;
	unsigned long delays;
    };

    for (const auto& expected : { Expected { 44100, 32, 2103.35775, -27168.3775, 224180 },
				  Expected { 48000, 35, 2281.47328, -32490.4872, 243968 },
				  Expected { 96000, 70, 4372.12802, -136309.286, 313968 } }) {
	const auto store = loadDefault (expected.rate);

	REQUIRE (store);
	CHECK (store->irSize == expected.irSize);
	CHECK (store->coeffs.size () == 828);
	CHECK (store->fields.size () == 1);
	CHECK (store->elevations.size () == 19);

	double sum = 0.0;
	double weighted = 0.0;
	unsigned long delays = 0;

	for (size_t i = 0; i < store->coeffs.size (); i++) {
	    for (size_t k = 0; k < HrirLength; k++) {
		for (size_t ear = 0; ear < 2; ear++) {
		    sum += std::abs (store->coeffs[i][k][ear]);
		    weighted += store->coeffs[i][k][ear] * static_cast<double> (k + 1) * static_cast<double> (ear + 1)
			* static_cast<double> ((i % 7) + 1);
		}
	    }

	    delays += store->delays[i][0] * 3u + store->delays[i][1];
	}

	CHECK (sum == Catch::Approx (expected.sum).epsilon (1e-6));
	CHECK (weighted == Catch::Approx (expected.weighted).epsilon (1e-6));
	CHECK (delays == expected.delays);
    }
}

TEST_CASE ("Interpolated filters keep the delays and energies of the data set") {
    struct Expected {
	float elevation;
	float azimuth;
	unsigned delayLeft;
	unsigned delayRight;
	double left;
	double right;
	double weighted;
    };

    for (const auto& e : {
	     Expected { 0.0f, 0.0f, 22, 22, 0.293480277, 0.293480277, 0.0 },
	     Expected { 0.0f, 0.3f, 25, 18, 0.118863497, 0.543009726, 4.4756684 },
	     Expected { 0.0f, 1.0f, 29, 7, 0.0284384749, 0.505535634, 7.69075073 },
	     Expected { 0.0f, 1.5707964f, 37, 0, 0.0307828009, 0.754527524, 6.71730357 },
	     Expected { 0.0f, 3.0f, 18, 15, 0.0933420212, 0.104774858, 1.94758336 },
	     Expected { 0.0f, -0.8f, 8, 30, 0.454046479, 0.0365498303, -8.45949059 },
	     Expected { 0.0f, -2.0f, 1, 30, 0.586779965, 0.0198425651, -8.31883515 },
	     Expected { 0.4f, 0.3f, 25, 16, 0.0836256734, 0.279478338, 3.23966884 },
	     Expected { 0.4f, 1.5707964f, 40, 2, 0.0350624931, 0.414952858, 3.34995542 },
	     Expected { -0.5f, 0.3f, 26, 20, 0.0980439068, 0.393178997, 2.76170064 },
	     Expected { -0.5f, -2.0f, 4, 30, 0.618948035, 0.0196677005, -8.92334892 },
	     Expected { 1.0f, 1.0f, 14, 1, 0.0689194798, 0.456843147, 4.16397346 },
	     Expected { 1.0f, -0.8f, 2, 13, 0.447100397, 0.0791804655, -3.78917729 },
	 }) {
	Filter filter;

	getCoeffs (store48k (), e.elevation, e.azimuth, 1.0f, 0.5f, filter);

	double left = 0.0;
	double right = 0.0;
	double weighted = 0.0;

	for (size_t k = 0; k < HrirLength; k++) {
	    left += static_cast<double> (filter.coeffs[k][0]) * filter.coeffs[k][0];
	    right += static_cast<double> (filter.coeffs[k][1]) * filter.coeffs[k][1];
	    weighted += (static_cast<double> (filter.coeffs[k][0]) - filter.coeffs[k][1]) * static_cast<double> (k + 1);
	}

	INFO ("elevation " << e.elevation << " azimuth " << e.azimuth);
	CHECK (filter.delay[0] == e.delayLeft);
	CHECK (filter.delay[1] == e.delayRight);
	CHECK (left == Catch::Approx (e.left).epsilon (1e-5));
	CHECK (right == Catch::Approx (e.right).epsilon (1e-5));
	CHECK (weighted == Catch::Approx (e.weighted).epsilon (1e-5).margin (1e-6));
    }
}

TEST_CASE ("A single field answers the same at every distance") {
    for (const float distance : { 0.0f, 0.5f, 2.0f }) {
	Filter filter;

	getCoeffs (store48k (), 0.1f, 0.9f, distance, 0.0f, filter);

	double sum = 0.0;

	for (size_t k = 0; k < HrirLength; k++) {
	    sum += filter.coeffs[k][0] * static_cast<double> (k + 1) + filter.coeffs[k][1];
	}

	CHECK (filter.delay[0] == 33);
	CHECK (filter.delay[1] == 8);
	CHECK (sum == Catch::Approx (1.52886636).epsilon (1e-5));
    }
}

TEST_CASE ("An impulse through a side filter lands where the data set says") {
    const auto out = impulseResponse (filterFor (0.7f), 1.0f, 1024);
    double left = 0.0;
    double right = 0.0;
    double weightedLeft = 0.0;
    double weightedRight = 0.0;

    for (size_t i = 0; i < out.size (); i++) {
	left += out[i][0];
	right += out[i][1];
	weightedLeft += out[i][0] * static_cast<double> (i);
	weightedRight += out[i][1] * static_cast<double> (i);
    }

    CHECK (left == Catch::Approx (0.369838507).epsilon (1e-5));
    CHECK (right == Catch::Approx (0.280054989).epsilon (1e-5));
    CHECK (weightedLeft == Catch::Approx (107.674025).epsilon (1e-5));
    CHECK (weightedRight == Catch::Approx (65.7119441).epsilon (1e-5));
}

namespace {
struct FadeSummary {
    double left = 0.0;
    double right = 0.0;
    double weighted = 0.0;
    float at300 = 0.0f;
};

FadeSummary
fadeRun (float firstGain, float secondGain, unsigned count, unsigned counter, unsigned outPos, unsigned secondCount) {
    Mixer mixer (store48k ().irSize);
    Channel channel;
    auto first = filterFor (0.4f);
    auto second = filterFor (-1.1f);

    first.gain = firstGain;
    second.gain = secondGain;

    std::vector<float> samples (1024);

    for (size_t i = 0; i < samples.size (); i++) {
	samples[i] = std::sin (static_cast<float> (i) * 0.37f) + 0.3f * std::cos (static_cast<float> (i) * 1.9f);
    }

    mixer.mix (channel, first, samples.data (), count, 0);
    mixer.mix (channel, second, samples.data () + count, secondCount, counter, outPos);

    std::vector<float> left (1024), right (1024);

    mixer.finish (left.data (), right.data (), 1024);

    FadeSummary summary;

    for (unsigned i = 0; i < 1024; i++) {
	summary.left += static_cast<double> (left[i]) * left[i];
	summary.right += static_cast<double> (right[i]) * right[i];
	summary.weighted += static_cast<double> (i) * (left[i] - 0.5 * right[i]);
    }

    summary.at300 = left[300] + 2.0f * right[300];

    return summary;
}
}

TEST_CASE ("A second pass fades from the old filter to the new one the way OpenAL does") {
    struct Run {
	float firstGain, secondGain;
	unsigned count, counter, outPos, secondCount;
	double left, right, weighted;
	float at300;
    };
    const Run runs[] = { { 1.0f, 1.0f, 200, 960, 0, 200, 75.7280258, 77.2989837, -479.939469, 0.38133502f },
			 { 1.0f, 0.5f, 200, 960, 0, 200, 67.0452049, 75.7281808, -433.786364, 0.387648225f },
			 { 0.8f, 1.0f, 200, 200, 0, 200, 63.4574193, 51.962977, -472.56556, 0.292945683f },
			 { 1.0f, 1.0f, 200, 100, 0, 200, 120.902169, 78.3437258, -480.206354, -0.112697899f },
			 { 1.0f, 1.0f, 200, 150, 0, 150, 60.0724617, 71.7505667, -293.860625, 0.216657579f },
			 { 0.5f, 1.0f, 200, 960, 100, 200, 33.9333177, 18.0148771, -593.854336, -0.479922056f },
			 { 0.5f, 1.0f, 200, 500, 0, 120, 16.5388948, 17.7255974, 195.080974, 0.0138674676f },
			 { 1.0f, 0.0f, 200, 960, 0, 300, 54.0404322, 78.0715383, -510.610804, 0.558640599f },
			 { 0.0f, 1.0f, 200, 960, 0, 300, 4.44146906, 0.208997971, -215.755777, -0.012627339f },
			 { 0.0001f, 1.0f, 200, 300, 0, 300, 45.4822652, 2.14117157, -690.421758, -0.0403488427f },
			 { 0.7f, 0.2f, 200, 301, 0, 300, 14.6149649, 35.8270988, -164.394141, 0.402345568f } };

    for (const auto& run : runs) {
	const auto got = fadeRun (run.firstGain, run.secondGain, run.count, run.counter, run.outPos, run.secondCount);

	CAPTURE (run.firstGain, run.secondGain, run.count, run.counter, run.outPos, run.secondCount);
	CHECK (got.left == Catch::Approx (run.left).epsilon (1e-4));
	CHECK (got.right == Catch::Approx (run.right).epsilon (1e-4));
	CHECK (got.weighted == Catch::Approx (run.weighted).epsilon (1e-4));
	CHECK (got.at300 == Catch::Approx (run.at300).margin (1e-4));
    }
}

namespace {
Store taggedStore () {
    Store store;

    store.irSize = 32;
    store.fields = { { 1.5f, 3 }, { 0.5f, 3 } };

    for (unsigned e = 0; e < 6; e++) {
	store.elevations.push_back ({ 4, static_cast<uint16_t> (e * 4) });
    }

    store.coeffs.assign (24, HrirArray {});
    store.delays.assign (24, {});

    for (size_t i = 0; i < 24; i++) {
	store.coeffs[i][0] = { static_cast<float> (i + 1), static_cast<float> (i + 1) };
    }

    return store;
}

float tagAt (float elevation, float distance) {
    static const auto store = taggedStore ();
    Filter filter;

    getCoeffs (store, elevation, Tau / 8.0f, distance, 0.0f, filter);

    return filter.coeffs[0][0];
}
}

TEST_CASE ("The field is picked by distance, a source exactly on a field's distance uses that field") {
    CHECK (tagAt (-HalfPi / 2.0f, 2.0f) == Catch::Approx (3.5f));
    CHECK (tagAt (-HalfPi / 2.0f, 1.5f) == Catch::Approx (3.5f));
    CHECK (tagAt (-HalfPi / 2.0f, 1.0f) == Catch::Approx (15.5f));
    CHECK (tagAt (-HalfPi / 2.0f, 0.5f) == Catch::Approx (15.5f));
    CHECK (tagAt (-HalfPi / 2.0f, 0.1f) == Catch::Approx (15.5f));
}

TEST_CASE ("An elevation past the top of the field stays inside it") {
    CHECK (tagAt (3.2f, 2.0f) == Catch::Approx (9.5f));
    CHECK (tagAt (3.2f, 0.5f) == Catch::Approx (21.5f));
}

TEST_CASE ("Finishing writes exactly the frames asked for") {
    Mixer mixer (store48k ().irSize);
    Channel channel;
    std::vector<float> samples (256, 1.0f);

    mixer.mix (channel, filterFor (0.0f), samples.data (), samples.size (), 0);

    std::vector<float> left (DirectDelay + 65, -7.0f), right (DirectDelay + 65, -7.0f);

    mixer.finish (left.data (), right.data (), DirectDelay + 64);

    CHECK (left[DirectDelay + 64] == -7.0f);
    CHECK (right[DirectDelay + 64] == -7.0f);
}
