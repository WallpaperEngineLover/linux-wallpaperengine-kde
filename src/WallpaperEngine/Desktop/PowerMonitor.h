#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <thread>

namespace WallpaperEngine::Desktop {
// a system battery is discharging and no external supply is online, nullopt without a system battery
std::optional<bool> sysfsOnBattery (const std::filesystem::path& powerSupplies);

/**
 * Battery (UPower, else sysfs) and idle state (ext-idle-notify-v1, MIT-SCREEN-SAVER or Mutter's IdleMonitor), polled
 * on its own thread so a slow bus never stalls a frame
 */
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
} // namespace WallpaperEngine::Desktop
