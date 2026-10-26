#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>

#include "WallpaperEngine/Data/Parsers/PackageParser.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/FileSystem/Adapters/Package.h"
#include "WallpaperEngine/Render/GifAnimation.h"

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::FileSystem::Adapters;
using WallpaperEngine::Render::GifAnimation;

namespace {
struct Bytes {
    std::string data;

    Bytes& u32 (const uint32_t value) {
	for (int shift = 0; shift < 32; shift += 8) {
	    this->data.push_back (static_cast<char> ((value >> shift) & 0xFF));
	}

	return *this;
    }

    Bytes& text (const std::string& value) {
	this->data += value;
	return *this;
    }

    Bytes& tag (const std::string& value) { return this->text (value).text (std::string (1, '\0')); }

    Bytes& sized (const std::string& value) { return this->u32 (static_cast<uint32_t> (value.size ())).text (value); }

    [[nodiscard]] ReadStreamSharedPtr stream () const { return std::make_shared<std::istringstream> (this->data); }
    [[nodiscard]] BinaryReader reader () const { return BinaryReader (this->stream ()); }
};

// TEXV0005 with a TEXI0001 header (a 4x4 RGBA image) and the start of a TEXB0001 container
Bytes textureStart () {
    Bytes bytes;

    bytes.tag ("TEXV0005").tag ("TEXI0001");
    bytes.u32 (0).u32 (0).u32 (4).u32 (4).u32 (4).u32 (4).u32 (0);
    bytes.tag ("TEXB0001");

    return bytes;
}
}

TEST_CASE ("A read past the end gives zeros, not whatever was on the stack") {
    Bytes bytes;
    bytes.text (std::string ("\x01\x02", 2));
    auto reader = bytes.reader ();

    CHECK (reader.nextUInt32 () == 0x0201);
    CHECK (reader.base ().fail ());
    CHECK (reader.nextUInt32 () == 0);
    CHECK (reader.nextInt () == 0);
    CHECK_THAT (reader.nextFloat (), Catch::Matchers::WithinULP (0.0f, 0));

    char out[8];
    std::memset (out, 0x7F, sizeof (out));
    reader.next (out, sizeof (out));

    for (const char byte : out) {
	CHECK (byte == 0);
    }
}

TEST_CASE ("A string without a terminator ends with the data") {
    Bytes bytes;
    bytes.text ("abc");

    CHECK (bytes.reader ().nextNullTerminatedString () == "abc");
}

TEST_CASE ("A sized string longer than the data is refused without being allocated") {
    Bytes bytes;
    bytes.u32 (0xFFFFFFFFu).text ("ab");
    auto reader = bytes.reader ();

    CHECK (reader.nextSizedString ().empty ());
    CHECK (reader.base ().fail ());
}

TEST_CASE ("A package that lists more files than it can hold is refused") {
    Bytes bytes;
    bytes.sized ("PKGV0001").u32 (0xFFFFFFFFu);

    CHECK_THROWS_AS (PackageParser::parse (bytes.stream ()), std::runtime_error);
}

TEST_CASE ("A package entry past the end is refused and the entries after it still read") {
    constexpr uint32_t headerSize = 4 + 8 + 4 + (4 + 7 + 8) + (4 + 6 + 8);
    Bytes bytes;

    bytes.sized ("PKGV0001").u32 (2);
    bytes.sized ("bad.bin").u32 (0).u32 (0x40000000u);
    bytes.sized ("ok.txt").u32 (0).u32 (5);
    REQUIRE (bytes.data.size () == headerSize);
    bytes.text ("hello");

    PackageAdapter adapter (PackageParser::parse (bytes.stream ()));

    CHECK_THROWS_AS (adapter.open ("bad.bin"), std::filesystem::filesystem_error);

    const auto good = adapter.open ("ok.txt");
    std::string content (5, '\0');
    good->read (content.data (), 5);

    CHECK (content == "hello");
}

TEST_CASE ("A texture that claims more mipmap data than the file holds is refused") {
    auto bytes = textureStart ();

    // one image, one mipmap of 4x4 that says it is 2 GB long
    bytes.u32 (1).u32 (1).u32 (4).u32 (4).u32 (0x7FFFFFF0u);

    auto reader = bytes.reader ();

    CHECK_THROWS_AS (TextureParser::parse (reader), std::runtime_error);
}

TEST_CASE ("A texture without any mipmap is refused") {
    auto bytes = textureStart ();

    bytes.u32 (1).u32 (0);

    auto reader = bytes.reader ();

    CHECK_THROWS_AS (TextureParser::parse (reader), std::runtime_error);
}

TEST_CASE ("A texture that claims billions of images is refused") {
    auto bytes = textureStart ();

    bytes.u32 (0x7FFFFFFFu);

    auto reader = bytes.reader ();

    CHECK_THROWS_AS (TextureParser::parse (reader), std::runtime_error);
}

TEST_CASE ("A texture with room for what it claims still loads") {
    auto bytes = textureStart ();

    // one image, one 4x4 mipmap of 64 raw bytes
    bytes.u32 (1).u32 (1).u32 (4).u32 (4).u32 (64).text (std::string (64, 'x'));

    auto reader = bytes.reader ();
    const auto texture = TextureParser::parse (reader);

    REQUIRE (texture != nullptr);
    REQUIRE (texture->images.size () == 1);
    REQUIRE (texture->images.at (0).size () == 1);
    CHECK (texture->images.at (0).front ()->uncompressedSize == 64);
}

TEST_CASE ("A GIF whose header claims a canvas of billions of pixels is refused") {
    // GIF89a, 65535 x 65535, no color table
    const uint8_t header[] = { 'G', 'I', 'F', '8', '9', 'a', 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00 };
    const auto gif = GifAnimation::open (header, sizeof (header));

    REQUIRE (gif != nullptr);
    CHECK_FALSE (gif->advance (0.0f));
}

TEST_CASE ("A one pixel GIF still plays") {
    const uint8_t pixel[] = { 0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF,
			      0xFF, 0x00, 0x00, 0x00, 0x21, 0xF9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00,
			      0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3B };
    const auto gif = GifAnimation::open (pixel, sizeof (pixel));

    REQUIRE (gif != nullptr);
    CHECK (gif->advance (0.0f));
    CHECK (gif->width () == 1);
    CHECK (gif->height () == 1);
}
