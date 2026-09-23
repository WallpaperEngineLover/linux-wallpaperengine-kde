#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"

using WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder;

namespace {
// what a capturing recorder does once it has a new spectrum
class PushingRecorder : public PlaybackRecorder {
public:
    void push (const float* bands) { this->notifySpectrumListeners (bands); }
};
} // namespace

TEST_CASE ("Spectrum listeners are pushed every new spectrum until removed") {
    PushingRecorder recorder;
    float bands[64] = { 0.5f };
    int calls = 0;
    float first = 0.0f;

    const int id = recorder.addSpectrumListener ([&] (const float* audio64) {
	calls++;
	first = audio64[0];
    });

    recorder.push (bands);
    bands[0] = 0.25f;
    recorder.push (bands);

    CHECK (calls == 2);
    CHECK (first == 0.25f);

    recorder.removeSpectrumListener (id);
    recorder.push (bands);

    CHECK (calls == 2);
}

TEST_CASE ("Several spectrum listeners each get the spectrum") {
    PushingRecorder recorder;
    float bands[64] = {};
    int a = 0;
    int b = 0;

    recorder.addSpectrumListener ([&] (const float*) { a++; });
    const int second = recorder.addSpectrumListener ([&] (const float*) { b++; });

    recorder.push (bands);
    recorder.removeSpectrumListener (second);
    recorder.push (bands);

    CHECK (a == 2);
    CHECK (b == 1);
}
