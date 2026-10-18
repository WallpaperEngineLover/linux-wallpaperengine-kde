#include <bit>
#include <iostream>

#include "BinaryReader.h"

#include <cstring>

using namespace WallpaperEngine::Data::Utils;

BinaryReader::BinaryReader (ReadStreamSharedPtr file) : m_input (std::move (file)) { }

// a read that comes up short leaves zeros for the missing bytes (and the stream's failbit for callers that look)
uint32_t BinaryReader::nextUInt32 () const {
    char buffer[4] = {};

    this->m_input->read (buffer, 4);

    if constexpr (std::endian::native == std::endian::little) {
	return (buffer[3] & 0xFF) << 24 | (buffer[2] & 0xFF) << 16 | (buffer[1] & 0xFF) << 8 | (buffer[0] & 0xFF);
    } else {
	return (buffer[0] & 0xFF) << 24 | (buffer[1] & 0xFF) << 16 | (buffer[2] & 0xFF) << 8 | (buffer[3] & 0xFF);
    }
}

int BinaryReader::nextInt () const {
    char buffer[4] = {};

    this->m_input->read (buffer, 4);

    if constexpr (std::endian::native == std::endian::little) {
	return (buffer[3] & 0xFF) << 24 | (buffer[2] & 0xFF) << 16 | (buffer[1] & 0xFF) << 8 | (buffer[0] & 0xFF);
    } else {
	return (buffer[0] & 0xFF) << 24 | (buffer[1] & 0xFF) << 16 | (buffer[2] & 0xFF) << 8 | (buffer[3] & 0xFF);
    }
}

float BinaryReader::nextFloat () const {
    float result = 0.0f;
    static_assert (std::endian::native == std::endian::little, "Only little endian is supported for floats");

    this->m_input->read (reinterpret_cast<char*> (&result), sizeof (result));

    return result;
}

std::string BinaryReader::nextNullTerminatedString () const {
    std::string output;

    // the end of the data ends the string too, there is no terminator to wait for
    for (char c = this->next (); this->m_input->good () && c != '\0'; c = this->next ()) {
	output += c;
    }

    return output;
}

std::string BinaryReader::nextSizedString () const {
    const uint32_t length = this->nextUInt32 ();

    // a length the file can't hold would allocate it (up to 4 GB) for nothing
    if (length > this->remaining ()) {
	this->m_input->setstate (std::ios::failbit);
	return {};
    }

    std::string output (length, '\0');

    this->m_input->read (output.data (), length);

    return output;
}

void BinaryReader::next (char* out, size_t size) const {
    this->m_input->read (out, static_cast<std::streamsize> (size));

    if (const auto got = static_cast<size_t> (this->m_input->gcount ()); got < size) {
	memset (out + got, 0, size - got);
    }
}

char BinaryReader::next () const {
    char buffer = 0;
    this->m_input->read (&buffer, 1);
    return buffer;
}

std::istream& BinaryReader::base () const { return *this->m_input; }

uint64_t BinaryReader::size () const {
    if (this->m_size < 0) {
	const auto here = this->m_input->tellg ();

	if (here < 0) {
	    return 0;
	}

	// relative moves only, MemoryStream has no absolute seek
	this->m_input->seekg (0, std::ios::end);
	const auto end = this->m_input->tellg ();
	this->m_input->seekg (here - end, std::ios::cur);
	this->m_size = static_cast<int64_t> (end);
    }

    return static_cast<uint64_t> (this->m_size);
}

uint64_t BinaryReader::remaining () const {
    const auto here = this->m_input->tellg ();

    if (here < 0) {
	return 0;
    }

    const uint64_t total = this->size ();

    return total > static_cast<uint64_t> (here) ? total - static_cast<uint64_t> (here) : 0;
}

bool BinaryReader::fits (const uint64_t count, const uint64_t recordBytes) const {
    if (count == 0 || recordBytes == 0) {
	return true;
    }

    // count * recordBytes without wrapping
    return count <= this->remaining () / recordBytes;
}
