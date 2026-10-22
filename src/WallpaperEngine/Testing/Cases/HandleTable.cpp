#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

#include "WallpaperEngine/Scripting/HandleTable.h"

using WallpaperEngine::Scripting::HandleTable;

TEST_CASE ("A handle finds its record until the record is released") {
    HandleTable<int> table;
    int first = 1;
    int second = 2;

    void* a = table.insert (&first);
    void* b = table.insert (&second);

    CHECK (a != nullptr);
    CHECK (a != b);
    CHECK (table.find (a) == &first);
    CHECK (table.find (b) == &second);
    CHECK (table.live () == 2);

    table.release (a);

    CHECK (table.find (a) == nullptr);
    CHECK (table.find (b) == &second);
    CHECK (table.live () == 1);

    table.release (a);
    CHECK (table.live () == 1);
}

TEST_CASE ("Nothing but an issued handle finds a record") {
    HandleTable<int> table;
    int value = 1;
    void* handle = table.insert (&value);

    CHECK (table.find (nullptr) == nullptr);
    CHECK (table.find (&value) == nullptr);
    CHECK (table.find (reinterpret_cast<void*> (static_cast<uintptr_t> (0xFFFFFFFFFFFFull))) == nullptr);
    CHECK (table.find (reinterpret_cast<void*> (reinterpret_cast<uintptr_t> (handle) + (1ull << 24))) == nullptr);
    CHECK (table.find (reinterpret_cast<void*> (reinterpret_cast<uintptr_t> (handle) + 1)) == nullptr);
}

TEST_CASE ("A released slot is used again later and the old handle stays stale") {
    HandleTable<int> table;
    std::vector<int> values (500);
    std::vector<void*> handles;

    handles.reserve (values.size ());
    for (auto& value : values) {
	handles.push_back (table.insert (&value));
    }

    void* stale = handles[0];
    table.release (stale);

    int replacement = 7;
    std::set<uintptr_t> seen;

    for (int i = 0; i < 2000; i++) {
	void* handle = table.insert (&replacement);
	CHECK (table.find (stale) == nullptr);
	CHECK (table.find (handle) == &replacement);
	seen.insert (reinterpret_cast<uintptr_t> (handle) & 0xFFFFFF);
	table.release (handle);
    }

    CHECK (seen.size () > 1);
    CHECK (table.find (handles[1]) == &values[1]);
}

TEST_CASE ("Handles stay below 2^47 and nonzero however often a slot is reused") {
    HandleTable<int> table;
    int value = 1;

    for (int i = 0; i < 300000; i++) {
	void* handle = table.insert (&value);
	REQUIRE (reinterpret_cast<uintptr_t> (handle) != 0);
	REQUIRE (reinterpret_cast<uintptr_t> (handle) < (1ull << 47));
	REQUIRE (table.find (handle) == &value);
	table.release (handle);
    }

    CHECK (table.live () == 0);
}
