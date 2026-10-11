#include "ModelFiles.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <cstdlib>

namespace montage {

std::string modelsDirectory() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.montage";
    return dir.toStdString();
}

std::string ModelPack::directory() const {
    if (!directoryEnv.empty())
        if (const char* env = std::getenv(directoryEnv.c_str()); env && *env) return env;
    if (!parentEnv.empty())
        if (const char* env = std::getenv(parentEnv.c_str()); env && *env) return std::string(env) + "/" + id;
    return modelsDirectory() + "/models/" + id;
}

std::string ModelPack::path(const ModelFile& f) const { return directory() + "/" + f.name; }

std::string ModelPack::url(const ModelFile& f) const {
    if (!urlEnv.empty())
        if (const char* env = std::getenv(urlEnv.c_str()); env && *env) {
            std::string base = env;
            if (base.back() == '/') base.pop_back();
            return base + "/" + f.name;
        }
    return f.url;
}

int64_t ModelPack::bytes() const {
    int64_t n = 0;
    for (const auto& f : files) n += f.bytes;
    return n;
}

bool ModelPack::installed() const {
    for (const auto& f : files)
        if (QFileInfo(QString::fromStdString(path(f))).size() != f.bytes) return false;
    return !files.empty();
}

bool modelFileVerified(const std::string& path, const ModelFile& f) {
    QFile file(QString::fromStdString(path));
    if (file.size() != f.bytes || !file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&file)) return false;
    return h.result().toHex().toStdString() == f.sha256;
}

}  // namespace montage
