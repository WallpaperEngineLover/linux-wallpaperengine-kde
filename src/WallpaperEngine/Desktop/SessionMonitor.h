#pragma once

#include <atomic>
#include <memory>
#include <thread>

namespace WallpaperEngine::Desktop {
/**
 * Whether the session is locked: logind's LockedHint of this session, or a screen saver that says it is active
 * (org.freedesktop.ScreenSaver on KDE and others, org.gnome.ScreenSaver, Cinnamon, MATE). And whether the displays are
 * blanked, where something tells: the DPMS state of an X server, Mutter's PowerSaveMode. Wayland compositors without
 * either are left to their frame callbacks, which most stop for a blanked output. Runs on a thread of its own so the
 * render thread never waits for a bus call.
 */
class SessionMonitor {
public:
    SessionMonitor ();
    ~SessionMonitor ();

    SessionMonitor (const SessionMonitor&) = delete;
    SessionMonitor& operator= (const SessionMonitor&) = delete;

    [[nodiscard]] bool locked () const { return this->m_locked; }
    [[nodiscard]] bool displayOff () const { return this->m_displayOff; }

    struct Sources;

private:
    void run ();
    void wake () const;

    std::atomic<bool> m_locked = false;
    std::atomic<bool> m_displayOff = false;
    std::atomic<bool> m_stop = false;
    int m_wakeFd = -1;
    std::unique_ptr<Sources> m_sources;
    std::thread m_thread;
};
}
