// ref <scenario> <out.f32>: renders a scenario through OpenAL Soft 1.21.1 (the version WE's mediaextensions64.dll
// links) on a 48 kHz float stereo loopback device with HRTF on, one alcRenderSamplesSOFT per update
#include "scenarios.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <algorithm>
#include <cstdio>

int main (int argc, char** argv) {
    if (argc < 3) {
	fprintf (stderr, "usage: %s moving|center|impulse|stereo out.f32\n", argv[0]);
	return 1;
    }

    const Scenario s = scenario (argv[1]);
    auto openLoopback = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT> (alcGetProcAddress (nullptr, "alcLoopbackOpenDeviceSOFT"));
    auto render = reinterpret_cast<LPALCRENDERSAMPLESSOFT> (alcGetProcAddress (nullptr, "alcRenderSamplesSOFT"));
    ALCdevice* device = openLoopback (nullptr);
    const ALCint attrs[] = { ALC_FREQUENCY,		48000,	ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
			     ALC_FORMAT_TYPE_SOFT,	ALC_FLOAT_SOFT, ALC_HRTF_SOFT,	  ALC_TRUE,
			     0 };
    ALCcontext* context = alcCreateContext (device, attrs);

    alcMakeContextCurrent (context);

    ALCint status = 0;
    alcGetIntegerv (device, ALC_HRTF_STATUS_SOFT, 1, &status);
    fprintf (stderr, "HRTF %s: %s\n", status ? "on" : "off", status ? alcGetString (device, ALC_HRTF_SPECIFIER_SOFT) : "-");

    ALuint buffer, source;
    alGenBuffers (1, &buffer);
    alGenSources (1, &source);

    if (s.stereo) {
	std::vector<float> interleaved;

	for (size_t i = 0; i < s.channels[0].size (); i++) {
	    interleaved.push_back (s.channels[0][i]);
	    interleaved.push_back (s.channels[1][i]);
	}

	alBufferData (buffer, AL_FORMAT_STEREO_FLOAT32, interleaved.data (), interleaved.size () * 4, 48000);
    } else {
	alBufferData (buffer, AL_FORMAT_MONO_FLOAT32, s.channels[0].data (), s.channels[0].size () * 4, 48000);
    }

    alSourcei (source, AL_BUFFER, buffer);

    FILE* out = fopen (argv[2], "wb");
    std::vector<float> frames (UpdateSize * 2);

    for (int k = 0; k < Blocks + 4; k++) {
	const Block& b = s.path[std::min (k, Blocks - 1)];

	alSource3f (source, AL_POSITION, b.x, b.y, b.z);
	alSourcef (source, AL_GAIN, b.gain);

	if (k == 0) {
	    alSourcePlay (source);
	}

	render (device, frames.data (), UpdateSize);
	fwrite (frames.data (), 4, frames.size (), out);
    }

    fclose (out);
}
