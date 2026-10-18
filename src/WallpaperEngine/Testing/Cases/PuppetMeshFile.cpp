#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "WallpaperEngine/Render/Objects/PuppetClipping.h"
#include "WallpaperEngine/Render/Objects/PuppetMeshFile.h"
#include "WallpaperEngine/Render/Objects/PuppetRig.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
// position, blend indices, blend weights, uv: 52 bytes
constexpr uint32_t SKINNED = 0x1 | 0x800000 | 0x1000000 | 0x8;

struct Builder {
    std::vector<char> bytes;

    void u8 (uint8_t value) { bytes.push_back (static_cast<char> (value)); }

    void u32 (uint32_t value) {
	for (int shift = 0; shift < 32; shift += 8) {
	    bytes.push_back (static_cast<char> (value >> shift));
	}
    }

    void f32 (float value) { u32 (std::bit_cast<uint32_t> (value)); }

    void text (const std::string& value) {
	bytes.insert (bytes.end (), value.begin (), value.end ());
	bytes.push_back ('\0');
    }

    [[nodiscard]] size_t size () const { return bytes.size (); }
};

struct Options {
    int version = 23;
    uint32_t format = SKINNED;
    uint32_t flags = 0;
    std::string material = "materials/puppet.json";
    /** padding before the UVs, a stride the format doesn't describe */
    size_t padding = 0;
    std::vector<uint16_t> indices = { 0, 1, 2 };
    /** v21 auxiliary block and its declared size */
    std::optional<uint32_t> auxBytes;
    /** v21 part ranges as (bone, order, first, count) */
    std::vector<std::array<uint32_t, 4>> parts;
    /** v23 clipping records: targets, then sources */
    std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> records;
    bool skeleton = false;
    bool attachmentsFirst = false;
};

struct Fixture {
    std::vector<char> bytes;
    size_t vertices = 0;
    size_t indexEnd = 0;
    size_t skeleton = 0;
    size_t sections = 0;
};

uint32_t strideOf (uint32_t format) {
    uint32_t stride = 0;
    stride += format & 0x1 ? 12 : 0;
    stride += format & 0x2 ? 12 : 0;
    stride += format & 0x800000 ? 16 : 0;
    stride += format & 0x1000000 ? 16 : 0;
    stride += format & 0x8 ? 8 : 0;
    stride += format & 0x8000 ? 16 : 0;
    return stride;
}

// one three-vertex mesh as WE's resource compiler writes it
Fixture puppet (const Options& options = {}) {
    Builder out;
    Fixture fixture;
    const std::string tag
	= "MDLV" + std::string (options.version < 10 ? "000" : "00") + std::to_string (options.version);

    out.text (tag);
    out.u32 (options.format);
    out.u32 (1);
    out.u32 (1);
    out.text (options.material);
    out.u32 (options.flags);

    if (options.flags & 2) {
	out.u32 (1);
    }

    if (options.version >= 17) {
	for (int i = 0; i < 6; i++) {
	    out.f32 (static_cast<float> (i));
	}
    }

    if (options.version >= 15) {
	out.u32 (options.format);
    }

    const uint32_t stride = strideOf (options.format) + static_cast<uint32_t> (options.padding);
    out.u32 (stride * 3);
    fixture.vertices = out.size ();

    for (int vertex = 0; vertex < 3; vertex++) {
	out.f32 (vertex == 1 ? 100.0f : 0.0f);
	out.f32 (vertex == 2 ? 100.0f : 0.0f);
	out.f32 (0.0f);

	if (options.format & 0x2) {
	    out.f32 (0.0f);
	    out.f32 (0.0f);
	    out.f32 (1.0f);
	}

	if (options.format & 0x800000) {
	    out.u32 (0);
	    out.u32 (1);
	    out.u32 (0);
	    out.u32 (0);
	}

	if (options.format & 0x1000000) {
	    out.f32 (0.25f);
	    out.f32 (0.75f);
	    out.f32 (0.0f);
	    out.f32 (0.0f);
	}

	for (size_t i = 0; i < options.padding; i++) {
	    out.u8 (0);
	}

	if (options.format & 0x8) {
	    out.f32 (vertex == 1 ? 1.0f : 0.0f);
	    out.f32 (vertex == 2 ? 1.0f : 0.0f);
	}

	if (options.format & 0x8000) {
	    out.f32 (1.0f);
	    out.f32 (1.0f);
	    out.f32 (1.0f);
	    out.f32 (1.0f);
	}
    }

    out.u32 (static_cast<uint32_t> (options.indices.size () * sizeof (uint16_t)));

    for (const uint16_t index : options.indices) {
	out.u8 (index & 0xFF);
	out.u8 (index >> 8);
    }

    fixture.indexEnd = out.size ();

    if (options.version >= 21) {
	out.u8 (options.auxBytes.has_value () ? 1 : 0);

	if (options.auxBytes.has_value ()) {
	    out.u32 (0);
	    out.u32 (*options.auxBytes);

	    for (uint32_t i = 0; i < *options.auxBytes / 4; i++) {
		out.f32 (static_cast<float> (i));
	    }
	}

	out.u8 (options.parts.empty () ? 0 : 1);

	if (!options.parts.empty ()) {
	    out.u32 (static_cast<uint32_t> (options.parts.size () * 16));

	    for (const auto& part : options.parts) {
		for (const uint32_t value : part) {
		    out.u32 (value);
		}
	    }
	}
    }

    if (options.version >= 23) {
	out.u32 (static_cast<uint32_t> (options.records.size ()));

	for (const auto& [targets, sources] : options.records) {
	    out.u32 (0);
	    out.u32 (0);
	    out.text ("masks/clip");
	    out.u32 (0);

	    for (const auto* list : { &targets, &sources }) {
		out.u32 (static_cast<uint32_t> (list->size ()));

		for (const uint32_t part : *list) {
		    out.u32 (part);
		}
	    }
	}
    }

    fixture.sections = out.size ();

    if (options.attachmentsFirst) {
	out.text ("MDAT0001");
	out.u32 (static_cast<uint32_t> (out.size () + sizeof (uint32_t) * 2));
	out.u32 (0);
    }

    fixture.skeleton = out.size ();

    if (options.skeleton) {
	// one root bone: no name, parent -1, identity bind matrix, no physics
	out.text ("MDLS0001");
	out.u32 (0);
	out.u32 (1);
	out.u32 (0);
	out.u32 (0xFFFFFFFF);
	out.u32 (64);

	for (int i = 0; i < 16; i++) {
	    out.f32 (i % 5 == 0 ? 1.0f : 0.0f);
	}

	out.text ("");
    } else {
	out.u8 (0);
    }

    fixture.bytes = std::move (out.bytes);
    return fixture;
}
} // namespace

TEST_CASE ("The first puppet mesh is read from its header") {
    for (const int version : { 13, 15, 17, 21, 23 }) {
	INFO ("MDLV" << version);

	const auto fixture = puppet ({ .version = version });
	const auto mesh = readPuppetMeshFile (fixture.bytes);

	REQUIRE (mesh.has_value ());
	CHECK (mesh->version == version);
	CHECK (mesh->layout.vertexStride == 52);
	CHECK (mesh->layout.block.headerOffset + 8 == fixture.vertices);
	CHECK (mesh->indexEnd == fixture.indexEnd);
	REQUIRE (mesh->positions.size () == 9);
	CHECK (mesh->positions[3] == 100.0f);
	CHECK (mesh->positions[7] == 100.0f);
	CHECK (mesh->texcoords[2] == 1.0f);
	CHECK (mesh->indices == std::vector<uint16_t> { 0, 1, 2 });
	REQUIRE (mesh->blendIndices.size () == 3);
	CHECK (mesh->blendIndices[2].y == 1);
	CHECK (mesh->blendWeights[2].y == 0.75f);
	CHECK (mesh->warnings.empty ());
    }
}

TEST_CASE ("MDLS bytes in a material name or the vertices don't frame the skeleton") {
    const auto withName = puppet ({ .material = "materials/MDLS_embedded.json" });

    CHECK (findPuppetSkeleton (withName.bytes) == withName.bytes.size ());

    const auto withSkeleton = puppet ({ .material = "materials/MDLS_embedded.json", .skeleton = true });

    CHECK (findPuppetSkeleton (withSkeleton.bytes) == withSkeleton.skeleton);

    // the padding in every vertex spells MDLS too
    auto inVertices = puppet ({ .padding = 4, .skeleton = true });
    const uint32_t stride = 56;

    for (int vertex = 0; vertex < 3; vertex++) {
	std::copy_n ("MDLS", 4, inVertices.bytes.begin () + inVertices.vertices + vertex * stride + 44);
    }

    CHECK (findPuppetSkeleton (inVertices.bytes) == inVertices.skeleton);

    const auto mesh = readPuppetMeshFile (withSkeleton.bytes);
    REQUIRE (mesh.has_value ());
    CHECK (mesh->skeleton == withSkeleton.skeleton);
    CHECK (mesh->meshCount == 1);
}

TEST_CASE ("MDLS is found among the sections after the meshes in any order") {
    const auto later = puppet ({ .skeleton = true, .attachmentsFirst = true });

    REQUIRE (later.skeleton > later.sections);
    CHECK (findPuppetSkeleton (later.bytes) == later.skeleton);
    CHECK (findMdlSection (later.bytes, later.sections, "MDAT") == later.sections);
    CHECK_FALSE (findMdlSection (later.bytes, later.sections, "MDLA").has_value ());

    const auto mesh = readPuppetMeshFile (later.bytes);
    REQUIRE (mesh.has_value ());
    CHECK (mesh->skeleton == later.skeleton);
    CHECK (mesh->sections == later.sections);

    PuppetRig rig;
    REQUIRE_NOTHROW (rig.load (later.bytes, mesh->sections, mesh->meshCount, "test"));
    CHECK (rig.bones.size () == 1);

    const auto first = puppet ({ .skeleton = true });
    CHECK (readPuppetMeshFile (first.bytes)->sections == first.skeleton);
}

TEST_CASE ("Meshes the header doesn't describe fall back to the stride search") {
    // UVs not last (a colour follows), so the declared format isn't used
    const auto colourLast = puppet ({ .format = SKINNED | 0x8000 });
    auto mesh = readPuppetMeshFile (colourLast.bytes);

    REQUIRE (mesh.has_value ());
    CHECK_FALSE (mesh->declaredLayout);

    const auto padded = puppet ({ .padding = 4 });
    mesh = readPuppetMeshFile (padded.bytes);

    // one triangle can't rank strides, any usable mesh will do
    REQUIRE (mesh.has_value ());
    CHECK_FALSE (mesh->declaredLayout);
    CHECK (mesh->indices.size () == 3);

    const auto declared = puppet ();
    mesh = readPuppetMeshFile (declared.bytes);

    REQUIRE (mesh.has_value ());
    CHECK (mesh->declaredLayout);
    CHECK (mesh->flags == 0);
    CHECK (mesh->format == SKINNED);
}

TEST_CASE ("Broken puppet meshes are refused without throwing") {
    const auto good = puppet ();

    const auto pastEnd = puppet ({ .indices = { 0, 1, 7 } });
    CHECK_NOTHROW (readPuppetMeshFile (pastEnd.bytes));
    CHECK_FALSE (readPuppetMeshFile (pastEnd.bytes).has_value ());

    for (const size_t length : { size_t (0), size_t (5), size_t (40), good.vertices + 20, good.indexEnd - 1 }) {
	INFO ("cut at " << length);
	const std::vector<char> cut (good.bytes.begin (), good.bytes.begin () + length);

	CHECK_NOTHROW (readPuppetMeshFile (cut));
	CHECK_NOTHROW (findPuppetSkeleton (cut));
    }
}

TEST_CASE ("A header that declares more index bytes than the file holds is refused") {
    auto bytes = puppet ().bytes;
    const auto good = puppet ();

    // the index byte count sits right before the three indices
    const size_t countAt = good.indexEnd - 3 * sizeof (uint16_t) - sizeof (uint32_t);
    uint32_t declared = 0;
    std::memcpy (&declared, bytes.data () + countAt, sizeof (declared));
    REQUIRE (declared == 3 * sizeof (uint16_t));

    // about 2.9 GB of indices in a few hundred bytes used to be reserved before anything was read
    const uint32_t huge = 0xAFB8C39A;
    std::memcpy (bytes.data () + countAt, &huge, sizeof (huge));

    CHECK_NOTHROW (readPuppetMeshFile (bytes));
    CHECK_FALSE (readPuppetMeshFile (bytes).has_value ());
}

TEST_CASE ("The stride search of a damaged file gives up instead of trying every offset") {
    // no readable header, and every word says 600000 bytes: each offset looks like a mesh that fits a dozen strides
    // (a truncated 33 MB model ran for minutes)
    std::vector<char> bytes (8 * 1024 * 1024);
    const std::string tag ("MDLV0013");

    std::copy (tag.begin (), tag.end (), bytes.begin ());

    for (size_t offset = tag.size () + 1; offset + sizeof (uint32_t) <= bytes.size (); offset += sizeof (uint32_t)) {
	const uint32_t word = 600000;
	std::memcpy (bytes.data () + offset, &word, sizeof (word));
    }

    const auto start = std::chrono::steady_clock::now ();

    CHECK_NOTHROW (readPuppetMeshFile (bytes));

    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();

    INFO ("took " << seconds << " s");
    CHECK (seconds < 20.0);
}

TEST_CASE ("Bad optional puppet data leaves the plain mesh") {
    // an auxiliary block shorter than one vec3 per vertex is dropped with a warning
    const auto shortAux = puppet ({ .version = 21, .auxBytes = 12 });
    auto mesh = readPuppetMeshFile (shortAux.bytes);

    REQUIRE (mesh.has_value ());
    CHECK (mesh->auxPositions.empty ());
    CHECK (mesh->warnings.size () == 1);
    CHECK (mesh->positions.size () == 9);

    const auto aux = puppet ({ .version = 21, .auxBytes = 36 });
    mesh = readPuppetMeshFile (aux.bytes);

    REQUIRE (mesh.has_value ());
    CHECK (mesh->auxPositions.size () == 9);
    CHECK (mesh->auxPositions[8] == 8.0f);

    // bad part ranges or records throw, CImage draws the mesh without them
    const auto badParts = puppet ({ .flags = 8, .parts = { { 0, 0, 0, 9 } } });
    mesh = readPuppetMeshFile (badParts.bytes);

    REQUIRE (mesh.has_value ());
    CHECK_THROWS (PuppetClipping::readParts (badParts.bytes, mesh->indexEnd, mesh->version, mesh->indices.size ()));

    const auto badRecord = puppet ({ .parts = { { 0, 0, 0, 3 } }, .records = { { { 0 }, { 4 } } } });
    mesh = readPuppetMeshFile (badRecord.bytes);

    REQUIRE (mesh.has_value ());
    CHECK_THROWS (PuppetClipping::read (badRecord.bytes, mesh->indexEnd, mesh->version, mesh->indices.size ()));

    const auto goodRecord = puppet ({ .parts = { { 0, 0, 0, 3 } }, .records = { { { 0 }, {} } } });
    const auto clipping = PuppetClipping::read (
	goodRecord.bytes, goodRecord.indexEnd, 23, readPuppetMeshFile (goodRecord.bytes)->indices.size ()
    );

    REQUIRE (clipping.has_value ());
    CHECK (clipping->records.size () == 1);

    // truncated skeleton fails, CImage keeps the bind pose
    auto cutSkeleton = puppet ({ .skeleton = true });
    cutSkeleton.bytes.resize (cutSkeleton.skeleton + 20);
    mesh = readPuppetMeshFile (cutSkeleton.bytes);

    REQUIRE (mesh.has_value ());
    CHECK (mesh->skeleton == cutSkeleton.skeleton);

    PuppetRig rig;
    CHECK_THROWS (rig.load (cutSkeleton.bytes, mesh->skeleton, mesh->meshCount, "cut.mdl"));
}
