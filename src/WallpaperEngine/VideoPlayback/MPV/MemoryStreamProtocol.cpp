#include "MemoryStreamProtocol.h"

#include "GLPlayer.h"

#include <mpv/stream_cb.h>

using namespace WallpaperEngine::VideoPlayback::MPV;

int64_t mem_seek (void* cookie, const int64_t offset) {
    const auto stream = static_cast<MemoryStreamProtocol*> (cookie);

    // a seek can happen while the stream is in a fail state from an earlier mpv operation;
    // clear it and retry, the failbit gets set again if this seek also fails
    if (stream->fail ()) {
	stream->clear ();
    }

    stream->seekg (offset, std::ios_base::beg);

    return stream->tellg ();
}

int64_t mem_read (void* cookie, char* buf, uint64_t bytes) {
    return static_cast<MemoryStreamProtocol*> (cookie)->read (buf, bytes).gcount ();
}

int64_t mem_size (void* cookie) { return static_cast<MemoryStreamProtocol*> (cookie)->m_size; }

void mem_close (void* cookie) {
    // reset position for the next play; the stream's lifetime is owned elsewhere, so there's
    // nothing to free here - but that also means two instances can't share the same stream
    mem_seek (cookie, 0);
}

int mem_open (void* userdata, char* uri, struct mpv_stream_cb_info* info) {
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