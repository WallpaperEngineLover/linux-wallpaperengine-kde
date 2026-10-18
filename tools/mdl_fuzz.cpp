// Mutates every .mdl of the given .pkg files (truncation, random bytes, extreme 32 bit fields, copied chunks) and
// pushes each mutant through the puppet readers (readPuppetMeshFile, readMdlvMeshes, PuppetClipping, PuppetRig::load
// and a few pose updates). It is only worth running against an engine library built with AddressSanitizer:
//
//   cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-sanitize=vptr" && cmake --build build-asan
//   g++ -O1 -g -fsanitize=address -fno-sanitize=vptr -std=gnu++20 <the defines and -I flags of any engine source in
//       build-asan/compile_commands.json> tools/mdl_fuzz.cpp -o mdl_fuzz -Lbuild-asan/output
//       -llinux-wallpaperengine-lib -Wl,-rpath,$PWD/build-asan/output
//   ASAN_OPTIONS=allocator_may_return_null=1:max_allocation_size_mb=1024:detect_leaks=0:hard_rss_limit_mb=3500 \
//       ./mdl_fuzz <seed> <mutants per file> ~/.local/share/Steam/steamapps/workshop/content/431960/*/*.pkg
//
// usage: mdl_fuzz <seed> <mutants per file> <pkg>...   (mutant 0 of every file is the original)
// A crash is attributed by the last "MUT <pkg> <file> seed= index= kind=" line on stderr, a mutant that runs for more
// than 20 s prints HANG and that line and exits 3. Replay one: MDLFUZZ_ONLY=<name substring> MDLFUZZ_INDEX=<n> with the
// same seed and mutant count. 417 installed models, 4 seeds, about 230k mutants: no reports since the 2026-10-17 fixes.
#include "WallpaperEngine/Data/Parsers/PackageParser.h"
#include "WallpaperEngine/FileSystem/Adapters/Package.h"
#include "WallpaperEngine/Render/Objects/PuppetClipping.h"
#include "WallpaperEngine/Render/Objects/PuppetMeshFile.h"
#include "WallpaperEngine/Render/Objects/PuppetRig.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <unistd.h>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;

static char g_current[512];

static void onAlarm (int) {
    const char msg[] = "\nHANG: ";
    write (2, msg, sizeof (msg) - 1);
    write (2, g_current, strlen (g_current));
    write (2, "\n", 1);
    _exit (3);
}

static std::vector<char> mutate (const std::vector<char>& original, std::mt19937_64& rng, int kind) {
    std::vector<char> data = original;

    if (data.empty ()) {
	return data;
    }

    static const uint32_t interesting[]
	= { 0u,          1u,          2u,          0xFFu,       0xFFFFu,     0x10000u,    0x7FFFFFFFu,
	    0x80000000u, 0xFFFFFFFFu, 0x00FFFFFFu, 0x01000000u, 0x3FFFFFFFu, 0xFFFFFFFEu, 0x00020000u };

    switch (kind) {
	case 0:
	    data.resize (rng () % data.size ());
	    break;
	case 1:
	    {
		const int count = 1 + rng () % 8;
		for (int i = 0; i < count; i++) {
		    data[rng () % data.size ()] = static_cast<char> (rng ());
		}
		break;
	    }
	case 2:
	    {
		const int count = 1 + rng () % 3;
		for (int i = 0; i < count && data.size () >= 4; i++) {
		    const size_t at = (rng () % (data.size () / 4)) * 4;
		    const uint32_t value = interesting[rng () % (sizeof (interesting) / sizeof (interesting[0]))];
		    std::memcpy (data.data () + at, &value, 4);
		}
		break;
	    }
	case 3:
	    {
		const size_t at = rng () % (data.size () > 4 ? data.size () - 4 : 1);
		const uint32_t value = interesting[rng () % (sizeof (interesting) / sizeof (interesting[0]))];
		if (data.size () >= at + 4) {
		    std::memcpy (data.data () + at, &value, 4);
		}
		break;
	    }
	default:
	    {
		const size_t length = 1 + rng () % std::min<size_t> (256, data.size ());
		const size_t from = rng () % (data.size () - length + 1);
		const size_t to = rng () % (data.size () - length + 1);
		std::memmove (data.data () + to, data.data () + from, length);
		break;
	    }
    }

    return data;
}

static void parseOne (const std::vector<char>& data, const std::string& name) {
    const auto mesh = readPuppetMeshFile (data);

    try {
	std::ignore = readMdlvMeshes (data);
    } catch (const std::exception&) { }

    if (!mesh.has_value ()) {
	return;
    }

    const int flags = static_cast<int> (mesh->flags);

    try {
	if (flags & 8) {
	    std::ignore = PuppetClipping::readParts (data, mesh->indexEnd, mesh->version, mesh->indices.size ());
	}
    } catch (const std::exception&) { }

    try {
	auto clipping = PuppetClipping::read (data, mesh->indexEnd, mesh->version, mesh->indices.size ());

	if (clipping.has_value ()) {
	    clipping->build (mesh->indices);
	}
    } catch (const std::exception&) { }

    if (mesh->skeleton < data.size () && !mesh->blendIndices.empty ()) {
	try {
	    PuppetRig rig;
	    rig.load (data, mesh->sections, mesh->meshCount, name);
	    rig.addSceneLayers ({});

	    for (int i = 0; i < 4; i++) {
		rig.updatePose (glm::mat4 (1.0f));
	    }
	} catch (const std::exception&) { }
    }
}

int main (int argc, char** argv) {
    if (argc < 4) {
	std::cerr << "usage: mdlfuzz <seed> <mutants per file> <pkg>..." << std::endl;
	return 2;
    }

    const uint64_t seed = std::strtoull (argv[1], nullptr, 10);
    const int mutants = std::atoi (argv[2]);
    const char* only = std::getenv ("MDLFUZZ_ONLY");
    const char* onlyIndex = std::getenv ("MDLFUZZ_INDEX");
    std::signal (SIGALRM, onAlarm);

    size_t files = 0, runs = 0;

    for (int i = 3; i < argc; i++) {
	auto stream = std::make_shared<std::ifstream> (argv[i], std::ios::binary);

	try {
	    FileSystem::Adapters::PackageAdapter adapter (Data::Parsers::PackageParser::parse (stream));

	    for (const auto& entry : adapter.package->files) {
		if (!entry->filename.ends_with (".mdl")
		    || (only != nullptr && entry->filename.find (only) == std::string::npos)) {
		    continue;
		}

		const auto handle = adapter.open (entry->filename);
		const std::vector<char> original (
		    (std::istreambuf_iterator<char> (*handle)), std::istreambuf_iterator<char> ()
		);
		files++;

		for (int m = 0; m < mutants; m++) {
		    if (onlyIndex != nullptr && m != std::atoi (onlyIndex)) {
			continue;
		    }

		    std::mt19937_64 rng (seed * 1000003 + files * 7919 + static_cast<uint64_t> (m));
		    const auto data = m == 0 && onlyIndex == nullptr ? original : mutate (original, rng, m % 5);
		    std::snprintf (
			g_current, sizeof (g_current), "%s %s seed=%llu index=%d kind=%d bytes=%zu", argv[i],
			entry->filename.c_str (), static_cast<unsigned long long> (seed), m, m % 5, data.size ()
		    );
		    std::fprintf (stderr, "MUT %s\n", g_current);
		    alarm (20);
		    parseOne (data, entry->filename);
		    alarm (0);
		    runs++;
		}
	    }
	} catch (const std::exception& e) {
	    std::cerr << argv[i] << " : package : " << e.what () << std::endl;
	}
    }

    std::cout << "mdlfuzz: " << files << " mdl files, " << runs << " runs, no crash" << std::endl;
    return 0;
}
