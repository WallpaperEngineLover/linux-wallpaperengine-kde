#pragma once

#include <libavutil/samplefmt.h>
#include <vector>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PulseAudioPlaybackRecorder.h"

namespace WallpaperEngine {
namespace Application {
    class ApplicationContext;
}

namespace Audio {
    namespace Drivers {
	class AudioDriver;

	namespace Recorders {
	    class PulseAudioPlaybackRecorder;
	}
    } // namespace Drivers

    class AudioStream;

    class AudioContext {
    public:
	explicit AudioContext (Drivers::AudioDriver& driver);

	int addStream (AudioStream* stream, int volume = -1, float left = 1.0f, float right = 1.0f) const;

	void removeStream (int streamId) const;

	/**
	 * Overrides the volume used to mix a single stream, instead of the driver's global volume
	 *
	 * @param streamId The stream to change
	 * @param volume 0-128, or a negative value to go back to using the global volume
	 */
	void setStreamVolume (int streamId, int volume) const;

	/**
	 * Scales the left and right channel of a stream on top of its volume
	 */
	void setStreamGains (int streamId, float left, float right) const;

	void setStreamPaused (int streamId, bool paused) const;

	/**
	 * TODO: MAYBE THIS SHOULD BE OUR OWN DEFINITIONS INSTEAD OF LIBRARY SPECIFIC ONES?
	 *
	 * @return The audio format the driver supports
	 */
	[[nodiscard]] AVSampleFormat getFormat () const;
	[[nodiscard]] int getSampleRate () const;
	[[nodiscard]] int getChannels () const;
	Application::ApplicationContext& getApplicationContext () const;
	[[nodiscard]] Drivers::Recorders::PlaybackRecorder& getRecorder () const;

	[[nodiscard]] Drivers::AudioDriver& getDriver () const;

    private:
	Drivers::AudioDriver& m_driver;
    };
} // namespace Audio
} // namespace WallpaperEngine