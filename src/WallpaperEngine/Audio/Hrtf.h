#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * OpenAL Soft 1.21.1's HRTF renderer, the one WE's mediaextensions64.dll links statically (alc/hrtf.cpp,
 * core/mixer/hrtfbase.h, alc/voice.cpp DoHrtfMix, common/polyphase_resampler.cpp; LGPL, Chris Robinson). WE
 * creates its context without attributes, so OpenAL turns HRTF on by itself for a stereo headphones/headset endpoint
 * (aluInitRenderer sub_1801990F0, WASAPI form factor 3 or 5 in sub_180186400) and renders in its default "full" mode:
 * every voice is convolved with the built-in KEMAR data set (embedded byte for byte in the DLL), resampled to the
 * device rate.
 */
namespace WallpaperEngine::Audio::Hrtf {
constexpr unsigned HistoryLength = 64;
constexpr unsigned HrirLength = 128;
constexpr unsigned DirectDelay = 256;
/** OpenAL Soft's mixing granularity, the most samples one update mixes */
constexpr unsigned BufferLineSize = 1024;

using HrirArray = std::array<std::array<float, 2>, HrirLength>;

struct Store {
    unsigned sampleRate = 0;
    unsigned irSize = 0;

    struct Field {
	float distance;
	uint8_t evCount;
    };

    struct Elevation {
	uint16_t azCount;
	uint16_t irOffset;
    };

    /** stored backwards like OpenAL: the farthest field first */
    std::vector<Field> fields;
    std::vector<Elevation> elevations;
    std::vector<HrirArray> coeffs;
    std::vector<std::array<uint8_t, 2>> delays;
};

/** The built-in data set ("Built-In HRTF") loaded for the given output rate, nullptr if it can't be read */
std::unique_ptr<Store> loadDefault (unsigned deviceRate);

struct Filter {
    alignas (16) HrirArray coeffs {};
    std::array<unsigned, 2> delay {};
    float gain = 0.0f;
};

/** GetHrtfCoeffs: blended impulse response and delays for a direction (radians), normalized like OpenAL's */
void getCoeffs (const Store& store, float elevation, float azimuth, float distance, float spread, Filter& out);

/** Mixing state of one input channel of a voice */
struct Channel {
    std::array<float, HistoryLength> history {};
    Filter old {};
};

/** The device side: the accumulation buffer every voice convolves into, read out with OpenAL's direct delay */
class Mixer {
public:
    explicit Mixer (unsigned irSize);

    /**
     * DoHrtfMix for one channel over one mixing pass of count samples (at most BufferLineSize).
     *
     * @param counter OpenAL's fade counter: 0 on a voice's first update (no fade), the update size after that, minus
     * what earlier passes of the update mixed
     * @param outPos where the pass starts in the update, the IR crossfade only happens in the first pass
     */
    void mix (
	Channel& channel, const Filter& target, const float* samples, unsigned count, unsigned counter,
	unsigned outPos = 0
    );
    /** ProcessHrtf without ambisonic input: count samples of left/right out, then shifts the accumulator */
    void finish (float* left, float* right, unsigned count);
    void clear ();

    [[nodiscard]] unsigned irSize () const { return this->m_irSize; }

private:
    unsigned m_irSize;
    std::vector<std::array<float, 2>> m_accum;
    std::vector<float> m_samples;
};
} // namespace WallpaperEngine::Audio::Hrtf
