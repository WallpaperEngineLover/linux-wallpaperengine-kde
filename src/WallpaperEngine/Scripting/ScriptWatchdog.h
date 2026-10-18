#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <v8-isolate.h>

namespace WallpaperEngine::Scripting {
/**
 * Stops script code that runs too long. begin () sets a deadline for one call into a script, end () clears it, and a
 * thread looks at it a few times a second: past the deadline it calls the isolate's TerminateExecution, which unwinds
 * whatever the script is doing (a script can't catch it) and leaves the embedder to resume with
 * CancelTerminateExecution. Polling keeps begin () and end () to a lock and a clock read, they run for every update ()
 * of every script every frame.
 */
class ScriptWatchdog {
public:
    explicit ScriptWatchdog (v8::Isolate* isolate);
    ~ScriptWatchdog ();
    ScriptWatchdog (const ScriptWatchdog&) = delete;
    ScriptWatchdog& operator= (const ScriptWatchdog&) = delete;

    void begin (std::chrono::milliseconds budget);
    bool fired ();
    bool end ();

private:
    void run ();

    v8::Isolate* m_isolate;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::chrono::steady_clock::time_point m_deadline {};
    bool m_armed = false;
    bool m_fired = false;
    bool m_stop = false;
    std::thread m_thread;
};
}
