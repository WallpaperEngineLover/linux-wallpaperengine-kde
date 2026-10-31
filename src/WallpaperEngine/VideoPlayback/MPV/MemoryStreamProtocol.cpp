#include "MemoryStreamProtocol.h"

#include "GLPlayer.h"

#include <mpv/stream_cb.h>

using namespace WallpaperEngine::VideoPlayback::MPV;

static int64_t mem_seek (void* cookie, const int64_t offset) {
    const auto stream = static_cast<MemoryStreamProtocol*> (cookie);

    // clear a fail state left by an earlier operation, failbit returns if this seek fails too
    if (stream->fail ()) {
	stream->clear ();
    }

    stream->seekg (offset, std::ios_base::beg);

    return stream->tellg ();
}

static int64_t mem_read (void* cookie, char* buf, uint64_t bytes) {
    return static_cast<MemoryStreamProtocol*> (cookie)->read (buf, bytes).gcount ();
}

static int64_t mem_size (void* cookie) { return static_cast<MemoryStreamProtocol*> (cookie)->m_size; }

static void mem_close (void* cookie) {
    // rewind for the next play; the stream is owned elsewhere, so instances can't share one
    mem_seek (cookie, 0);
}

static int mem_open (void* userdata, char* uri, struct mpv_stream_cb_info* info) {
    info->cookie = userdata;
    info->read_fn = mem_read;
    info->seek_fn = mem_seek;
    info->close_fn = mem_close;
    info->size_fn = mem_size;

    return 0;
}

void MemoryStreamProtocol::registerReadCallback (mpv_handle* handle) {
    if (mpv_stream_cb_add_ro (handle, "buffer", this, mem_open) < 0) {
	sLog.exception ("Cannot register memory stream protocol for mpv");
    }
}