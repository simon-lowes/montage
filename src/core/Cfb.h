// Montage — Microsoft Compound File Binary ([MS-CFB], "structured
// storage"): the container AAF files are made of. A tree of storages
// (folders, each with a class ID) and streams (bytes), written as version 4
// (4096-byte sectors) with small streams in the mini stream, and read back.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace montage {

struct CfbEntry {
    std::string name;  // at most 31 UTF-16 code units
    bool storage = false;
    std::array<uint8_t, 16> clsid{};  // storages: as stored (a GUID's little-endian bytes)
    std::string data;                 // streams
    std::vector<CfbEntry> children;   // storages

    CfbEntry* find(const std::string& child);
    const CfbEntry* find(const std::string& child) const;
    // A descendant by '/'-separated path.
    const CfbEntry* at(const std::string& path) const;
};

// `root` is the root storage (its name is ignored; its class ID is kept).
bool writeCompoundFile(const std::string& path, const CfbEntry& root, std::string* error = nullptr);
bool readCompoundFile(const std::string& path, CfbEntry& root, std::string* error = nullptr);

}  // namespace montage
