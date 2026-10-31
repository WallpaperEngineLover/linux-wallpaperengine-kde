#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/AudioPlayingDetector.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"
#include "WallpaperEngine/Render/Drivers/Detectors/FullScreenDetector.h"

using namespace std::chrono_literals;
using Catch::Approx;
using WallpaperEngine::Application::ApplicationContext;
using WallpaperEngine::Audio::AudioContext;
using WallpaperEngine::Audio::AudioStream;
using WallpaperEngine::Audio::Drivers::AudioDriver;
using WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector;
using WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder;
using WallpaperEngine::Data::Utils::MemoryStream;
using WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector;

namespace {
class QuietScreen final : public FullScreenDetector {
public:
    using FullScreenDetector::FullScreenDetector;

    [[nodiscard]] bool anythingFullscreen () const override { return false; }
};

class FixedDriver final : public AudioDriver {
public:
    FixedDriver (ApplicationContext& context, AudioPlayingDetector& detector, PlaybackRecorder& recorder, int rate) :
	AudioDriver (context, detector, recorder), m_rate (rate) { }

    int addStream (AudioStream*, int, float, float) override { return 0; }
    void removeStream (int) override { }
    void setStreamVolume (int, int) override { }
    void setStreamGains (int, float, float) override { }
    void setStreamPaused (int, bool) override { }

    [[nodiscard]] AVSampleFormat getFormat () const override { return AV_SAMPLE_FMT_FLT; }
    [[nodiscard]] int getSampleRate () const override { return m_rate; }
    [[nodiscard]] int getChannels () const override { return 2; }

private:
    int m_rate;
};

struct Device {
    explicit Device (int rate) :
	context (makeContext ()), screen (context), detector (context, screen),
	driver (context, detector, recorder, rate), audio (driver) {
	context.state.general.keepRunning = true;
    }

    static ApplicationContext makeContext () {
	static const char* argv[] = { "linux-wallpaperengine" };

	return ApplicationContext (1, const_cast<char**> (argv));
    }

    ApplicationContext context;
    QuietScreen screen;
    AudioPlayingDetector detector;
    PlaybackRecorder recorder;
    FixedDriver driver;
    AudioContext audio;
};

void put (std::vector<char>& out, const void* data, size_t size) {
    out.insert (out.end (), static_cast<const char*> (data), static_cast<const char*> (data) + size);
}

void putLE (std::vector<char>& out, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) {
	out.push_back (static_cast<char> ((value >> (8 * i)) & 0xff));
    }
}

template <typename Sample> std::shared_ptr<MemoryStream> wav (int rate, int channels, int frames, Sample sample) {
    std::vector<char> data;

    for (int i = 0; i < frames; i++) {
	for (int c = 0; c < channels; c++) {
	    putLE (data, static_cast<uint16_t> (static_cast<int16_t> (sample (i, c))), 2);
	}
    }

    std::vector<char> file;

    put (file, "RIFF", 4);
    putLE (file, 36 + data.size (), 4);
    put (file, "WAVEfmt ", 8);
    putLE (file, 16, 4);
    putLE (file, 1, 2);
    putLE (file, channels, 2);
    putLE (file, rate, 4);
    putLE (file, rate * channels * 2, 4);
    putLE (file, channels * 2, 2);
    putLE (file, 16, 2);
    put (file, "data", 4);
    putLE (file, data.size (), 4);
    file.insert (file.end (), data.begin (), data.end ());

    auto buffer = std::make_unique<char[]> (file.size ());

    std::memcpy (buffer.get (), file.data (), file.size ());

    return std::make_shared<MemoryStream> (std::move (buffer), file.size ());
}

constexpr float Scale = 1.0f / 32768.0f;

std::vector<float> readFrames (AudioStream& stream, int frames, int chunk = 64) {
    std::vector<float> out;
    std::vector<float> part (static_cast<size_t> (chunk) * 2);
    const auto deadline = std::chrono::steady_clock::now () + 5s;

    while (static_cast<int> (out.size ()) < frames * 2 && std::chrono::steady_clock::now () < deadline) {
	const int want = std::min (chunk, frames - static_cast<int> (out.size () / 2));
	const int got = stream.read (part.data (), want);

	if (got < 0) {
	    break;
	}

	if (got == 0) {
	    std::this_thread::sleep_for (1ms);
	    continue;
	}

	out.insert (out.end (), part.begin (), part.begin () + got * 2);
    }

    return out;
}
}

TEST_CASE ("A file at the device rate is played sample for sample") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 2, 1000, [] (int i, int c) { return c == 0 ? i * 20 : -i * 10; }));

    CHECK (stream.getSourceChannels () == 2);
    CHECK (stream.getDuration () == Approx (1000.0 / 48000.0).margin (1e-3));

    const auto out = readFrames (stream, 900);

    REQUIRE (out.size () == 1800);

    for (int i = 0; i < 900; i++) {
	CAPTURE (i);
	CHECK (out[i * 2] == Approx (static_cast<float> (i * 20) * Scale).margin (1e-6));
	CHECK (out[i * 2 + 1] == Approx (static_cast<float> (-i * 10) * Scale).margin (1e-6));
    }
}

TEST_CASE ("A mono file is sent to both channels unchanged") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 500, [] (int i, int) { return i * 30 - 4000; }));

    CHECK (stream.getSourceChannels () == 1);

    const auto out = readFrames (stream, 400);

    REQUIRE (out.size () == 800);

    for (int i = 0; i < 400; i++) {
	CAPTURE (i);
	CHECK (out[i * 2] == Approx (static_cast<float> (i * 30 - 4000) * Scale).margin (1e-6));
	CHECK (out[i * 2 + 1] == out[i * 2]);
    }
}

TEST_CASE ("A file at half the device rate is interpolated linearly") {
    Device device (48000);
    AudioStream stream (device.audio, wav (24000, 1, 600, [] (int i, int) { return i * 40; }));

    const auto out = readFrames (stream, 800);

    REQUIRE (out.size () == 1600);

    for (int i = 0; i < 400; i++) {
	CAPTURE (i);
	CHECK (out[i * 4] == Approx (static_cast<float> (i * 40) * Scale).margin (1e-6));
	CHECK (out[i * 4 + 2] == Approx (static_cast<float> (i * 40 + 20) * Scale).margin (1e-5));
    }
}

TEST_CASE ("A file at twice the device rate skips every other frame") {
    Device device (24000);
    AudioStream stream (device.audio, wav (48000, 1, 1200, [] (int i, int) { return i * 10; }));

    const auto out = readFrames (stream, 500);

    REQUIRE (out.size () == 1000);

    for (int i = 0; i < 500; i++) {
	CAPTURE (i);
	CHECK (out[i * 2] == Approx (static_cast<float> (i * 20) * Scale).margin (1e-6));
    }
}

TEST_CASE ("A file that runs out holds its last frame to the end of the update and then ends the voice") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 100, [] (int i, int) { return 1000 + i; }));
    std::vector<float> out (512 * 2);
    int got = 0;

    for (int attempt = 0; attempt < 5000 && got == 0; attempt++) {
	got = stream.read (out.data (), 256);

	if (got == 0) {
	    std::this_thread::sleep_for (1ms);
	}
    }

    REQUIRE (got == 256);
    CHECK (out[99 * 2] == Approx (1099.0f * Scale).margin (1e-6));
    CHECK (out[255 * 2] == Approx (1099.0f * Scale).margin (1e-6));
    CHECK (stream.ended ());
    CHECK (stream.read (out.data (), 256) < 0);
}

TEST_CASE ("A repeating file loops without a gap") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 100, [] (int i, int) { return 2000 + i * 7; }), true);

    CHECK (stream.isRepeat ());

    const auto out = readFrames (stream, 450);

    REQUIRE (out.size () == 900);

    for (int i = 0; i < 450; i++) {
	CAPTURE (i);
	CHECK (out[i * 2] == Approx (static_cast<float> (2000 + (i % 100) * 7) * Scale).margin (1e-6));
    }

    CHECK_FALSE (stream.ended ());
}

TEST_CASE ("The mixing passes of an update follow the source step") {
    Device same (48000);
    AudioStream plain (same.audio, wav (48000, 1, 100, [] (int i, int) { return i; }));

    CHECK (plain.passes (960) == std::vector<int> { 960 });
    CHECK (plain.passes (2500) == std::vector<int> { 2500 });

    Device slow (48000);
    AudioStream fast (slow.audio, wav (192000, 1, 100, [] (int i, int) { return i; }));
    const auto parts = fast.passes (960);
    int total = 0;

    for (const int part : parts) {
	total += part;
    }

    CHECK (total == 960);
    CHECK (parts.size () > 1);

    for (size_t i = 0; i + 1 < parts.size (); i++) {
	CHECK (parts[i] % 4 == 0);
    }
}

TEST_CASE ("A stopping voice fades from its place to the quietest upcoming sample") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 400, [] (int i, int) {
			    return static_cast<int> (10000.0 * std::sin (static_cast<double> (i) * 3.14159265 / 20.0));
			}));

    REQUIRE (readFrames (stream, 30).size () == 60);

    std::vector<float> tail (64 * 2);

    stream.stopTail (tail.data (), 64);

    CHECK (stream.ended ());
    CHECK (std::abs (tail[63 * 2]) < 0.05f);
    CHECK (tail[63 * 2] == tail[62 * 2]);
}

TEST_CASE ("A file that is not audio is refused") {
    Device device (48000);
    auto buffer = std::make_unique<char[]> (64);

    std::memset (buffer.get (), 0x55, 64);

    CHECK_THROWS (AudioStream (device.audio, std::make_shared<MemoryStream> (std::move (buffer), 64)));
}

namespace {
int mixedTone (int i, int) {
    return static_cast<int> (
	9000.0 * std::sin (static_cast<double> (i) * 0.173) + 3000.0 * std::sin (static_cast<double> (i) * 0.031)
    );
}

std::vector<float> model (int fileRate, int deviceRate, int frames, int length, int (*sample) (int, int)) {
    const auto step = static_cast<uint32_t> (
	std::lrint (static_cast<float> (fileRate) / static_cast<float> (deviceRate) * 4096.0f)
    );
    std::vector<float> out;
    uint64_t pos = 0;
    uint32_t frac = 0;

    for (int i = 0; i < frames; i++) {
	const int a = sample (static_cast<int> (std::min<uint64_t> (pos, length - 1)), 0);
	const int b = sample (static_cast<int> (std::min<uint64_t> (pos + 1, length - 1)), 0);
	const float value
	    = (static_cast<float> (a) + static_cast<float> (b - a) * (static_cast<float> (frac) / 4096.0f)) * Scale;

	out.push_back (value);
	frac += step;
	pos += frac >> 12;
	frac &= 4095;
    }

    return out;
}
}

TEST_CASE ("Resampling follows OpenAL's fixed point steps whatever the update size") {
    for (const auto [fileRate, deviceRate] :
	 { std::pair { 44100, 48000 }, std::pair { 48000, 44100 }, std::pair { 22050, 48000 } }) {
	Device device (deviceRate);
	AudioStream stream (device.audio, wav (fileRate, 1, 12000, mixedTone));
	const auto out = readFrames (stream, 9000, 333);
	const auto expected = model (fileRate, deviceRate, 9000, 12000, mixedTone);

	REQUIRE (out.size () == 18000);

	for (int i = 0; i < 9000; i++) {
	    CAPTURE (fileRate, deviceRate, i);
	    REQUIRE (out[i * 2] == Approx (expected[i]).margin (2e-5));
	}
    }
}

TEST_CASE ("The queue counters follow the packets in and out") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 2, 200000, [] (int i, int c) { return (i * 3 + c) % 20000; }));
    const auto deadline = std::chrono::steady_clock::now () + 5s;

    while (stream.getQueuePacketCount () == 0 && std::chrono::steady_clock::now () < deadline) {
	std::this_thread::sleep_for (1ms);
    }

    CHECK (stream.getQueuePacketCount () > 0);
    CHECK (stream.getQueueSize () > 0);
    CHECK (stream.getQueueDuration () > 0);
    CHECK (stream.getTimeBase ().den > 0);

    std::vector<float> out (960 * 2);
    int total = 0;

    while (std::chrono::steady_clock::now () < deadline) {
	const int got = stream.read (out.data (), 960);

	if (got < 0) {
	    break;
	}

	total += got;
	CHECK (stream.getQueuePacketCount () >= 0);
	CHECK (stream.getQueueDuration () >= 0);
    }

    CHECK (total >= 200000);
    CHECK (stream.getQueuePacketCount () == 0);
    CHECK (stream.getQueueSize () == 0);
    CHECK (stream.getQueueDuration () == 0);
}

TEST_CASE ("The mixing passes split a fast source into runs that fit the line") {
    Device device (48000);
    AudioStream stream (device.audio, wav (100000, 1, 4000, mixedTone));

    REQUIRE (readFrames (stream, 107).size () == 214);

    CHECK (stream.passes (100) == std::vector<int> { 100 });
    CHECK (stream.passes (960) == std::vector<int> { 488, 472 });
    CHECK (stream.passes (5000) == std::vector<int> { 488, 488, 488, 488, 488, 488, 488, 488, 488, 488, 120 });
}

TEST_CASE ("A stopping voice runs on to its quietest upcoming sample and holds it") {
    struct Expected {
	int fileRate;
	std::vector<float> every3rd;
    };
    const Expected cases[] = {
	{ 100000, { 0.262288809f, 0.322106659f, 0.109198041f, 0.0361938477f, 0.0361938477f, 0.0361938477f } },
	{ 44100, { -0.255040616f, -0.267906129f, -0.219908535f, -0.124591976f, -0.0640258789f, -0.0640258789f } },
	{ 48000, { -0.107177734f, 0.0249938965f, 0.144439697f, 0.217590332f, 0.223175049f, 0.157867432f } },
    };

    for (const auto& expected : cases) {
	Device device (48000);
	AudioStream stream (device.audio, wav (expected.fileRate, 1, 4000, mixedTone));

	REQUIRE (readFrames (stream, 107).size () == 214);

	std::vector<float> tail (40 * 2);

	stream.stopTail (tail.data (), 40);

	CAPTURE (expected.fileRate);

	for (size_t k = 0; k < expected.every3rd.size (); k++) {
	    CHECK (tail[k * 3 * 2] == Approx (expected.every3rd[k]).margin (1e-6));
	}

	CHECK (tail[39 * 2] == tail[38 * 2]);
	CHECK (tail[0] == tail[1]);
    }
}

TEST_CASE ("The mixing passes break at the line size whatever the fraction") {
    struct Expected {
	int frames;
	int read;
	std::vector<int> passes;
    };
    const Expected cases[] = {
	{ 492, 0, { 492 } },
	{ 493, 0, { 488, 5 } },
	{ 980, 0, { 488, 488, 4 } },
	{ 1956, 1, { 488, 488, 488, 488, 4 } },
	{ 1468, 33, { 488, 488, 492 } },
	{ 1956, 107, { 488, 488, 488, 488, 4 } },
    };

    for (const auto& expected : cases) {
	Device device (48000);
	AudioStream stream (device.audio, wav (100000, 1, 4000, mixedTone));

	if (expected.read > 0) {
	    REQUIRE (readFrames (stream, expected.read).size () == static_cast<size_t> (expected.read) * 2);
	}

	CAPTURE (expected.frames, expected.read);
	CHECK (stream.passes (expected.frames) == expected.passes);
    }
}

namespace {
void waitForPackets (AudioStream& stream) {
    const auto deadline = std::chrono::steady_clock::now () + 5s;

    while (stream.getQueuePacketCount () == 0 && std::chrono::steady_clock::now () < deadline) {
	std::this_thread::sleep_for (1ms);
    }

    REQUIRE (stream.getQueuePacketCount () > 0);
}
}

TEST_CASE ("A stopping voice before its first update fades from silence") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 4000, [] (int i, int) { return 100 + i * 20; }));

    waitForPackets (stream);

    std::vector<float> tail (40 * 2 + 2, 7.0f);

    stream.stopTail (tail.data (), 40);

    for (int i = 0; i < 80; i++) {
	CAPTURE (i);
	CHECK (tail[i] == 0.0f);
    }

    CHECK (tail[80] == 7.0f);
    CHECK (tail[81] == 7.0f);
}

TEST_CASE ("A stopping voice with nothing quieter ahead holds the sample before its place") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 4000, [] (int i, int) { return 100 + i * 20; }));

    REQUIRE (readFrames (stream, 30).size () == 60);

    std::vector<float> tail (40 * 2);

    stream.stopTail (tail.data (), 40);

    for (const float value : tail) {
	CHECK (value == Approx (static_cast<float> (100 + 29 * 20) * Scale).margin (1e-6));
    }
}

TEST_CASE ("A stopping voice on equal samples stops at the first of them") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 4000, [] (int, int) { return 5000; }));

    waitForPackets (stream);

    std::vector<float> tail (40 * 2);

    stream.stopTail (tail.data (), 40);

    for (const float value : tail) {
	CHECK (value == 0.0f);
    }
}

TEST_CASE ("A stopping voice looks as far ahead as its padding and no further") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 4000, [] (int i, int) { return 30000 - i * 20; }));

    REQUIRE (readFrames (stream, 30).size () == 60);

    std::vector<float> tail (40 * 2);

    stream.stopTail (tail.data (), 40);

    for (int i = 0; i < 40; i++) {
	CAPTURE (i);
	CHECK (tail[i * 2] == Approx (static_cast<float> (30000 - (30 + std::min (i, 22)) * 20) * Scale).margin (1e-6));
    }
}

namespace {
void waitForReader (AudioStream& stream) {
    int last = -1;
    int same = 0;
    const auto deadline = std::chrono::steady_clock::now () + 5s;

    while (same < 30 && std::chrono::steady_clock::now () < deadline) {
	std::this_thread::sleep_for (2ms);

	const int now = stream.getQueuePacketCount ();

	same = now == last && now > 0 ? same + 1 : 0;
	last = now;
    }

    REQUIRE (same >= 30);
}
}

TEST_CASE ("An update with the whole file queued is served in full") {
    Device device (48000);
    AudioStream stream (device.audio, wav (48000, 1, 4000, mixedTone));

    waitForReader (stream);

    std::vector<float> out (960 * 2);

    CHECK (stream.read (out.data (), 960) == 960);
    CHECK (stream.read (out.data (), 960) == 960);
    CHECK (stream.read (out.data (), 960) == 960);
    CHECK_FALSE (stream.ended ());
}

TEST_CASE ("A file that ends exactly with an update ends the voice there") {
    for (const int frames : { 960, 1920 }) {
	Device device (48000);
	AudioStream stream (device.audio, wav (48000, 1, frames, mixedTone));

	waitForReader (stream);

	std::vector<float> out (960 * 2);

	for (int done = 0; done < frames; done += 960) {
	    CAPTURE (frames, done);
	    CHECK_FALSE (stream.ended ());
	    CHECK (stream.read (out.data (), 960) == 960);
	}

	CHECK (stream.ended ());
	CHECK (stream.read (out.data (), 960) == -1);
    }
}
