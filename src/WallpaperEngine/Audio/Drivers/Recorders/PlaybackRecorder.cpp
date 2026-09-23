#include "PlaybackRecorder.h"

#include <ranges>

namespace WallpaperEngine::Audio::Drivers::Recorders {
void PlaybackRecorder::update () { }

int PlaybackRecorder::addSpectrumListener (SpectrumListener listener) {
    std::lock_guard guard (this->m_listenersMutex);

    const int id = ++this->m_nextListenerId;
    this->m_listeners.emplace (id, std::move (listener));

    return id;
}

void PlaybackRecorder::removeSpectrumListener (int id) {
    std::lock_guard guard (this->m_listenersMutex);

    this->m_listeners.erase (id);
}

void PlaybackRecorder::notifySpectrumListeners (const float* audio64) {
    std::lock_guard guard (this->m_listenersMutex);

    for (const auto& listener : this->m_listeners | std::views::values) {
	listener (audio64);
    }
}

} // namespace WallpaperEngine::Audio::Drivers::Recorders
