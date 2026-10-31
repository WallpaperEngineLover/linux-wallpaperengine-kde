#include "ScriptWatchdog.h"

#include "WallpaperEngine/Logging/ThreadGuard.h"

using namespace WallpaperEngine::Scripting;

namespace {
// how late a runaway script is noticed at the most, on top of its budget
constexpr std::chrono::milliseconds PollInterval { 100 };
}

ScriptWatchdog::ScriptWatchdog (v8::Isolate* isolate) : m_isolate (isolate) {
    this->m_thread
	= std::thread ([this] { WallpaperEngine::Logging::guardThread ("Script watchdog", [this] { this->run (); }); });
}

ScriptWatchdog::~ScriptWatchdog () {
    {
	const Debugging::LockGuard lock (this->m_mutex);
	this->m_stop = true;
    }

    this->m_wake.notify_all ();
    this->m_thread.join ();
}

void ScriptWatchdog::begin (const std::chrono::milliseconds budget) {
    const Debugging::LockGuard lock (this->m_mutex);

    this->m_deadline = std::chrono::steady_clock::now () + budget;
    this->m_armed = true;
    this->m_fired = false;
}

bool ScriptWatchdog::fired () {
    const Debugging::LockGuard lock (this->m_mutex);

    return this->m_fired;
}

bool ScriptWatchdog::end () {
    const Debugging::LockGuard lock (this->m_mutex);
    const bool fired = this->m_fired;

    this->m_armed = false;
    this->m_fired = false;

    return fired;
}

void ScriptWatchdog::run () {
    Debugging::UniqueLock lock (this->m_mutex);

    while (!this->m_stop) {
	this->m_wake.wait_for (lock, PollInterval, [this] () LWE_REQUIRES (this->m_mutex) { return this->m_stop; });

	// decided and done under the lock: a call that just ended (end ()) or began again can't be hit by a stale
	// decision
	if (this->m_armed && std::chrono::steady_clock::now () >= this->m_deadline) {
	    this->m_armed = false;
	    this->m_fired = true;
	    this->m_isolate->TerminateExecution ();
	}
    }
}
