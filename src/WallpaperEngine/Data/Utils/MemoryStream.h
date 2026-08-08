#pragma once

#include <algorithm>
#include <iostream>
#include <memory>

namespace WallpaperEngine::Data::Utils {
struct MemoryStream : std::istream, private std::streambuf {
    MemoryStream (std::unique_ptr<char[]> buffer, const size_t size) :
	std::istream (this), m_buffer (std::move (buffer)) {
	this->setg (this->m_buffer.get (), this->m_buffer.get (), this->m_buffer.get () + size);
    }

    // Callers occasionally derive seek offsets from untrusted file contents (e.g. a corrupt or
    // unrecognized .mdl section) and never clamp them. Without clamping here, gptr() can end up past
    // egptr(); the next read then has libstdc++'s xsgetn compute a negative "bytes available" count,
    // which turns into a huge size_t passed to memmove and segfaults.
    std::streambuf::pos_type
    seekoff (std::streambuf::off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
	char* target = gptr ();
	if (dir == std::ios_base::cur) {
	    target = gptr () + off;
	} else if (dir == std::ios_base::end) {
	    target = egptr () + off;
	} else if (dir == std::ios_base::beg) {
	    target = eback () + off;
	}

	target = std::min (std::max (target, eback ()), egptr ());
	setg (eback (), target, egptr ());

	return gptr () - eback ();
    }

    std::unique_ptr<char[]> m_buffer;
};

using MemoryStreamSharedPtr = std::shared_ptr<MemoryStream>;
using MemoryStreamUniquePtr = std::unique_ptr<MemoryStream>;
}