#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <thread>

namespace WallpaperEngine::Desktop {
std::optional<bool> sysfsOnBattery (const std::filesystem::path& powerSupplies);

class PowerMonitor {
public:
    PowerMonitor ();
    ~PowerMonitor ();

    PowerMonitor (const PowerMonitor&) = delete;
    PowerMonitor& operator= (const PowerMonitor&) = delete;

    [[nodiscard]] bool onBattery () const { return this->m_onBattery; }
    [[nodiscard]] bool idle () const { return this->m_idle; }

    /** 0 turns idle detection off */
    void setIdleTimeout (int minutes);

    struct Sources;

private:
    void run ();
    void wake () const;

    std::atomic<bool> m_onBattery = false;
    std::atomic<bool> m_idle = false;
    std::atomic<int> m_idleMinutes = 0;
    std::atomic<bool> m_stop = false;
    int m_wakeFd = -1;
    Sources* m_sources = nullptr;
    std::thread m_thread;
};
}
