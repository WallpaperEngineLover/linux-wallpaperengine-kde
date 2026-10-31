#include <fstream>
#include <memory>

#include "Package.h"

#include "WallpaperEngine/Data/Parsers/PackageParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"

#include <algorithm>
#include <cctype>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

// scenes are authored on Windows, so fall back to a case-insensitive lookup when the exact path is missing
static bool equalsIgnoreCase (const std::string& a, const std::string& b) {
    return std::ranges::equal (a, b, [] (unsigned char x, unsigned char y) {
	return std::tolower (x) == std::tolower (y);
    });
}

template <typename Files> static auto findFile (const Files& files, const std::filesystem::path& path) {
    const auto wanted = path.string ();
    const auto exact = std::ranges::find_if (files, [&wanted] (const auto& file) { return file->filename == wanted; });

    if (exact != files.end ()) {
	return exact;
    }

    return std::ranges::find_if (files, [&wanted] (const auto& file) {
	return equalsIgnoreCase (file->filename, wanted);
    });
}

PackageAdapter::~PackageAdapter () {
    if (this->fd >= 0) {
	close (this->fd);
    }
}

ReadStreamSharedPtr PackageAdapter::open (const std::filesystem::path& path) const {
    const auto it = findFile (this->package->files, path);

    if (it == this->package->files.end ()) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    const uint64_t start = static_cast<uint64_t> (it->get ()->offset) + this->package->baseOffset;

    // an entry that runs past the end (corrupt or truncated package) must not allocate its claimed size
    if (start + it->get ()->length > this->package->file->size ()) {
	throw std::filesystem::filesystem_error (
	    "Package entry runs past the end of the package", path,
	    std::make_error_code (std::errc::result_out_of_range)
	);
    }

    auto buffer = std::make_unique<char[]> (it->get ()->length);

    if (this->fd >= 0) {
	uint64_t done = 0;

	while (done < it->get ()->length) {
	    const ssize_t got
		= pread (this->fd, buffer.get () + done, it->get ()->length - done, static_cast<off_t> (start + done));

	    if (got < 0 && errno == EINTR) {
		continue;
	    }

	    if (got <= 0) {
		throw std::filesystem::filesystem_error (
		    "Cannot read the package entry", path, std::make_error_code (std::errc::io_error)
		);
	    }

	    done += static_cast<uint64_t> (got);
	}

	return std::make_shared<MemoryStream> (std::move (buffer), it->get ()->length);
    }

    const Debugging::LockGuard lock (this->m_readMutex);

    // the stream is shared by every file of the package, nothing an earlier read left behind may carry over
    this->package->file->base ().clear ();
    this->package->file->base ().seekg (static_cast<std::streamoff> (start), std::ios::beg);
    this->package->file->next (buffer.get (), it->get ()->length);

    return std::make_shared<MemoryStream> (std::move (buffer), it->get ()->length);
}

bool PackageAdapter::exists (const std::filesystem::path& path) const {
    return findFile (this->package->files, path) != this->package->files.end ();
}

std::filesystem::path PackageAdapter::physicalPath (const std::filesystem::path& path) const {
    throw std::filesystem::filesystem_error ("Package adapter does not support realpath", path, std::error_code ());
}

std::optional<std::filesystem::path>
PackageAdapter::resolveWorkshopDependencyAlias (const std::filesystem::path& path) const {
    std::optional<std::filesystem::path> found = std::nullopt;

    for (const auto& file : this->package->files) {
	if (!isWorkshopDependencyAliasOf (file->filename, path)) {
	    continue;
	}

	if (found.has_value ()) {
	    // more than one dependency ships a same-named file under this path, refuse to guess
	    return std::nullopt;
	}

	found = file->filename;
    }

    return found;
}

std::vector<std::string> PackageAdapter::listFiles (const std::filesystem::path& dir) const {
    std::vector<std::string> names;

    for (const auto& file : this->package->files) {
	const std::filesystem::path path = file->filename;

	if (equalsIgnoreCase (path.parent_path ().generic_string (), dir.generic_string ())) {
	    names.push_back (path.filename ().string ());
	}
    }

    return names;
}

bool PackageFactory::handlesMountpoint (const std::filesystem::path& path) const {
    try {
	const auto finalpath = std::filesystem::canonical (path);
	const auto status = std::filesystem::status (finalpath);

	return std::filesystem::exists (finalpath) && std::filesystem::is_regular_file (status)
	    && finalpath.extension () == ".pkg";
    } catch (std::filesystem::filesystem_error&) {
	return false;
    }
}

AdapterSharedPtr PackageFactory::create (const std::filesystem::path& path) const {
    // the table and the entries have to come from one file, so the package is parsed through the descriptor too
    int fd = ::open (path.c_str (), O_RDONLY | O_CLOEXEC);
    const std::filesystem::path source
	= fd >= 0 ? std::filesystem::path ("/proc/self/fd/" + std::to_string (fd)) : path;
    auto stream = std::make_shared<std::ifstream> (source, std::ios::binary);

    // without /proc the entries are read from this one stream as well, a second open could be another file
    if (!stream->is_open ()) {
	if (fd >= 0) {
	    close (fd);
	    fd = -1;
	}

	stream = std::make_shared<std::ifstream> (path, std::ios::binary);
    }

    std::unique_ptr<PackageAdapter> adapter;

    try {
	adapter = std::make_unique<PackageAdapter> (Data::Parsers::PackageParser::parse (stream));
    } catch (...) {
	if (fd >= 0) {
	    close (fd);
	}

	throw;
    }

    adapter->fd = fd;

    return adapter;
}
