#include "WebHostSharedMemory.h"

#include <atomic>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::WebBrowser::IPC;

namespace {
std::atomic<uint32_t> g_nameCounter { 0 };

std::string generateName () {
    return "/lwe-web-" + std::to_string (getpid ()) + "-" + std::to_string (g_nameCounter.fetch_add (1));
}
} // namespace

WebHostSharedMemory* WallpaperEngine::WebBrowser::IPC::createSharedMemory (
    std::string& outName, uint32_t width, uint32_t height
) {
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
