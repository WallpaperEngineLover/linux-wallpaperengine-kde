// refwav <file.wav> <gain> <x> <y> <z> <updates> <out.f32> [reference distance] [rolloff]: plays a 16 bit WAV
// looping through OpenAL Soft 1.21.1 with HRTF on (48 kHz float stereo loopback), what the engine should produce
// for a sound object with that AL_GAIN and position (compare with SDL_AUDIODRIVER=disk output of the engine)
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main (int argc, char** argv) {
    if (argc < 8) {
	fprintf (stderr, "usage: %s file.wav gain x y z updates out.f32 [reference] [rolloff]\n", argv[0]);
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

    auto openLoopback = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT> (alcGetProcAddress (nullptr, "alcLoopbackOpenDeviceSOFT"));
    auto render = reinterpret_cast<LPALCRENDERSAMPLESSOFT> (alcGetProcAddress (nullptr, "alcRenderSamplesSOFT"));
    ALCdevice* device = openLoopback (nullptr);
    const ALCint attrs[] = { ALC_FREQUENCY,		48000,	ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
			     ALC_FORMAT_TYPE_SOFT,	ALC_FLOAT_SOFT, ALC_HRTF_SOFT,	  ALC_TRUE,
			     0 };

    alcMakeContextCurrent (alcCreateContext (device, attrs));

    ALuint buffer, source;
    alGenBuffers (1, &buffer);
    alGenSources (1, &source);
    alBufferData (buffer, channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, &wav[dataOffset], dataSize, rate);
    alSourcei (source, AL_BUFFER, buffer);
    alSourcei (source, AL_LOOPING, AL_TRUE);
    alSourcef (source, AL_GAIN, atof (argv[2]));
    alSource3f (source, AL_POSITION, atof (argv[3]), atof (argv[4]), atof (argv[5]));
    alSourcef (source, AL_REFERENCE_DISTANCE, argc > 8 ? atof (argv[8]) : 1.0f);
    alSourcef (source, AL_ROLLOFF_FACTOR, argc > 9 ? atof (argv[9]) : 1.0f);
    alSourcePlay (source);

    FILE* out = fopen (argv[7], "wb");
    std::vector<float> frames (2048);

    for (int k = 0; k < atoi (argv[6]); k++) {
	render (device, frames.data (), 1024);
	fwrite (frames.data (), 4, frames.size (), out);
    }

    fclose (out);
}
