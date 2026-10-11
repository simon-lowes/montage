// Montage — reading zip archives (stored and deflated entries, Zip64 sizes),
// and gzipped tar archives, for model downloads that come packed: the
// inflater follows RFC 1951 and every entry is checked against its CRC-32.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace montage {

// Raw DEFLATE data (no zlib or gzip header) inflated onto `out`; false if it
// is malformed or ends early. `expected` reserves room when it is known.
bool inflateRaw(const uint8_t* data, size_t size, std::string& out, size_t expected = 0);

uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);

// A gzip file's contents (RFC 1952, one member), checked against its CRC-32 and length.
bool gunzip(const std::string& gz, std::string& out, std::string* error = nullptr);

// The regular files in a tar archive (POSIX ustar, with GNU long names): each name without a leading "./", and
// where its bytes are in `tar`. Empty if it is not a tar archive.
struct TarEntry {
    std::string name;
    size_t offset = 0, size = 0;
};
std::vector<TarEntry> tarEntries(const std::string& tar);

class ZipReader {
public:
    // The archive's bytes (kept); false if it is not a zip.
    bool open(std::string bytes);
    bool openFile(const std::string& path);

    struct Entry {
        std::string name;
        uint16_t method = 0;  // 0 stored, 8 deflated
        uint32_t crc = 0;
        uint64_t compressedSize = 0, size = 0, headerOffset = 0;
    };
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& name) const;
    // An entry's contents, checked against its CRC; false (with a reason) if it cannot be read.
    bool read(const Entry& e, std::string& out, std::string* error = nullptr) const;
    bool read(const std::string& name, std::string& out, std::string* error = nullptr) const;

private:
    std::string bytes_;
    std::vector<Entry> entries_;
};

}  // namespace montage
