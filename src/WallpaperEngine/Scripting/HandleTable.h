#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

namespace WallpaperEngine::Scripting {
/**
 * Hands out values a v8::External can carry in place of a raw pointer to a native record a script may keep using after
 * the record is gone. A released handle (and one of an earlier use of its slot) finds nothing, so the record can be
 * freed while stale JS objects still call into it. The value stays below 2^47, which every pointer-sized External
 * accepts, and is never 0.
 */
template <typename T> class HandleTable {
public:
    [[nodiscard]] void* insert (T* object) {
	uint32_t index;

	if (m_free.size () > REUSE_AFTER) {
	    index = m_free.front ();
	    m_free.pop_front ();
	} else {
	    if (m_slots.size () >= INDEX_MASK) {
		throw std::length_error ("too many live script handles");
	    }

	    index = static_cast<uint32_t> (m_slots.size ());
	    m_slots.emplace_back ();
	}

	m_slots[index].object = object;

	return encode (index, m_slots[index].generation);
    }

    [[nodiscard]] T* find (void* handle) const {
	const auto slot = decode (handle);

	return slot == nullptr ? nullptr : slot->object;
    }

    void release (void* handle) {
	const auto slot = decode (handle);

	if (slot == nullptr) {
	    return;
	}

	slot->object = nullptr;
	slot->generation = slot->generation >= GENERATION_MASK ? 1 : slot->generation + 1;
	m_free.push_back (static_cast<uint32_t> (slot - m_slots.data ()));
    }

    [[nodiscard]] size_t live () const { return m_slots.size () - m_free.size (); }

private:
    static constexpr uint64_t INDEX_MASK = (1u << 24) - 1;
    static constexpr uint64_t GENERATION_MASK = (1u << 23) - 1;
    // a slot waits behind this many others before it is used again, so a stale handle stays stale for long
    static constexpr size_t REUSE_AFTER = 64;

    struct Slot {
	T* object = nullptr;
	uint32_t generation = 1;
    };

    static void* encode (uint32_t index, uint32_t generation) {
	return reinterpret_cast<void*> (
	    static_cast<uintptr_t> ((static_cast<uint64_t> (generation) << 24) | (index + 1u))
	);
    }

    Slot* decode (void* handle) const {
	const auto value = static_cast<uint64_t> (reinterpret_cast<uintptr_t> (handle));
	const uint64_t index = value & INDEX_MASK;

	if (index == 0 || index > m_slots.size ()) {
	    return nullptr;
	}

	auto* slot = const_cast<Slot*> (&m_slots[index - 1]);

	return slot->object != nullptr && slot->generation == (value >> 24) ? slot : nullptr;
    }

    std::vector<Slot> m_slots;
    std::deque<uint32_t> m_free;
};
}
