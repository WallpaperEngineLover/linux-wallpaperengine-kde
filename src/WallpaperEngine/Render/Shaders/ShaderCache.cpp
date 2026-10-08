#include "ShaderCache.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <vector>

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render::Shaders;

namespace {
constexpr char MAGIC[8] = { 'L', 'W', 'E', 'S', 'H', 'C', '0', '1' };
// pruned to 3/4 of this on a run's first write and again every 1/16 of it written
constexpr uint64_t SIZE_LIMIT = 256ull * 1024 * 1024;

uint64_t fnv1a (const std::string& data) {
    uint64_t hash = 0xcbf29ce484222325ull;

    for (const unsigned char c : data) {
	hash = (hash ^ c) * 0x100000001b3ull;
    }

    return hash;
}

/** guards against file name collisions */
uint64_t mix64 (const std::string& data) {
    uint64_t hash = 0x9e3779b97f4a7c15ull ^ data.size ();
    size_t i = 0;

    const auto round = [&hash] (uint64_t value) {
	value *= 0xbf58476d1ce4e5b9ull;
	value ^= value >> 31;
	hash = (hash ^ value) * 0x94d049bb133111ebull;
	hash ^= hash >> 29;
    };

    for (; i + 8 <= data.size (); i += 8) {
	uint64_t value;
	memcpy (&value, data.data () + i, 8);
	round (value);
    }

    uint64_t tail = 0;
    memcpy (&tail, data.data () + i, data.size () - i);
    round (tail);

    return hash;
}

void appendU32 (std::string& out, uint32_t value) { out.append (reinterpret_cast<const char*> (&value), 4); }

void appendString (std::string& out, const std::string& value) {
    appendU32 (out, static_cast<uint32_t> (value.size ()));
    out += value;
}

bool readU32 (const std::string& in, size_t& offset, uint32_t& value) {
    if (offset + 4 > in.size ()) {
	return false;
    }

    memcpy (&value, in.data () + offset, 4);
    offset += 4;
    return true;
}

bool readString (const std::string& in, size_t& offset, std::string& value) {
    uint32_t size;

    if (!readU32 (in, offset, size) || offset + size > in.size ()) {
	return false;
    }

    value.assign (in, offset, size);
    offset += size;
    return true;
}

std::filesystem::path cacheDirectory () {
    if (const char* home = std::getenv ("XDG_CACHE_HOME"); home != nullptr && home[0] == '/') {
	return std::filesystem::path (home) / "linux-wallpaperengine" / "shaders";
    }

    if (const char* home = std::getenv ("HOME"); home != nullptr && home[0] != '\0') {
	return std::filesystem::path (home) / ".cache" / "linux-wallpaperengine" / "shaders";
    }

    return {};
}
} // namespace

ShaderCache& ShaderCache::get () {
    static ShaderCache instance;
    return instance;
}

ShaderCache::ShaderCache () : m_directory (cacheDirectory ()) { this->m_enabled = !this->m_directory.empty (); }

void ShaderCache::setEnabled (bool enabled) {
    std::lock_guard lock (this->m_mutex);
    this->m_enabled = enabled && !this->m_directory.empty ();
}

std::optional<ShaderCache::Entry> ShaderCache::entry (const std::string& kind, const std::string& key) const {
    if (!this->m_enabled) {
	return std::nullopt;
    }

    char name[17];
    snprintf (name, sizeof (name), "%016llx", static_cast<unsigned long long> (fnv1a (key)));

    return Entry { .path = this->m_directory / kind / name, .check = mix64 (key) };
}

std::optional<std::string> ShaderCache::read (const Entry& entry) const {
    std::ifstream file (entry.path, std::ios::binary);

    if (!file) {
	return std::nullopt;
    }

    std::string data ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
    uint64_t check;

    if (data.size () < sizeof (MAGIC) + 8 || memcmp (data.data (), MAGIC, sizeof (MAGIC)) != 0) {
	return std::nullopt;
    }

    memcpy (&check, data.data () + sizeof (MAGIC), 8);

    if (check != entry.check) {
	return std::nullopt;
    }

    std::error_code error;
    std::filesystem::last_write_time (entry.path, std::filesystem::file_time_type::clock::now (), error);

    return data.substr (sizeof (MAGIC) + 8);
}

void ShaderCache::write (const Entry& entry, const std::string& payload) {
    std::error_code error;
    std::filesystem::create_directories (entry.path.parent_path (), error);

    if (error) {
	return;
    }

    const auto temporary = entry.path.string () + "." + std::to_string (getpid ()) + ".tmp";

    {
	std::ofstream file (temporary, std::ios::binary | std::ios::trunc);

	if (!file) {
	    return;
	}

	file.write (MAGIC, sizeof (MAGIC));
	file.write (reinterpret_cast<const char*> (&entry.check), 8);
	file.write (payload.data (), static_cast<std::streamsize> (payload.size ()));

	if (!file) {
	    file.close ();
	    std::filesystem::remove (temporary, error);
	    return;
	}
    }

    std::filesystem::rename (temporary, entry.path, error);

    if (error) {
	std::filesystem::remove (temporary, error);
	return;
    }

    this->m_bytesWritten += payload.size ();

    if (!this->m_pruned || this->m_bytesWritten > SIZE_LIMIT / 16) {
	this->prune ();
    }
}

void ShaderCache::prune () {
    this->m_pruned = true;
    this->m_bytesWritten = 0;

    struct File {
	std::filesystem::path path;
	std::filesystem::file_time_type time;
	uintmax_t size;
    };

    std::vector<File> files;
    uintmax_t total = 0;
    std::error_code error;

    for (auto it = std::filesystem::recursive_directory_iterator (this->m_directory, error);
	 !error && it != std::filesystem::recursive_directory_iterator (); it.increment (error)) {
	if (!it->is_regular_file (error)) {
	    continue;
	}

	const auto size = it->file_size (error);
	const auto time = it->last_write_time (error);

	if (!error) {
	    files.push_back ({ it->path (), time, size });
	    total += size;
	}
    }

    if (total <= SIZE_LIMIT) {
	return;
    }

    std::ranges::sort (files, {}, &File::time);

    for (const auto& file : files) {
	if (total <= SIZE_LIMIT / 4 * 3) {
	    break;
	}

	if (std::filesystem::remove (file.path, error)) {
	    total -= file.size;
	}
    }
}

std::optional<GLSLContext::Sources> ShaderCache::loadTranslation (const std::string& key) {
    std::lock_guard lock (this->m_mutex);
    const auto entry = this->entry ("translations", key);

    if (!entry.has_value ()) {
	return std::nullopt;
    }

    const auto data = this->read (*entry);

    if (!data.has_value ()) {
	return std::nullopt;
    }

    GLSLContext::Sources sources;
    size_t offset = 0;

    if (!readString (*data, offset, sources.vertex) || !readString (*data, offset, sources.fragment)
	|| !readString (*data, offset, sources.geometry) || offset != data->size ()) {
	return std::nullopt;
    }

    return sources;
}

void ShaderCache::storeTranslation (const std::string& key, const GLSLContext::Sources& sources) {
    std::lock_guard lock (this->m_mutex);
    const auto entry = this->entry ("translations", key);

    if (!entry.has_value ()) {
	return;
    }

    std::string payload;
    payload.reserve (sources.vertex.size () + sources.fragment.size () + sources.geometry.size () + 12);
    appendString (payload, sources.vertex);
    appendString (payload, sources.fragment);
    appendString (payload, sources.geometry);

    this->write (*entry, payload);
}

bool ShaderCache::programsSupported () {
    std::lock_guard lock (this->m_mutex);

    if (!this->m_enabled) {
	return false;
    }

    if (!this->m_programsSupported.has_value ()) {
	GLint formats = 0;

	if (GLEW_VERSION_4_1 || GLEW_ARB_get_program_binary) {
	    glGetIntegerv (GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
	}

	this->m_programsSupported = formats > 0;

	// binaries only load into the driver that made them
	for (const GLenum name : { GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION }) {
	    const auto* value = reinterpret_cast<const char*> (glGetString (name));
	    this->m_driver += value != nullptr ? value : "";
	    this->m_driver += '\0';
	}
    }

    return *this->m_programsSupported;
}

GLuint ShaderCache::loadProgram (const std::string& key) {
    if (!this->programsSupported ()) {
	return 0;
    }

    std::lock_guard lock (this->m_mutex);
    const auto entry = this->entry ("programs", this->m_driver + key);

    if (!entry.has_value ()) {
	return 0;
    }

    const auto data = this->read (*entry);
    uint32_t format;
    size_t offset = 0;

    if (!data.has_value () || !readU32 (*data, offset, format) || offset == data->size ()) {
	return 0;
    }

    const GLuint program = glCreateProgram ();
    glProgramBinary (program, format, data->data () + offset, static_cast<GLsizei> (data->size () - offset));

    GLint linked = GL_FALSE;
    glGetProgramiv (program, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE) {
	// some driver updates break binaries without a version change
	glDeleteProgram (program);
	std::error_code error;
	std::filesystem::remove (entry->path, error);
	return 0;
    }

    return program;
}

void ShaderCache::storeProgram (const std::string& key, GLuint program) {
    if (!this->programsSupported ()) {
	return;
    }

    std::lock_guard lock (this->m_mutex);
    const auto entry = this->entry ("programs", this->m_driver + key);
    GLint length = 0;

    if (!entry.has_value ()) {
	return;
    }

    glGetProgramiv (program, GL_PROGRAM_BINARY_LENGTH, &length);

    if (length <= 0) {
	return;
    }

    std::string payload (4 + length, '\0');
    GLenum format = 0;
    GLsizei written = 0;
    glGetProgramBinary (program, length, &written, &format, payload.data () + 4);

    if (written <= 0) {
	return;
    }

    memcpy (payload.data (), &format, 4);
    payload.resize (4 + written);

    this->write (*entry, payload);
}
