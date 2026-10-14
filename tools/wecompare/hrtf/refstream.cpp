// refstream <file.wav> <gain> <updates> <out.f32> [stop update] [update frames] [hrtf]: streams a 16 bit WAV at
// its own rate through OpenAL Soft 1.21.1 the way WE's mediaextensions64.dll does (sf::Music queues one second
// buffers, sub_18000D530), 48 kHz float stereo loopback, mixed in updates of the given size (960 = WE's 20 ms on
// WASAPI). The source plays once; with a stop update it gets alSourceStop before that update is rendered. Compare
// with the engine's SDL_AUDIODRIVER=disk output of a "single" sound object from its first non-zero frame
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main (int argc, char** argv) {
    if (argc < 5) {
	fprintf (stderr, "usage: %s file.wav gain updates out.f32 [stop update] [update frames] [hrtf]\n", argv[0]);
	return 1;
    }

    FILE* in = fopen (argv[1], "rb");
    std::vector<unsigned char> wav (1 << 26);
    wav.resize (fread (wav.data (), 1, wav.size (), in));
    fclose (in);

    uint16_t channels;
    uint32_t rate;
    memcpy (&channels, &wav[22], 2);
    memcpy (&rate, &wav[24], 4);

    size_t offset = 12, dataOffset = 0, dataSize = 0;

    while (offset + 8 <= wav.size ()) {
	uint32_t size;
	memcpy (&size, &wav[offset + 4], 4);

	if (memcmp (&wav[offset], "data", 4) == 0) {
	    dataOffset = offset + 8;
	    dataSize = size;
	    break;
	}

	offset += 8 + size;
    }

    const int stopAt = argc > 5 ? atoi (argv[5]) : -1;
    const int updateFrames = argc > 6 ? atoi (argv[6]) : 960;
    const bool hrtf = argc > 7 && atoi (argv[7]) != 0;

    auto openLoopback
	= reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT> (alcGetProcAddress (nullptr, "alcLoopbackOpenDeviceSOFT"));
    auto render = reinterpret_cast<LPALCRENDERSAMPLESSOFT> (alcGetProcAddress (nullptr, "alcRenderSamplesSOFT"));
    ALCdevice* device = openLoopback (nullptr);
    const ALCint attrs[] = { ALC_FREQUENCY,
			     48000,
			     ALC_FORMAT_CHANNELS_SOFT,
			     ALC_STEREO_SOFT,
			     ALC_FORMAT_TYPE_SOFT,
			     ALC_FLOAT_SOFT,
			     ALC_HRTF_SOFT,
			     hrtf ? ALC_TRUE : ALC_FALSE,
			     0 };

    alcMakeContextCurrent (alcCreateContext (device, attrs));

    ALuint source;
    alGenSources (1, &source);

    // sf::Music: buffers of rate * channels samples, one second
    const size_t chunk = static_cast<size_t> (rate) * channels * 2;

    for (size_t done = 0; done < dataSize; done += chunk) {
	ALuint buffer;
	alGenBuffers (1, &buffer);
	alBufferData (
	    buffer, channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, &wav[dataOffset + done],
	    static_cast<ALsizei> (std::min (chunk, dataSize - done)), static_cast<ALsizei> (rate)
	);
	alSourceQueueBuffers (source, 1, &buffer);
    }

    alSourcef (source, AL_GAIN, atof (argv[2]));
    alSourcePlay (source);

    FILE* out = fopen (argv[4], "wb");
    std::vector<float> frames (static_cast<size_t> (updateFrames) * 2);

    for (int k = 0; k < atoi (argv[3]); k++) {
	if (k == stopAt) {
	    alSourceStop (source);
	}

	render (device, frames.data (), updateFrames);
	fwrite (frames.data (), 4, frames.size (), out);
    }

    fclose (out);
}
