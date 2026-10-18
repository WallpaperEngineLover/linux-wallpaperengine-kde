#include "AudioContext.h"
#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"

namespace WallpaperEngine::Audio {
AudioContext::AudioContext (Drivers::AudioDriver& driver) : m_driver (driver) { }

int AudioContext::addStream (AudioStream* stream, int volume, float left, float right) const {
    return this->m_driver.addStream (stream, volume, left, right);
}
void AudioContext::removeStream (int streamId) const { this->m_driver.removeStream (streamId); }

void AudioContext::setStreamVolume (int streamId, int volume) const {
    this->m_driver.setStreamVolume (streamId, volume);
}

void AudioContext::setStreamGains (int streamId, float left, float right) const {
    this->m_driver.setStreamGains (streamId, left, right);
}

void AudioContext::setStreamPaused (int streamId, bool paused) const {
    this->m_driver.setStreamPaused (streamId, paused);
}

void AudioContext::stopStream (int streamId, std::unique_ptr<AudioStream> stream) const {
    this->m_driver.stopStream (streamId, std::move (stream));
}

void AudioContext::setStreamHrtf (int streamId, const Drivers::StreamHrtf* hrtf) const {
    this->m_driver.setStreamHrtf (streamId, hrtf);
}

const Hrtf::Store* AudioContext::getHrtf () const { return this->m_driver.getHrtf (); }

AVSampleFormat AudioContext::getFormat () const { return this->m_driver.getFormat (); }

int AudioContext::getSampleRate () const { return this->m_driver.getSampleRate (); }

int AudioContext::getChannels () const { return this->m_driver.getChannels (); }

Application::ApplicationContext& AudioContext::getApplicationContext () const {
    return this->m_driver.getApplicationContext ();
}

Drivers::Recorders::PlaybackRecorder& AudioContext::getRecorder () const { return this->m_driver.getRecorder (); }

Drivers::AudioDriver& AudioContext::getDriver () const { return this->m_driver; }
}