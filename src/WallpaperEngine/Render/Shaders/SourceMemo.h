#pragma once

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace WallpaperEngine::Render::Shaders {
/**
 * 128 bit digest of a list of sources, so memos don't keep a copy of what they are keyed by; two unrelated 64 bit
 * hashes and the length make a collision between the sources of one run a non-issue
 */
inline std::string sourceDigest (const std::initializer_list<std::string_view> parts) {
    uint64_t fnv = 0xcbf29ce484222325ull;
    uint64_t mix = 0x9e3779b97f4a7c15ull;
    uint64_t length = 0;

    for (const auto part : parts) {
	for (const char c : part) {
	    fnv = (fnv ^ static_cast<unsigned char> (c)) * 0x100000001b3ull;
	    mix = (mix ^ static_cast<unsigned char> (c)) * 0x94d049bb133111ebull;
	    mix ^= mix >> 29;
	}

	// part boundaries count: ("ab", "c") is not ("a", "bc")
	fnv = (fnv ^ 0x1f) * 0x100000001b3ull;
	mix = (mix ^ 0x1f) * 0x94d049bb133111ebull;
	length += part.size () + 1;
    }

    char out[49];
    snprintf (
	out, sizeof (out), "%016llx%016llx%016llx", static_cast<unsigned long long> (fnv),
	static_cast<unsigned long long> (mix), static_cast<unsigned long long> (length)
    );
    return out;
}

/** Digest keyed memo of strings that is emptied once the values it holds pass a byte budget */
template <typename Value> class SourceMemo {
public:
    explicit SourceMemo (const size_t budget) : m_budget (budget) { }

    [[nodiscard]] std::optional<Value> find (const std::string& key) {
	const std::lock_guard lock (m_mutex);

	if (const auto it = m_entries.find (key); it != m_entries.end ()) {
	    return it->second;
	}

	return std::nullopt;
    }

    void insert (std::string key, Value value, const size_t bytes) {
	const std::lock_guard lock (m_mutex);

	if (m_bytes + bytes > m_budget) {
	    m_entries.clear ();
	    m_bytes = 0;
	}

	if (m_entries.emplace (std::move (key), std::move (value)).second) {
	    m_bytes += bytes;
	}
    }

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, Value> m_entries;
    size_t m_bytes = 0;
    size_t m_budget;
};
}
