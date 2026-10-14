// ours <scenario> <out.f32>: the same scenario through the engine's Audio/Hrtf port, with the source parameters
// worked out like OpenAL (CSound::sourceParams / hrtfOutput)
#include "scenarios.h"

#include "WallpaperEngine/Audio/Hrtf.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <limits>

using namespace WallpaperEngine::Audio;

int main (int argc, char** argv) {
    if (argc < 3) {
	fprintf (stderr, "usage: %s moving|center|impulse|stereo out.f32\n", argv[0]);
	return 1;
    }

    const Scenario s = scenario (argv[1]);
    const auto store = Hrtf::loadDefault (48000);
    Hrtf::Mixer mixer (store->irSize);
    std::array<Hrtf::Channel, 2> channels {};
    const float inf = std::numeric_limits<float>::infinity ();
    const std::vector<float> silence (UpdateSize, 0.0f);
    std::vector<float> left (UpdateSize), right (UpdateSize), frames (UpdateSize * 2);
    bool fading = false;
    FILE* out = fopen (argv[2], "wb");

    for (int k = 0; k < Blocks + 4; k++) {
	const Block& b = s.path[std::min (k, Blocks - 1)];
	std::array<Hrtf::Filter, 2> filters {};

	if (s.stereo) {
	    const float angle = 30.0f * 3.14159265358979323846f / 180.0f;

	    Hrtf::getCoeffs (*store, 0.0f, -angle, inf, 0.0f, filters[0]);
	    Hrtf::getCoeffs (*store, 0.0f, angle, inf, 0.0f, filters[1]);
	    filters[0].gain = filters[1].gain = std::clamp (b.gain, 0.0f, 1.0f);
	} else {
	    // alu::Vector::normalize with AL_REFERENCE_DISTANCE 1, then the inverse clamped distance model
	    float x = b.x, y = b.y, z = b.z, distance = 0.0f, gain = b.gain;
	    const float limit = std::max (1.0f / 1024.0f, FLT_EPSILON);

	    if (x * x + y * y + z * z > limit * limit) {
		distance = std::sqrt (x * x + y * y + z * z);
		const float inverse = 1.0f / distance;
		x *= inverse;
		y *= inverse;
		z *= inverse;
	    }

	    gain *= 1.0f / (1.0f + (std::clamp (distance, 1.0f, FLT_MAX) - 1.0f));
	    gain = std::clamp (gain, 0.0f, 1.0f);

	    if (distance > FLT_EPSILON) {
		Hrtf::getCoeffs (*store, std::asin (std::clamp (y, -1.0f, 1.0f)), std::atan2 (x, -z), distance, 0.0f, filters[0]);
	    } else {
		Hrtf::getCoeffs (*store, 0.0f, 0.0f, inf, 0.0f, filters[0]);
	    }

	    filters[0].gain = gain;
	}

	for (size_t c = 0; c < s.channels.size (); c++) {
	    const bool more = static_cast<size_t> (k + 1) * UpdateSize <= s.channels[c].size ();
	    const float* samples = more ? s.channels[c].data () + static_cast<size_t> (k) * UpdateSize : silence.data ();

	    mixer.mix (channels[c], filters[c], samples, UpdateSize, fading ? UpdateSize : 0);
	}

	fading = true;
	mixer.finish (left.data (), right.data (), UpdateSize);

	for (int i = 0; i < UpdateSize; i++) {
	    frames[2 * i] = left[i];
	    frames[2 * i + 1] = right[i];
	}

	fwrite (frames.data (), 4, frames.size (), out);
    }

    fclose (out);
}
