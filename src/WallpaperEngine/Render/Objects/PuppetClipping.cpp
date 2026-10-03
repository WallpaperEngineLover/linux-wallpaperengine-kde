#include "PuppetClipping.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

using namespace WallpaperEngine::Render::Objects;

namespace {
class Reader {
public:
    Reader (const std::vector<char>& data, size_t offset) : m_data (data), m_offset (offset) { }

    template <typename T> T value () {
	if (m_offset + sizeof (T) > m_data.size ()) {
	    throw std::runtime_error ("puppet clipping data is cut short");
	}

	T result;
	std::memcpy (&result, m_data.data () + m_offset, sizeof (T));
	m_offset += sizeof (T);
	return result;
    }

    std::string string () {
	const auto* start = m_data.data () + m_offset;
	const auto* end = static_cast<const char*> (std::memchr (start, '\0', m_data.size () - m_offset));

	if (end == nullptr) {
	    throw std::runtime_error ("puppet clipping mask name is cut short");
	}

	m_offset += end - start + 1;
	return { start, end };
    }

    void skip (size_t bytes) {
	if (m_offset + bytes > m_data.size ()) {
	    throw std::runtime_error ("puppet clipping data is cut short");
	}

	m_offset += bytes;
    }

private:
    const std::vector<char>& m_data;
    size_t m_offset;
};

bool contains (const std::vector<uint32_t>& list, uint32_t value) {
    return std::find (list.begin (), list.end (), value) != list.end ();
}

// sub_140261880 after the records: a record whose sources are all targets of another one is nested in it, a loop
// in that chain drops the link
void linkParents (std::vector<PuppetClipping::Record>& records) {
    if (records.size () <= 1) {
	return;
    }

    for (size_t index = 0; index < records.size (); index++) {
	auto& record = records[index];

	if (record.sources.empty ()) {
	    continue;
	}

	for (size_t other = 0; other < records.size (); other++) {
	    if (other == index) {
		continue;
	    }

	    const auto found = std::ranges::count_if (record.sources, [&] (uint32_t source) {
		return contains (records[other].targets, source);
	    });

	    if (static_cast<size_t> (found) == record.sources.size ()) {
		record.parent = static_cast<int> (other);
		break;
	    }
	}

	std::set<int> seen;
	for (int parent = record.parent; parent != -1; parent = records[parent].parent) {
	    if (!seen.insert (parent).second) {
		record.parent = -1;
		break;
	    }
	}
    }
}
} // namespace

std::optional<PuppetClipping>
PuppetClipping::read (const std::vector<char>& data, size_t offset, int version, size_t indexCount) {
    if (version < 23) {
	return std::nullopt;
    }

    Reader reader (data, offset);
    PuppetClipping clipping;

    if (reader.value<uint8_t> () != 0) {
	reader.value<uint32_t> ();
	reader.skip (reader.value<uint32_t> ());
    }

    if (reader.value<uint8_t> () != 0) {
	const auto bytes = reader.value<uint32_t> ();

	for (uint32_t index = 0; index < bytes / 16; index++) {
	    reader.value<uint32_t> ();
	    reader.value<uint32_t> ();
	    const auto first = reader.value<uint32_t> ();
	    const auto count = reader.value<uint32_t> ();

	    if (first > indexCount || count > indexCount - first) {
		throw std::runtime_error ("puppet part range is past the index buffer");
	    }

	    clipping.parts.push_back ({ .firstIndex = first, .indexCount = count });
	}

	reader.skip (bytes % 16);
    }

    const auto count = reader.value<uint32_t> ();

    for (uint32_t index = 0; index < count; index++) {
	Record record;
	reader.value<uint64_t> ();
	record.mask = reader.string ();
	record.flags = reader.value<uint32_t> ();

	for (auto* list : { &record.targets, &record.sources }) {
	    const auto entries = reader.value<uint32_t> ();

	    for (uint32_t entry = 0; entry < entries; entry++) {
		const auto part = reader.value<uint32_t> ();

		// WE fails fast on these
		if (part >= clipping.parts.size ()) {
		    throw std::runtime_error ("puppet clipping record names a part that doesn't exist");
		}

		list->push_back (part);
	    }
	}

	clipping.records.push_back (std::move (record));
    }

    if (clipping.records.empty ()) {
	return std::nullopt;
    }

    linkParents (clipping.records);
    return clipping;
}

bool PuppetClipping::build (const std::vector<uint16_t>& meshIndices) {
    // the AtTargets path (part flag 4 in sub_14020B720) reads like it takes every later part as a target, left out
    if (std::ranges::any_of (this->records, [] (const Record& record) { return record.flags & AtTargets; })) {
	return false;
    }

    constexpr uint32_t Target = 0x1;
    constexpr uint32_t Source = 0x8;

    const auto partCount = static_cast<int> (this->parts.size ());
    std::vector<uint32_t> partFlags (this->parts.size (), 0);

    for (const auto& record : this->records) {
	for (const auto part : record.targets) {
	    partFlags[part] |= Target;
	}

	for (const auto part : record.sources) {
	    partFlags[part] |= Source;
	}
    }

    struct Entry {
	uint32_t record = 0;
	int firstTarget = INT_MAX;
	int firstSource = -1;
	int lastSource = -1;
	std::vector<uint32_t> maskParts;
	std::vector<uint32_t> targets;
    };

    std::vector<Entry> entries (this->records.size ());
    std::set<uint32_t> hiddenSources;

    for (size_t index = 0; index < this->records.size (); index++) {
	const auto& record = this->records[index];
	auto& entry = entries[index];
	entry.record = static_cast<uint32_t> (index);

	for (int part = 0; part < partCount; part++) {
	    if ((partFlags[part] & Target) && contains (record.targets, part)) {
		entry.targets.push_back (part);
		entry.firstTarget = std::min (entry.firstTarget, part);
	    }

	    if (contains (record.sources, part)) {
		if (entry.firstSource < 0) {
		    entry.firstSource = part;
		}

		entry.lastSource = part;

		if (record.flags & HideSources) {
		    hiddenSources.insert (part);
		}
	    }
	}

	// everything drawn between the first and the last source makes the mask, apart from the record's own targets
	for (int part = 0; part < partCount; part++) {
	    if (entry.firstSource <= part && entry.lastSource >= part && !contains (entry.targets, part)) {
		entry.maskParts.push_back (part);
	    }
	}
    }

    // a nested mask can only be drawn once its ancestors' sources are
    for (size_t index = 0; index < entries.size (); index++) {
	for (int parent = this->records[index].parent; parent != -1; parent = this->records[parent].parent) {
	    entries[index].lastSource = std::max (entries[index].lastSource, entries[parent].lastSource);
	}
    }

    std::ranges::stable_sort (entries, [] (const Entry& a, const Entry& b) { return a.firstTarget < b.firstTarget; });

    this->indices.clear ();
    this->draws.clear ();
    this->commands.clear ();

    const auto addDraw = [this, &meshIndices] (const std::vector<uint32_t>& list) {
	Draw draw { .offset = static_cast<uint32_t> (this->indices.size ()), .count = 0 };

	for (const auto part : list) {
	    const auto& range = this->parts[part];
	    this->indices.insert (
		this->indices.end (), meshIndices.begin () + range.firstIndex,
		meshIndices.begin () + range.firstIndex + range.indexCount
	    );
	    draw.count += range.indexCount;
	}

	this->draws.push_back (draw);
	return static_cast<int> (this->draws.size () - 1);
    };

    std::vector<uint32_t> pending;
    std::map<uint32_t, int> maskDraws;

    const auto flush = [&] () {
	if (pending.empty ()) {
	    return;
	}

	addDraw (pending);
	this->commands.push_back (PlainDraw);
	pending.clear ();
    };

    for (int part = 0; part < partCount; part++) {
	const uint32_t flags = partFlags[part];

	if (!(flags & Target)) {
	    if (!hiddenSources.contains (part)) {
		pending.push_back (part);
	    }
	} else if (!(flags & Source)) {
	    // drawn with its record
	    continue;
	}

	if (!(flags & Source)) {
	    continue;
	}

	for (const auto& entry : entries) {
	    if (entry.lastSource != part) {
		continue;
	    }

	    flush ();

	    std::vector<int> chain;
	    for (int parent = this->records[entry.record].parent; parent != -1; parent = this->records[parent].parent) {
		chain.push_back (parent);
	    }

	    maskDraws[entry.record] = addDraw (entry.maskParts);
	    addDraw (entry.targets);

	    if (chain.empty ()) {
		this->commands.push_back (Mask);
	    } else {
		this->commands.push_back (NestedMask);
		this->commands.push_back (static_cast<int> (chain.size ()));

		for (const auto parent : chain) {
		    this->commands.push_back (parent);
		    this->commands.push_back (maskDraws[parent]);
		}
	    }

	    this->commands.push_back (static_cast<int> (entry.record));
	}
    }

    flush ();
    return true;
}
