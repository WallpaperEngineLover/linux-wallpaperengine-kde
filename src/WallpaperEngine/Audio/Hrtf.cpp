#include "Hrtf.h"

#include "WallpaperEngine/Audio/DefaultHrtf.generated.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

using namespace WallpaperEngine::Audio::Hrtf;

namespace {
constexpr float Pi = 3.14159265358979323846f;
constexpr float Tau = 6.28318530717958647692f;
constexpr unsigned MinIrLength = 8;
constexpr unsigned MaxHrirDelay = HistoryLength - 1;
constexpr unsigned HrirDelayFracOne = 4;
/** first value of the pass-through coefficients, for omnidirectional sounds */
constexpr float PassthruCoeff = 0.707106781187f;

unsigned float2uint (float value) { return static_cast<unsigned> (static_cast<int> (value)); }

/** polyphase_resampler.cpp: Kaiser windowed sinc, only used to bring the HRIRs to the device rate */
class PolyphaseResampler {
public:
    PolyphaseResampler (unsigned srcRate, unsigned dstRate) {
	const unsigned gcd = std::gcd (srcRate, dstRate);
	this->m_p = dstRate / gcd;
	this->m_q = srcRate / gcd;

	const double cutoff = 0.475 / std::max (this->m_p, this->m_q);
	const double width = 0.05 / std::max (this->m_p, this->m_q);
	// -180 dB stop band, the left offset rounded up so the transition doesn't widen
	const unsigned l = (kaiserOrder (180.0, width) + 1) / 2;
	const double beta = kaiserBeta (180.0);

	this->m_m = l * 2 + 1;
	this->m_l = l;
	this->m_filter.resize (this->m_m);

	for (unsigned i = 0; i < this->m_m; i++) {
	    this->m_filter[i] = sincFilter (l, beta, this->m_p, cutoff, i);
	}
    }

    void process (unsigned inN, const double* in, unsigned outN, double* out) const {
	const unsigned p = this->m_p, q = this->m_q, m = this->m_m, l = this->m_l;
	const double* f = this->m_filter.data ();

	for (unsigned i = 0; i < outN; i++) {
	    // input starts at l to drop the filter delay
	    size_t jf = (l + q * i) % p;
	    size_t js = (l + q * i) / p;
	    double r = 0.0;

	    if (jf < m) {
		size_t length = (m - jf + p - 1) / p;

		if (js + 1 > inN) {
		    const size_t skip = std::min<size_t> (js + 1 - inN, length);

		    jf += p * skip;
		    js -= skip;
		    length -= skip;
		}

		for (size_t todo = std::min<size_t> (js + 1, length); todo > 0; todo--) {
		    r += f[jf] * in[js];
		    jf += p;
		    --js;
		}
	    }

	    out[i] = r;
	}
    }

private:
    static double sinc (double x) {
	if (std::abs (x) < 1e-9) {
	    return 1.0;
	}

	return std::sin (M_PI * x) / (M_PI * x);
    }

    static double besselI0 (double x) {
	const double x2 = x / 2.0;
	double term = 1.0;
	double sum = 1.0;
	double last;
	int k = 1;

	do {
	    const double y = x2 / k;

	    ++k;
	    last = sum;
	    term *= y * y;
	    sum += term;
	} while (sum != last);

	return sum;
    }

    static double kaiser (double b, double k) {
	if (!(k >= -1.0 && k <= 1.0)) {
	    return 0.0;
	}

	return besselI0 (b * std::sqrt (1.0 - k * k)) / besselI0 (b);
    }

    static unsigned kaiserOrder (double rejection, double transition) {
	const double wt = 2.0 * M_PI * transition;

	if (rejection > 21.0) {
	    return static_cast<unsigned> (std::ceil ((rejection - 7.95) / (2.285 * wt)));
	}

	return static_cast<unsigned> (std::ceil (5.79 / wt));
    }

    static double kaiserBeta (double rejection) {
	if (rejection > 50.0) {
	    return 0.1102 * (rejection - 8.7);
	}

	if (rejection >= 21.0) {
	    return 0.5842 * std::pow (rejection - 21.0, 0.4) + 0.07886 * (rejection - 21.0);
	}

	return 0.0;
    }

    static double sincFilter (unsigned l, double b, double gain, double cutoff, unsigned i) {
	const double x = static_cast<double> (i) - l;

	return kaiser (b, x / l) * 2.0 * gain * cutoff * sinc (2.0 * cutoff * x);
    }

    unsigned m_p, m_q, m_m, m_l;
    std::vector<double> m_filter;
};

class Reader {
public:
    Reader (const unsigned char* data, size_t size) : m_data (data), m_size (size) { }

    [[nodiscard]] bool good () const { return this->m_good; }

    unsigned read (unsigned bytes) {
	if (this->m_offset + bytes > this->m_size) {
	    this->m_good = false;
	    return 0;
	}

	unsigned value = 0;

	for (unsigned i = 0; i < bytes; i++) {
	    value |= static_cast<unsigned> (this->m_data[this->m_offset++]) << (i * 8);
	}

	return value;
    }

    /** readle<int,24> */
    int read24 () {
	constexpr int signbit = 1 << 23;

	return (static_cast<int> (this->read (3)) ^ signbit) - signbit;
    }

private:
    const unsigned char* m_data;
    size_t m_size;
    size_t m_offset = 0;
    bool m_good = true;
};

/** LoadHrtf03, the format of the built-in set */
std::unique_ptr<Store> loadHrtf03 (Reader& data) {
    auto store = std::make_unique<Store> ();

    store->sampleRate = data.read (4);
    const unsigned channelType = data.read (1);
    store->irSize = data.read (1);
    const unsigned fdCount = data.read (1);

    if (!data.good () || channelType > 1 || store->irSize < MinIrLength || store->irSize > HrirLength || fdCount < 1
	|| fdCount > 16) {
	return nullptr;
    }

    for (unsigned f = 0; f < fdCount; f++) {
	const unsigned distance = data.read (2);
	const unsigned evCount = data.read (1);

	if (!data.good () || distance < 50 || distance > 2500 || evCount < 5 || evCount > 181) {
	    return nullptr;
	}

	store->fields.push_back ({ static_cast<float> (distance) / 1000.0f, static_cast<uint8_t> (evCount) });

	if (f > 0 && store->fields[f].distance > store->fields[f - 1].distance) {
	    return nullptr;
	}

	for (unsigned e = 0; e < evCount; e++) {
	    const auto azCount = static_cast<uint16_t> (data.read (1));

	    if (azCount < 1) {
		return nullptr;
	    }

	    store->elevations.push_back ({ azCount, 0 });
	}
    }

    if (!data.good ()) {
	return nullptr;
    }

    for (size_t e = 1; e < store->elevations.size (); e++) {
	const auto& last = store->elevations[e - 1];

	store->elevations[e].irOffset = static_cast<uint16_t> (last.azCount + last.irOffset);
    }

    const auto irTotal = static_cast<size_t> (store->elevations.back ().azCount + store->elevations.back ().irOffset);

    store->coeffs.assign (irTotal, HrirArray {});
    store->delays.assign (irTotal, {});

    for (auto& hrir : store->coeffs) {
	for (unsigned k = 0; k < store->irSize; k++) {
	    hrir[k][0] = static_cast<float> (data.read24 ()) / 8388608.0f;

	    if (channelType == 1) {
		hrir[k][1] = static_cast<float> (data.read24 ()) / 8388608.0f;
	    }
	}
    }

    for (auto& delay : store->delays) {
	delay[0] = static_cast<uint8_t> (data.read (1));

	if (channelType == 1) {
	    delay[1] = static_cast<uint8_t> (data.read (1));
	}
    }

    if (!data.good ()) {
	return nullptr;
    }

    for (const auto& delay : store->delays) {
	if (delay[0] > MaxHrirDelay * HrirDelayFracOne || delay[1] > MaxHrirDelay * HrirDelayFracOne) {
	    return nullptr;
	}
    }

    // left ear only: the right ear's response is the left one mirrored around the median plane
    if (channelType == 0) {
	for (const auto& elevation : store->elevations) {
	    for (size_t j = 0; j < elevation.azCount; j++) {
		const size_t left = elevation.irOffset + j;
		const size_t right = elevation.irOffset + (elevation.azCount - j) % elevation.azCount;

		for (size_t k = 0; k < HrirLength; k++) {
		    store->coeffs[right][k][1] = store->coeffs[left][k][0];
		}

		store->delays[right][1] = store->delays[left][0];
	    }
	}
    }

    return store;
}

/** GetLoadedHrtf's conversion to the device rate */
void resample (Store& store, unsigned deviceRate) {
    std::array<std::array<double, HrirLength>, 2> inout {};
    const PolyphaseResampler resampler (store.sampleRate, deviceRate);

    for (auto& coeffs : store.coeffs) {
	for (size_t j = 0; j < 2; j++) {
	    for (size_t k = 0; k < HrirLength; k++) {
		inout[0][k] = coeffs[k][j];
	    }

	    resampler.process (HrirLength, inout[0].data (), HrirLength, inout[1].data ());

	    for (size_t k = 0; k < HrirLength; k++) {
		coeffs[k][j] = static_cast<float> (inout[1][k]);
	    }
	}
    }

    // delays scaled for the new rate, shrunk together (a smaller head) if one passes the history length
    const float rateScale = static_cast<float> (deviceRate) / static_cast<float> (store.sampleRate);
    std::vector<std::array<float, 2>> delays (store.delays.size ());
    float maxDelay = 0.0f;

    for (size_t i = 0; i < store.delays.size (); i++) {
	for (size_t j = 0; j < 2; j++) {
	    const float delay = std::round (static_cast<float> (store.delays[i][j]) * rateScale)
		/ static_cast<float> (HrirDelayFracOne);

	    maxDelay = std::max (maxDelay, delay);
	    delays[i][j] = delay;
	}
    }

    float delayScale = HrirDelayFracOne;

    if (maxDelay > MaxHrirDelay) {
	sLog.debug ("HRTF: resampled delay exceeds the maximum (", maxDelay, " > ", MaxHrirDelay, ")");
	delayScale *= static_cast<float> (MaxHrirDelay) / maxDelay;
    }

    for (size_t i = 0; i < store.delays.size (); i++) {
	for (size_t j = 0; j < 2; j++) {
	    store.delays[i][j] = static_cast<uint8_t> (static_cast<int> (delays[i][j] * delayScale + 0.5f));
	}
    }

    const float irSize = std::round (static_cast<float> (store.irSize) * rateScale);

    store.irSize = static_cast<unsigned> (std::min (static_cast<float> (HrirLength), irSize));
    store.sampleRate = deviceRate;
}

struct IdxBlend {
    unsigned idx;
    float blend;
};

IdxBlend evIndex (unsigned evCount, float elevation) {
    elevation = (Pi * 0.5f + elevation) * static_cast<float> (evCount - 1) / Pi;
    const unsigned idx = float2uint (elevation);

    return { std::min (idx, evCount - 1), elevation - static_cast<float> (idx) };
}

IdxBlend azIndex (unsigned azCount, float azimuth) {
    azimuth = (Tau + azimuth) * static_cast<float> (azCount) / Tau;
    const unsigned idx = float2uint (azimuth);

    return { idx % azCount, azimuth - static_cast<float> (idx) };
}

/**
 * ApplyCoeffs. The DLL mixes with the SSE variant (sub_1801A5EA0/sub_1801A6010), which goes through the taps in
 * pairs: an odd IR size also gets the next tap of the (resampled, so not zero) response
 */
void applyCoeffs (std::array<float, 2>* values, unsigned irSize, const HrirArray& coeffs, float left, float right) {
    const unsigned taps = (irSize + 1) & ~1u;

    for (unsigned c = 0; c < taps; c++) {
	values[c][0] += coeffs[c][0] * left;
	values[c][1] += coeffs[c][1] * right;
    }
}
} // namespace

std::unique_ptr<Store> WallpaperEngine::Audio::Hrtf::loadDefault (unsigned deviceRate) {
    if (sizeof (DEFAULT_HRTF) < 8 || std::memcmp (DEFAULT_HRTF, "MinPHR03", 8) != 0) {
	sLog.error ("HRTF: the built-in data set has an unknown header");
	return nullptr;
    }

    Reader reader (DEFAULT_HRTF + 8, sizeof (DEFAULT_HRTF) - 8);
    auto store = loadHrtf03 (reader);

    if (store == nullptr) {
	sLog.error ("HRTF: failed to read the built-in data set");
	return nullptr;
    }

    if (store->sampleRate != deviceRate) {
	resample (*store, deviceRate);
    }

    sLog.debug ("HRTF: loaded the built-in data set for ", store->sampleRate, "hz, ", store->irSize, "-sample filter");

    return store;
}

void WallpaperEngine::Audio::Hrtf::getCoeffs (
    const Store& store, float elevation, float azimuth, float distance, float spread, Filter& out
) {
    const float dirfact = 1.0f - spread / Tau;

    // the first field nearer than the distance, the nearest one at most
    size_t field = 0;
    size_t ebase = 0;

    while (distance < store.fields[field].distance && field != store.fields.size () - 1) {
	ebase += store.fields[field].evCount;
	++field;
    }

    const unsigned evCount = store.fields[field].evCount;
    const auto elev0 = evIndex (evCount, elevation);
    const size_t elev1 = std::min (elev0.idx + 1, evCount - 1);
    const auto& e0 = store.elevations[ebase + elev0.idx];
    const auto& e1 = store.elevations[ebase + elev1];
    const auto az0 = azIndex (e0.azCount, azimuth);
    const auto az1 = azIndex (e1.azCount, azimuth);

    const size_t idx[4] = { e0.irOffset + az0.idx, e0.irOffset + (az0.idx + 1) % e0.azCount, e1.irOffset + az1.idx,
			    e1.irOffset + (az1.idx + 1) % e1.azCount };
    const float blend[4]
	= { (1.0f - elev0.blend) * (1.0f - az0.blend) * dirfact, (1.0f - elev0.blend) * az0.blend * dirfact,
	    elev0.blend * (1.0f - az1.blend) * dirfact, elev0.blend * az1.blend * dirfact };

    for (size_t ear = 0; ear < 2; ear++) {
	float d = static_cast<float> (store.delays[idx[0]][ear]) * blend[0];

	d += static_cast<float> (store.delays[idx[1]][ear]) * blend[1];
	d += static_cast<float> (store.delays[idx[2]][ear]) * blend[2];
	d += static_cast<float> (store.delays[idx[3]][ear]) * blend[3];
	// fastf2u: cvtss2si, rounded to nearest (0x18018AE8D), not truncated like the indices
	out.delay[ear] = static_cast<unsigned> (std::lrint (d * (1.0f / HrirDelayFracOne)));
    }

    out.coeffs = {};
    out.coeffs[0][0] = PassthruCoeff * (1.0f - dirfact);
    out.coeffs[0][1] = PassthruCoeff * (1.0f - dirfact);

    for (size_t c = 0; c < 4; c++) {
	const HrirArray& source = store.coeffs[idx[c]];

	for (size_t k = 0; k < HrirLength; k++) {
	    out.coeffs[k][0] = source[k][0] * blend[c] + out.coeffs[k][0];
	    out.coeffs[k][1] = source[k][1] * blend[c] + out.coeffs[k][1];
	}
    }
}

Mixer::Mixer (unsigned irSize) :
    m_irSize (irSize), m_accum (BufferLineSize + HrirLength + DirectDelay), m_samples (HistoryLength + BufferLineSize) {
}

void Mixer::mix (
    Channel& channel, const Filter& target, const float* samples, unsigned count, unsigned counter, unsigned outPos
) {
    // voice mixing lands after the direct delay, aligned with the device's own HRTF filtering of the dry buffer
    auto* accum = this->m_accum.data () + DirectDelay + outPos;
    float* input = this->m_samples.data ();

    std::copy (channel.history.begin (), channel.history.end (), input);
    std::copy_n (samples, count, input + HistoryLength);
    std::copy_n (input + count, HistoryLength, channel.history.begin ());

    if (counter == 0) {
	channel.old = target;
    }

    unsigned pos = 0;
    unsigned fademix = 0;

    // fading: the new response fades in while the old one fades out, in the update's first pass only
    if (counter > 0 && outPos == 0) {
	fademix = std::min (count, counter);

	float gain = target.gain;

	if (counter > fademix) {
	    const float a = static_cast<float> (fademix) / static_cast<float> (counter);

	    gain = channel.old.gain + (target.gain - channel.old.gain) * a;
	}

	const float newGainStep = gain / static_cast<float> (fademix);

	if (channel.old.gain > 0.00001f) {
	    const float oldGainStep = channel.old.gain / static_cast<float> (fademix);
	    size_t left = HistoryLength - channel.old.delay[0];
	    size_t right = HistoryLength - channel.old.delay[1];
	    auto stepcount = static_cast<float> (fademix);

	    for (unsigned i = 0; i < fademix; i++) {
		const float g = oldGainStep * stepcount;

		applyCoeffs (accum + i, this->m_irSize, channel.old.coeffs, input[left++] * g, input[right++] * g);
		stepcount -= 1.0f;
	    }
	}

	if (newGainStep * static_cast<float> (fademix) > 0.00001f) {
	    size_t left = HistoryLength + 1 - target.delay[0];
	    size_t right = HistoryLength + 1 - target.delay[1];
	    float stepcount = 1.0f;

	    for (unsigned i = 1; i < fademix; i++) {
		const float g = newGainStep * stepcount;

		applyCoeffs (accum + i, this->m_irSize, target.coeffs, input[left++] * g, input[right++] * g);
		stepcount += 1.0f;
	    }
	}

	channel.old = target;
	channel.old.gain = gain;
	pos = fademix;
    }

    if (fademix < count) {
	const unsigned todo = count - fademix;
	float gain = target.gain;

	if (counter > count) {
	    const float a = static_cast<float> (todo) / static_cast<float> (counter - fademix);

	    gain = channel.old.gain + (target.gain - channel.old.gain) * a;
	}

	const float gainStep = (gain - channel.old.gain) / static_cast<float> (todo);
	const float* source = input + fademix;
	size_t left = HistoryLength - target.delay[0];
	size_t right = HistoryLength - target.delay[1];
	float stepcount = 0.0f;

	for (unsigned i = 0; i < todo; i++) {
	    const float g = channel.old.gain + gainStep * stepcount;

	    applyCoeffs (accum + pos + i, this->m_irSize, target.coeffs, source[left++] * g, source[right++] * g);
	    stepcount += 1.0f;
	}

	channel.old.gain = gain;
    }
}

void Mixer::finish (float* left, float* right, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
	left[i] = this->m_accum[i][0];
	right[i] = this->m_accum[i][1];
    }

    const auto end = std::copy_n (this->m_accum.begin () + count, HrirLength + DirectDelay, this->m_accum.begin ());

    std::fill_n (end, count, std::array<float, 2> {});
}

void Mixer::clear () { std::ranges::fill (this->m_accum, std::array<float, 2> {}); }
