// Montage — machine-learning models that are downloaded on first use: where
// each pack lives, which files it needs, and how to check them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage {

struct ModelFile {
    std::string name;    // file name in the pack's folder
    std::string url;     // where it is downloaded from (a pinned revision)
    std::string sha256;  // checked after downloading
    int64_t bytes = 0;
};

struct ModelPack {
    std::string id;           // folder name under the models folder, e.g. "edgetam-video"
    std::string title;        // for people, e.g. "object model"
    std::string directoryEnv; // environment variable naming the folder instead
    std::string urlEnv;       // environment variable replacing every file's folder URL (a mirror, or file:// in tests)
    std::vector<ModelFile> files;

    std::string directory() const;
    std::string path(const ModelFile& f) const;
    std::string url(const ModelFile& f) const;
    int64_t bytes() const;
    // Every file is present with its expected size.
    bool installed() const;
};

// The folder downloaded models go in (app data location).
std::string modelsDirectory();
// SHA-256 of the file matches (and its size).
bool modelFileVerified(const std::string& path, const ModelFile& f);

}  // namespace montage
