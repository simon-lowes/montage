// Montage — minimal cross-platform shared-library loader for plugin hosting.
#pragma once

#include <string>

namespace montage::plugins {

class DynLib {
public:
    DynLib() = default;
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;
    ~DynLib();

    // `path` is a library file (UTF-8). Returns false and sets `error` on failure.
    bool open(const std::string& path, std::string* error);
    void* symbol(const char* name) const;
    bool isOpen() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

// The loadable binary inside a plugin bundle (macOS .clap/.vst3 bundles,
// VST3 Contents/<arch> folders), or `path` itself when it is a file.
std::string pluginBinary(const std::string& path);

}  // namespace montage::plugins
