#include "DynLib.h"

#include <QDir>
#include <QFileInfo>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace montage::plugins {

DynLib::~DynLib() {
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
}

bool DynLib::open(const std::string& path, std::string* error) {
#ifdef _WIN32
    std::wstring wide = QString::fromStdString(path).toStdWString();
    handle_ = LoadLibraryW(wide.c_str());
    if (!handle_ && error) *error = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_ && error) {
        const char* e = dlerror();
        *error = e ? e : "dlopen failed";
    }
#endif
    return handle_ != nullptr;
}

void* DynLib::symbol(const char* name) const {
    if (!handle_) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

std::string pluginBinary(const std::string& path) {
    QFileInfo fi(QString::fromStdString(path));
    if (!fi.isDir()) return path;
    const QString stem = fi.completeBaseName();
    // macOS bundle: Contents/MacOS/<name>.
    QDir macos(fi.absoluteFilePath() + "/Contents/MacOS");
    if (macos.exists()) {
        if (macos.exists(stem)) return macos.filePath(stem).toStdString();
        const QStringList files = macos.entryList(QDir::Files);
        if (!files.isEmpty()) return macos.filePath(files.first()).toStdString();
    }
    // VST3 bundle layout on Windows and Linux: Contents/<arch>/<name>.<ext>.
#if defined(_WIN32)
    const QStringList archs = {"x86_64-win", "arm64x-win", "arm64ec-win"};
    const QString ext = "." + fi.suffix();
#else
    const QStringList archs = {"x86_64-linux", "aarch64-linux"};
    const QString ext = ".so";
#endif
    for (const QString& arch : archs) {
        QString candidate = fi.absoluteFilePath() + "/Contents/" + arch + "/" + stem + ext;
        if (QFileInfo::exists(candidate)) return candidate.toStdString();
    }
    return path;
}

}  // namespace montage::plugins
