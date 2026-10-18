#include "LocalStorage.h"

#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sys/file.h>
#include <unistd.h>

using namespace WallpaperEngine::Scripting;

namespace {
constexpr char FILE_MAGIC[] = "LSBK0001";
constexpr size_t KEY_LENGTH = 40;

std::mutex g_registryMutex;
std::map<std::string, std::weak_ptr<LocalStorage::Store>> g_registry;
std::optional<std::filesystem::path> g_directoryOverride;

uint32_t rotl (const uint32_t value, const int bits) { return (value << bits) | (value >> (32 - bits)); }

void sha1Block (std::array<uint32_t, 5>& state, const uint8_t* block) {
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
	w[i] = (uint32_t (block[i * 4]) << 24) | (uint32_t (block[i * 4 + 1]) << 16)
	    | (uint32_t (block[i * 4 + 2]) << 8) | uint32_t (block[i * 4 + 3]);
    }

    for (int i = 16; i < 80; i++) {
	w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

    for (int i = 0; i < 80; i++) {
	uint32_t f, k;

	if (i < 20) {
	    f = (b & c) | (~b & d);
	    k = 0x5A827999;
	} else if (i < 40) {
	    f = b ^ c ^ d;
	    k = 0x6ED9EBA1;
	} else if (i < 60) {
	    f = (b & c) | (b & d) | (c & d);
	    k = 0x8F1BBCDC;
	} else {
	    f = b ^ c ^ d;
	    k = 0xCA62C1D6;
	}

	const uint32_t temp = rotl (a, 5) + f + e + k + w[i];
	e = d;
	d = c;
	c = rotl (b, 30);
	b = a;
	a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void appendU32 (std::string& out, const uint32_t value) {
    for (int i = 0; i < 4; i++) {
	out.push_back (static_cast<char> ((value >> (8 * i)) & 0xFF));
    }
}

bool readU32 (std::istream& in, uint32_t& value) {
    uint8_t bytes[4];

    if (!in.read (reinterpret_cast<char*> (bytes), 4)) {
	return false;
    }

    value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (uint32_t (bytes[3]) << 24);
    return true;
}

size_t storedBytes (const std::vector<std::pair<std::string, std::string>>& entries, const std::string& except) {
    size_t total = 0;

    for (const auto& [key, value] : entries) {
	if (key != except) {
	    total += value.size ();
	}
    }

    return total;
}

/** Locks the directory since store files are replaced by rename */
class DirectoryLock {
public:
    explicit DirectoryLock (const std::filesystem::path& directory) {
	this->m_fd = ::open ((directory / ".lock").c_str (), O_RDWR | O_CREAT | O_CLOEXEC, 0600);

	if (this->m_fd >= 0) {
	    flock (this->m_fd, LOCK_EX);
	}
    }

    ~DirectoryLock () {
	if (this->m_fd >= 0) {
	    flock (this->m_fd, LOCK_UN);
	    close (this->m_fd);
	}
    }

    DirectoryLock (const DirectoryLock&) = delete;
    DirectoryLock& operator= (const DirectoryLock&) = delete;

private:
    int m_fd = -1;
};
}

std::string LocalStorage::sha1Hex (const std::string_view data) {
    std::array<uint32_t, 5> state = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    const auto* bytes = reinterpret_cast<const uint8_t*> (data.data ());
    size_t offset = 0;

    for (; offset + 64 <= data.size (); offset += 64) {
	sha1Block (state, bytes + offset);
    }

    uint8_t tail[128] = {};
    const size_t remaining = data.size () - offset;
    memcpy (tail, bytes + offset, remaining);
    tail[remaining] = 0x80;

    const size_t tailLength = remaining + 9 > 64 ? 128 : 64;
    const uint64_t bits = static_cast<uint64_t> (data.size ()) * 8;

    for (int i = 0; i < 8; i++) {
	tail[tailLength - 1 - i] = static_cast<uint8_t> (bits >> (8 * i));
    }

    sha1Block (state, tail);

    if (tailLength == 128) {
	sha1Block (state, tail + 64);
    }

    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve (KEY_LENGTH);

    for (const uint32_t word : state) {
	for (int shift = 28; shift >= 0; shift -= 4) {
	    hex.push_back (digits[(word >> shift) & 0xF]);
	}
    }

    return hex;
}

std::filesystem::path LocalStorage::directory () {
    if (g_directoryOverride.has_value ()) {
	return *g_directoryOverride;
    }

    if (const char* state = std::getenv ("XDG_STATE_HOME"); state != nullptr && state[0] == '/') {
	return std::filesystem::path (state) / "linux-wallpaperengine" / "scenestorage";
    }

    if (const char* home = std::getenv ("HOME"); home != nullptr && home[0] != '\0') {
	return std::filesystem::path (home) / ".local" / "state" / "linux-wallpaperengine" / "scenestorage";
    }

    return {};
}

void LocalStorage::setDirectory (std::filesystem::path directory) { g_directoryOverride = std::move (directory); }

std::vector<std::pair<std::string, std::string>> LocalStorage::readFile (const std::filesystem::path& file) {
    std::vector<std::pair<std::string, std::string>> entries;
    std::ifstream in (file, std::ios::binary);
    std::error_code error;

    // sub_140185960: header and count only = empty
    if (!in || std::filesystem::file_size (file, error) <= 12 || error) {
	return entries;
    }

    char magic[8];
    uint32_t count = 0;

    if (!in.read (magic, 8) || memcmp (magic, FILE_MAGIC, 8) != 0 || !readU32 (in, count)) {
	return entries;
    }

    for (uint32_t i = 0; i < count; i++) {
	char key[KEY_LENGTH + 1] = {};
	uint32_t size = 0;

	if (!in.read (key, KEY_LENGTH) || !readU32 (in, size) || size > MAX_STORE_BYTES) {
	    break;
	}

	std::string value (size, '\0');

	if (!in.read (value.data (), size)) {
	    break;
	}

	std::string name (key);
	const auto existing
	    = std::ranges::find_if (entries, [&name] (const auto& entry) { return entry.first == name; });

	if (existing != entries.end ()) {
	    existing->second = std::move (value);
	} else {
	    entries.emplace_back (std::move (name), std::move (value));
	}
    }

    return entries;
}

bool LocalStorage::writeFile (
    const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& entries
) {
    std::string data (FILE_MAGIC, 8);
    appendU32 (data, static_cast<uint32_t> (entries.size ()));

    // sub_1401E2BC0
    for (const auto& [key, value] : entries) {
	std::string paddedKey = key.substr (0, KEY_LENGTH);
	paddedKey.resize (KEY_LENGTH, '\0');
	data += paddedKey;
	appendU32 (data, static_cast<uint32_t> (value.size ()));
	data += value;
    }

    const auto temporary = file.string () + ".tmp" + std::to_string (getpid ());

    {
	std::ofstream out (temporary, std::ios::binary | std::ios::trunc);

	if (!out.write (data.data (), static_cast<std::streamsize> (data.size ())) || !out.flush ()) {
	    std::error_code ignored;
	    std::filesystem::remove (temporary, ignored);
	    return false;
	}
    }

    std::error_code error;
    std::filesystem::rename (temporary, file, error);

    if (error) {
	std::filesystem::remove (temporary, error);
	return false;
    }

    return true;
}

LocalStorage::Store::Store (std::string name) : m_name (std::move (name)) {
    if (const auto dir = directory (); !dir.empty ()) {
	this->m_entries = readFile (dir / (this->m_name + ".bin"));
    }
}

LocalStorage::Store::~Store () { this->flush (); }

std::vector<std::pair<std::string, std::string>>::iterator LocalStorage::Store::find (const std::string& key) {
    return std::ranges::find_if (this->m_entries, [&key] (const auto& entry) { return entry.first == key; });
}

std::optional<std::string> LocalStorage::Store::get (const std::string& key) const {
    const auto entry = std::ranges::find_if (this->m_entries, [&key] (const auto& e) { return e.first == key; });

    if (entry == this->m_entries.end ()) {
	return std::nullopt;
    }

    return entry->second;
}

bool LocalStorage::Store::set (const std::string& key, std::string value) {
    // sub_14018E3A0
    if (storedBytes (this->m_entries, key) + value.size () > MAX_STORE_BYTES) {
	return false;
    }

    this->m_changes[key] = value;

    if (const auto entry = this->find (key); entry != this->m_entries.end ()) {
	entry->second = std::move (value);
    } else {
	this->m_entries.emplace_back (key, std::move (value));
    }

    return true;
}

bool LocalStorage::Store::remove (const std::string& key) {
    const auto entry = this->find (key);

    if (entry == this->m_entries.end ()) {
	return false;
    }

    this->m_entries.erase (entry);
    this->m_changes[key] = std::nullopt;
    return true;
}

void LocalStorage::Store::clear () {
    this->m_entries.clear ();
    this->m_changes.clear ();
    this->m_cleared = true;
}

void LocalStorage::Store::flush () {
    if (!this->m_cleared && this->m_changes.empty ()) {
	return;
    }

    const auto dir = directory ();

    if (dir.empty ()) {
	return;
    }

    std::error_code error;
    std::filesystem::create_directories (dir, error);

    if (error) {
	sLog.error ("localStorage: cannot create ", dir.string (), ": ", error.message ());
	return;
    }

    const DirectoryLock lock (dir);
    const auto file = dir / (this->m_name + ".bin");
    auto entries = this->m_cleared ? std::vector<std::pair<std::string, std::string>> {} : readFile (file);

    for (const auto& [key, value] : this->m_changes) {
	const auto entry = std::ranges::find_if (entries, [&key] (const auto& e) { return e.first == key; });

	if (!value.has_value ()) {
	    if (entry != entries.end ()) {
		entries.erase (entry);
	    }
	} else if (entry != entries.end ()) {
	    entry->second = *value;
	} else {
	    entries.emplace_back (key, *value);
	}
    }

    if (!writeFile (file, entries)) {
	sLog.error ("localStorage: cannot write ", file.string ());
	return;
    }

    this->m_entries = std::move (entries);
    this->m_changes.clear ();
    this->m_cleared = false;
}

std::shared_ptr<LocalStorage::Store> LocalStorage::open (const std::string& name) {
    const std::lock_guard lock (g_registryMutex);

    if (const auto existing = g_registry.find (name); existing != g_registry.end ()) {
	if (auto store = existing->second.lock ()) {
	    return store;
	}
    }

    auto store = std::make_shared<Store> (name);
    g_registry[name] = store;
    return store;
}

void LocalStorage::flushAll () {
    std::vector<std::shared_ptr<Store>> stores;

    {
	const std::lock_guard lock (g_registryMutex);

	for (auto it = g_registry.begin (); it != g_registry.end ();) {
	    if (auto store = it->second.lock ()) {
		stores.push_back (std::move (store));
		++it;
	    } else {
		it = g_registry.erase (it);
	    }
	}
    }

    for (const auto& store : stores) {
	store->flush ();
    }
}
