#pragma once

#include <vector>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/AudioPlayingDetector.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Audio/Hrtf.h"

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

	/** HRTF filters of a stream, one per input channel it uses: a mono file's first channel, a stereo file's two */
	struct StreamHrtf {
	    int channels = 1;
	    std::array<Hrtf::Filter, 2> filters {};
	};

	class AudioDriver {
	public:
	    explicit AudioDriver (
		Application::ApplicationContext& applicationContext, Detectors::AudioPlayingDetector& detector,
		Recorders::PlaybackRecorder& recorder
	    );

	    virtual ~AudioDriver () = default;
	    // volume 0-128 or -1 for the global volume, left/right are the initial channel gains
	    virtual int addStream (AudioStream* stream, int volume = -1, float left = 1.0f, float right = 1.0f) = 0;

	    virtual void removeStream (int streamId) = 0;

	    // 0-128, negative goes back to the global volume
	    virtual void setStreamVolume (int streamId, int volume) = 0;

	    // per channel gains on top of the volume
	    virtual void setStreamGains (int streamId, float left, float right) = 0;

	    // a paused stream keeps its position and isn't decoded
	    virtual void setStreamPaused (int streamId, bool paused) = 0;
	    // stops like an OpenAL source: one more update without new audio (an HRTF voice's delay fades out), then
	    // removed
	    virtual void stopStream (int streamId, std::unique_ptr<AudioStream> stream) {
		this->removeStream (streamId);
	    }
	    // mixes through HRTF with these filters while it's on, nullptr goes back to the channel gains
	    virtual void setStreamHrtf (int streamId, const StreamHrtf* hrtf) { }
	    // at the output rate while headphones rendering is on, nullptr otherwise
	    [[nodiscard]] virtual const Hrtf::Store* getHrtf () const { return nullptr; }

	    // dt is seconds since the previous frame, scaled by the playback speed
	    virtual void update (float dt);

	    // TODO: our own format definitions instead of ffmpeg's?
	    [[nodiscard]] virtual AVSampleFormat getFormat () const = 0;
	    [[nodiscard]] virtual int getSampleRate () const = 0;
	    [[nodiscard]] virtual int getChannels () const = 0;
	    Application::ApplicationContext& getApplicationContext () const;
	    // stops sound when something else starts playing
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