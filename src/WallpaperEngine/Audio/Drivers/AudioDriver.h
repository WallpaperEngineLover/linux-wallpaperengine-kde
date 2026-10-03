#pragma once

#include <vector>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/AudioPlayingDetector.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"

namespace WallpaperEngine {
namespace Application {
    class ApplicationContext;
}

namespace Audio {
    class AudioStream;

    namespace Drivers {
	namespace Detectors {
	    class AudioPlayingDetector;
	}

	namespace Recorders {
	    class PulseAudioPlaybackRecorder;
	}

	class AudioDriver {
	public:
	    explicit AudioDriver (
		Application::ApplicationContext& applicationContext, Detectors::AudioPlayingDetector& detector,
		Recorders::PlaybackRecorder& recorder
	    );

	    virtual ~AudioDriver () = default;
	    /**
	     * Registers the given stream in the driver for playing
	     *
	     * @param volume Initial volume override (0-128), -1 = the global volume
	     * @param left Initial gain of the left channel
	     * @param right Initial gain of the right channel
	     */
	    virtual int addStream (AudioStream* stream, int volume = -1, float left = 1.0f, float right = 1.0f) = 0;

	    virtual void removeStream (int streamId) = 0;

	    /**
	     * Overrides the volume used to mix a single stream, instead of the driver's global volume
	     *
	     * @param streamId The stream to change
	     * @param volume 0-128, or a negative value to go back to using the global volume
	     */
	    virtual void setStreamVolume (int streamId, int volume) = 0;

	    /**
	     * Scales each output channel of a stream, on top of its volume
	     *
	     * @param left Gain of the left channel
	     * @param right Gain of the right channel
	     */
	    virtual void setStreamGains (int streamId, float left, float right) = 0;

	    /**
	     * A paused stream keeps its position and isn't decoded until it is resumed
	     */
	    virtual void setStreamPaused (int streamId, bool paused) = 0;

	    /**
	     * Updates status of the different audio settings
	     *
	     * @param dt Seconds since the previous frame, scaled by the playback speed
	     */
	    virtual void update (float dt);

	    /**
	     * TODO: MAYBE THIS SHOULD BE OUR OWN DEFINITIONS INSTEAD OF LIBRARY SPECIFIC ONES?
	     *
	     * @return The audio format the driver supports
	     */
	    [[nodiscard]] virtual AVSampleFormat getFormat () const = 0;
	    [[nodiscard]] virtual int getSampleRate () const = 0;
	    [[nodiscard]] virtual int getChannels () const = 0;
	    Application::ApplicationContext& getApplicationContext () const;
	    /**
	     * @return The audio playing detector to use to stop playing sound when something else starts playing
	     */
	    [[nodiscard]] Detectors::AudioPlayingDetector& getAudioDetector () const;
	    [[nodiscard]] Recorders::PlaybackRecorder& getRecorder () const;
	    void setRecorder (Recorders::PlaybackRecorder& recorder);

	private:
	    Application::ApplicationContext& m_applicationContext;
	    Detectors::AudioPlayingDetector& m_detector;
	    Recorders::PlaybackRecorder* m_recorder;
	};
    } // namespace Drivers
} // namespace Audio
} // namespace WallpaperEngine