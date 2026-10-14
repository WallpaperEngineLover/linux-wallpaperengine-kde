#include "WebHostSharedMemory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <linux/futex.h>
#include <map>
#include <mutex>
#include <new>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::WebBrowser::IPC;

namespace {
std::atomic<uint32_t> g_nameCounter { 0 };

std::string generateName () {
    return "/lwe-web-" + WallpaperEngine::Desktop::processTag () + "-" + std::to_string (g_nameCounter.fetch_add (1));
}

// a shared flock on an open descriptor marks a segment in use for as long as the descriptor stays open: unlike a pid
// in its name this works across pid namespaces (every Flatpak run has its own, /dev/shm is the host's)
std::mutex g_heldMutex;
std::map<std::string, int> g_held;

void hold (const std::string& name, int fd) {
    if (flock (fd, LOCK_SH) != 0) {
	close (fd);
	return;
    }

    const std::scoped_lock lock (g_heldMutex);
    g_held[name] = fd;
}

void release (const std::string& name) {
    const std::scoped_lock lock (g_heldMutex);

    if (const auto it = g_held.find (name); it != g_held.end ()) {
	close (it->second);
	g_held.erase (it);
    }
}
} // namespace

void WebHostSharedMemory::requestFrame () {
    this->frameRequestSeq.fetch_add (1, std::memory_order_release);
    // not the _PRIVATE variant, the waiter is in another process
    syscall (SYS_futex, reinterpret_cast<uint32_t*> (&this->frameRequestSeq), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

uint32_t WebHostSharedMemory::waitForFrameRequest (const uint32_t lastSeen, const uint32_t timeoutMs) {
    timespec timeout { .tv_sec = static_cast<time_t> (timeoutMs / 1000),
		       .tv_nsec = static_cast<long> (timeoutMs % 1000) * 1000000L };

    // returns right away if the value already changed, so a request that lands between the caller's last look and
    // this call is never missed
    syscall (
	SYS_futex, reinterpret_cast<uint32_t*> (&this->frameRequestSeq), FUTEX_WAIT, lastSeen, &timeout, nullptr, 0
    );

    return this->frameRequestSeq.load (std::memory_order_acquire);
}

namespace {
/**
 * Engines from before the locks never take one, so an unlocked name tagged with a plain pid (no Flatpak instance) of a
 * process that still runs is left alone
 */
bool pidOwnerAlive (const std::string& name, const std::string& prefix) {
    if (!name.starts_with (prefix)) {
	return false;
    }

    const std::size_t end = name.find ('-', prefix.size ());
    const std::string tag = name.substr (prefix.size (), end == std::string::npos ? 0 : end - prefix.size ());

    if (tag.empty () || tag.size () > 9
	|| !std::ranges::all_of (tag, [] (unsigned char c) { return std::isdigit (c); })) {
	return false;
    }

    const auto pid = static_cast<pid_t> (std::stol (tag));
    return pid > 0 && (kill (pid, 0) == 0 || errno == EPERM);
}
} // namespace

bool WallpaperEngine::WebBrowser::IPC::isLockedByAnyone (const std::string& path) {
    const int fd = open (path.c_str (), O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
	return false;
    }

    const bool locked = flock (fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    close (fd);
    return locked;
}

void WallpaperEngine::WebBrowser::IPC::removeStaleWebHostFiles () {
    std::error_code error;

    for (const auto& entry : std::filesystem::directory_iterator ("/dev/shm", error)) {
	const std::string name = entry.path ().filename ().string ();

	if (name.starts_with ("lwe-web-") && !isLockedByAnyone (entry.path ().string ())
	    && !pidOwnerAlive (name, "lwe-web-")) {
	    shm_unlink (("/" + name).c_str ());
	}
    }

    error.clear ();

    for (const auto& entry : std::filesystem::directory_iterator (std::filesystem::temp_directory_path (), error)) {
	const std::string name = entry.path ().filename ().string ();

	if (!name.starts_with ("lwe-cef-") || pidOwnerAlive (name, "lwe-cef-")) {
	    continue;
	}

	// the host locks it right after creating the folder, give one that has no lock file yet a minute
	const auto lockFile = entry.path () / CEF_PROFILE_LOCK;
	std::error_code timeError;
	const auto age = std::filesystem::file_time_type::clock::now ()
	    - std::filesystem::last_write_time (entry.path (), timeError);

	if (isLockedByAnyone (lockFile.string ())
	    || (!std::filesystem::exists (lockFile, timeError) && age < std::chrono::minutes (1))) {
	    continue;
	}

	std::error_code removeError;
	std::filesystem::remove_all (entry.path (), removeError);
    }
}

WebHostSharedMemory*
WallpaperEngine::WebBrowser::IPC::createSharedMemory (std::string& outName, uint32_t width, uint32_t height) {
    // once per engine process, before the first segment of this run exists
    static const bool swept = (removeStaleWebHostFiles (), true);
    (void)swept;

    const std::size_t size = WebHostSharedMemory::totalSize (width, height);
    outName = generateName ();

    const int fd = shm_open (outName.c_str (), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
	sLog.error ("Failed to create shared memory segment ", outName);
	return nullptr;
    }

    if (ftruncate (fd, static_cast<off_t> (size)) != 0) {
	sLog.error ("Failed to size shared memory segment ", outName);
	close (fd);
	shm_unlink (outName.c_str ());
	return nullptr;
    }

    void* mapping = mmap (nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (mapping == MAP_FAILED) {
	sLog.error ("Failed to map shared memory segment ", outName);
	close (fd);
	shm_unlink (outName.c_str ());
	return nullptr;
    }

    hold (outName, fd);

    auto* mem = new (mapping) WebHostSharedMemory ();
    mem->maxWidth = width;
    mem->maxHeight = height;

    return mem;
}

WebHostSharedMemory*
WallpaperEngine::WebBrowser::IPC::attachSharedMemory (const std::string& name, uint32_t width, uint32_t height) {
    const std::size_t size = WebHostSharedMemory::totalSize (width, height);

    const int fd = shm_open (name.c_str (), O_RDWR, 0600);
    if (fd < 0) {
	sLog.error ("Failed to open shared memory segment ", name);
	return nullptr;
    }

    void* mapping = mmap (nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (mapping == MAP_FAILED) {
	sLog.error ("Failed to map shared memory segment ", name);
	close (fd);
	return nullptr;
    }

    hold (name, fd);

    return static_cast<WebHostSharedMemory*> (mapping);
}

void WallpaperEngine::WebBrowser::IPC::closeSharedMemory (
    WebHostSharedMemory* mem, const std::string& name, uint32_t width, uint32_t height, bool unlinkSegment
) {
    if (mem != nullptr) {
	munmap (mem, WebHostSharedMemory::totalSize (width, height));
    }

    release (name);

    if (unlinkSegment) {
	shm_unlink (name.c_str ());
    }
}
