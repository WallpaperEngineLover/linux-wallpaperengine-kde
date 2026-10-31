#pragma once

#include <mutex>

// libstdc++'s std::mutex has no capability attributes, so clang's -Wthread-safety can't see which data a lock guards.
// Mutex, LockGuard and UniqueLock carry them; the attributes expand to nothing elsewhere
#if defined(__clang__)
#define LWE_TSA(attribute) __attribute__ ((attribute))
#else
#define LWE_TSA(attribute)
#endif

#define LWE_CAPABILITY(name) LWE_TSA (capability (name))
#define LWE_SCOPED_CAPABILITY LWE_TSA (scoped_lockable)
#define LWE_GUARDED_BY(mutex) LWE_TSA (guarded_by (mutex))
#define LWE_REQUIRES(mutex) LWE_TSA (requires_capability (mutex))
#define LWE_ACQUIRE(...) LWE_TSA (acquire_capability (__VA_ARGS__))
#define LWE_RELEASE(...) LWE_TSA (release_capability (__VA_ARGS__))
#define LWE_TRY_ACQUIRE(...) LWE_TSA (try_acquire_capability (__VA_ARGS__))
#define LWE_EXCLUDES(...) LWE_TSA (locks_excluded (__VA_ARGS__))
#define LWE_NO_THREAD_SAFETY_ANALYSIS LWE_TSA (no_thread_safety_analysis)

namespace WallpaperEngine::Debugging {
class LWE_CAPABILITY ("mutex") Mutex {
public:
    void lock () LWE_ACQUIRE () { this->m_mutex.lock (); }
    void unlock () LWE_RELEASE () { this->m_mutex.unlock (); }
    bool try_lock () LWE_TRY_ACQUIRE (true) { return this->m_mutex.try_lock (); }

private:
    std::mutex m_mutex;
};

class LWE_SCOPED_CAPABILITY LockGuard {
public:
    explicit LockGuard (Mutex& mutex) LWE_ACQUIRE (mutex) : m_mutex (mutex) { this->m_mutex.lock (); }
    ~LockGuard () LWE_RELEASE () { this->m_mutex.unlock (); }

    LockGuard (const LockGuard&) = delete;
    LockGuard& operator= (const LockGuard&) = delete;

private:
    Mutex& m_mutex;
};

class LWE_SCOPED_CAPABILITY UniqueLock {
public:
    explicit UniqueLock (Mutex& mutex) LWE_ACQUIRE (mutex) : m_mutex (mutex) {
	this->m_mutex.lock ();
	this->m_locked = true;
    }
    ~UniqueLock () LWE_RELEASE () {
	if (this->m_locked) {
	    this->m_mutex.unlock ();
	}
    }

    void lock () LWE_ACQUIRE () {
	this->m_mutex.lock ();
	this->m_locked = true;
    }
    void unlock () LWE_RELEASE () {
	this->m_locked = false;
	this->m_mutex.unlock ();
    }

    UniqueLock (const UniqueLock&) = delete;
    UniqueLock& operator= (const UniqueLock&) = delete;

private:
    Mutex& m_mutex;
    bool m_locked = false;
};
}
