// Test scenarios shared by ref.cpp (OpenAL Soft 1.21.1) and ours.cpp (Audio/Hrtf): a mono or stereo signal and,
// per 1024 sample update, where the source is and its AL_GAIN
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

struct Block {
    float x, y, z, gain;
};

constexpr int UpdateSize = 1024;
constexpr int Blocks = 60;

/** a 440 Hz tone with LCG noise on top, the same bits on both sides */
inline std::vector<float> signal (int frames, uint32_t seed) {
    std::vector<float> samples (frames);

    for (int i = 0; i < frames; i++) {
	seed = seed * 1664525u + 1013904223u;
	const float noise = static_cast<float> (seed >> 8) / 16777216.0f * 2.0f - 1.0f;

	samples[i] = 0.3f * std::sin (2.0f * 3.14159265f * 440.0f * i / 48000.0f) + 0.2f * noise;
    }

    return samples;
}

inline std::vector<float> impulse (int frames) {
    std::vector<float> samples (frames, 0.0f);

    samples[0] = 1.0f;
    return samples;
}

/** moves around the listener: every azimuth, elevations up and down, 0.3 to 4.3 units away, now and then on it */
inline std::vector<Block> movingPath () {
    std::vector<Block> path;

    for (int k = 0; k < Blocks; k++) {
	const float az = k * 0.37f, el = std::sin (k * 0.21f) * 1.2f;
	const float d = k % 17 == 5 ? 0.0f : 0.3f + 4.0f * (0.5f + 0.5f * std::sin (k * 0.13f));

	path.push_back (
	    { d * std::cos (el) * std::sin (az), d * std::sin (el), -d * std::cos (el) * std::cos (az),
	      0.4f + 0.5f * (k % 3) / 2.0f }
	);
    }

    return path;
}

struct Scenario {
    bool stereo;
    std::vector<std::vector<float>> channels;
    std::vector<Block> path;
};

/** moving | center | impulse | stereo */
inline Scenario scenario (const std::string& name) {
    Scenario s { name == "stereo", {}, {} };

    if (s.stereo) {
	auto right = signal (Blocks * UpdateSize, 11);

	for (auto& v : right) {
	    v *= 0.5f;
	}

	s.channels = { signal (Blocks * UpdateSize, 3), right };

	for (int k = 0; k < Blocks; k++) {
	    s.path.push_back ({ 0, 0, 0, k < 30 ? 0.5f : 0.8f });
	}

	return s;
    }

    s.channels = { name == "impulse" ? impulse (Blocks * UpdateSize + 48000) : signal (Blocks * UpdateSize + 48000, 7) };
    s.path = name == "moving" ? movingPath () : std::vector<Block> (Blocks, Block { 0, 0, 0, 0.9f });
    return s;
}
