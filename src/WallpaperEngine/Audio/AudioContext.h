#pragma once

#include <memory>

#include <libavutil/samplefmt.h>
#include <vector>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PulseAudioPlaybackRecorder.h"
#include "WallpaperEngine/Audio/Hrtf.h"

namespace WallpaperEngine {
namespace Application {
    class ApplicationContext;
}

namespace Audio {
    namespace Drivers {
	class AudioDriver;
	struct StreamHrtf;

	namespace Recorders {
	    class PulseAudioPlaybackRecorder;
	}
    }

    class AudioStream;

    class AudioContext {
    public:
	explicit AudioContext (Drivers::AudioDriver& driver);

	int addStream (AudioStream* stream, int volume = -1, float left = 1.0f, float right = 1.0f) const;

	void removeStream (int streamId) const;

	// 0-128, negative goes back to the global volume
	void setStreamVolume (int streamId, int volume) const;

	void setStreamGains (int streamId, float left, float right) const;

	void setStreamPaused (int streamId, bool paused) const;
	/** Stops the stream like OpenAL's alSourceStop (one more silent update, see AudioDriver) and frees it */
	void stopStream (int streamId, std::unique_ptr<AudioStream> stream) const;
	void setStreamHrtf (int streamId, const Drivers::StreamHrtf* hrtf) const;
	[[nodiscard]] const Hrtf::Store* getHrtf () const;

	[[nodiscard]] AVSampleFormat getFormat () const;
	[[nodiscard]] int getSampleRate () const;
	[[nodiscard]] int getChannels () const;
	Application::ApplicationContext& getApplicationContext () const;
	[[nodiscard]] Drivers::Recorders::PlaybackRecorder& getRecorder () const;

	[[nodiscard]] Drivers::AudioDriver& getDriver () const;

    private:
	Drivers::AudioDriver& m_driver;
    };
}
}