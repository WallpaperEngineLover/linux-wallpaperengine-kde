#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>

namespace WallpaperEngine::Data::Utils {
using ReadStream = std::istream;
using ReadStreamSharedPtr = std::shared_ptr<ReadStream>;

class BinaryReader {
public:
    explicit BinaryReader (ReadStreamSharedPtr file);

    [[nodiscard]] uint32_t nextUInt32 () const;
    [[nodiscard]] int nextInt () const;
    [[nodiscard]] float nextFloat () const;
    [[nodiscard]] std::string nextNullTerminatedString () const;
    [[nodiscard]] std::string nextSizedString () const;
    void next (char* out, size_t size) const;
    void skipUInt32 () const;
    void skipNullTerminatedString () const;
    [[nodiscard]] char next () const;

    [[nodiscard]] std::istream& base () const;

    [[nodiscard]] uint64_t size () const;
    [[nodiscard]] uint64_t remaining () const;
    [[nodiscard]] bool fits (uint64_t count, uint64_t recordBytes) const;

private:
    ReadStreamSharedPtr m_input;
    mutable int64_t m_size = -1;
};

using BinaryReaderUniquePtr = std::unique_ptr<BinaryReader>;
}
