#include "WebHostSharedMemory.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <fcntl.h>
#include <linux/futex.h>
#include <new>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::WebBrowser::IPC;

namespace {
std::atomic<uint32_t> g_nameCounter { 0 };

std::string generateName () {
    return "/lwe-web-" + std::to_string (getpid ()) + "-" + std::to_string (g_nameCounter.fetch_add (1));
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
// "<prefix><pid>-<rest>" -> pid, or 0 if the name isn't ours
pid_t pidFromName (const std::string& name, const std::string& prefix) {
    if (!name.starts_with (prefix)) {
	return 0;
    }

    const std::size_t end = name.find ('-', prefix.size ());

    if (end == std::string::npos || end == prefix.size ()) {
	return 0;
    }

    try {
	return static_cast<pid_t> (std::stol (name.substr (prefix.size (), end - prefix.size ())));
    } catch (...) {
	return 0;
    }
}

bool processExists (pid_t pid) { return kill (pid, 0) == 0 || errno == EPERM; }
} // namespace

void WallpaperEngine::WebBrowser::IPC::removeStaleWebHostFiles () {
    std::error_code error;

    for (const auto& entry : std::filesystem::directory_iterator ("/dev/shm", error)) {
	const std::string name = entry.path ().filename ().string ();
	const pid_t pid = pidFromName (name, "lwe-web-");

	if (pid > 0 && !processExists (pid)) {
	    shm_unlink (("/" + name).c_str ());
	}
    }

    error.clear ();

    for (const auto& entry : std::filesystem::directory_iterator (std::filesystem::temp_directory_path (), error)) {
	const pid_t pid = pidFromName (entry.path ().filename ().string (), "lwe-cef-");

	if (pid > 0 && !processExists (pid)) {
	    std::error_code removeError;
	    std::filesystem::remove_all (entry.path (), removeError);
	}
    }
}

WebHostSharedMemory* WallpaperEngine::WebBrowser::IPC::createSharedMemory (
    std::string& outName, uint32_t width, uint32_t height
) {
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
    close (fd);

    if (mapping == MAP_FAILED) {
	sLog.error ("Failed to map shared memory segment ", outName);
	shm_unlink (outName.c_str ());
	return nullptr;
    }

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
    close (fd);

    if (mapping == MAP_FAILED) {
	sLog.error ("Failed to map shared memory segment ", name);
	return nullptr;
    }

    return static_cast<WebHostSharedMemory*> (mapping);
}

void WallpaperEngine::WebBrowser::IPC::closeSharedMemory (
    WebHostSharedMemory* mem, const std::string& name, uint32_t width, uint32_t height, bool unlinkSegment
) {
    if (mem != nullptr) {
	munmap (mem, WebHostSharedMemory::totalSize (width, height));
    }

    if (unlinkSegment) {
	shm_unlink (name.c_str ());
    }
}
