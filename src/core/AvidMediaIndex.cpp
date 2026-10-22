#include "AvidMediaIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QUrl>
#include <algorithm>

#include "Aaf.h"

namespace montage {

std::string findInterchangeFile(const std::string& url, const std::string& aafDir) {
    QString path = QString::fromStdString(url);
    if (path.startsWith("file:", Qt::CaseInsensitive)) {
        const QUrl u(path);
        path = u.isLocalFile() ? u.toLocalFile() : QUrl::fromPercentEncoding(path.mid(5).toUtf8());
        // Windows paths written as file:///C:/...
        if (path.size() > 2 && path[0] == '/' && path[2] == ':') path = path.mid(1);
    }
    if (!path.isEmpty() && QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath().toStdString();
    const QString name = QFileInfo(path.replace('\\', '/')).fileName();
    if (name.isEmpty()) return path.toStdString();
    const QDir dir(QString::fromStdString(aafDir));
    if (QFileInfo(dir.filePath(name)).isFile()) return dir.filePath(name).toStdString();
    for (const QFileInfo& sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo(sub.filePath() + '/' + name).isFile()) return (sub.filePath() + '/' + name).toStdString();
    return path.toStdString();
}

std::vector<std::string> mxfPackageIds(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = f.read(128 * 1024);
    static const QByteArray tag("\x44\x01\x00\x20\x06\x0a\x2b\x34", 8);
    std::vector<std::string> ids;
    for (qsizetype at = head.indexOf(tag); at >= 0 && at + 36 <= head.size(); at = head.indexOf(tag, at + 1))
        if (const std::string id = aafMobIdFromUmidBytes(head.mid(at + 4, 32).toStdString()); !id.empty()) ids.push_back(id);
    return ids;
}

std::string AvidMediaIndex::find(const std::string& mobId, const std::string& url) {
    if (roots_.isEmpty()) findRoots();
    // "Avid MediaFiles/MXF/<n>/<file>" from the locator, under each.
    QString path = QUrl(QString::fromStdString(url)).isLocalFile() ? QUrl(QString::fromStdString(url)).toLocalFile() : QString::fromStdString(url);
    path.replace('\\', '/');
    const qsizetype at = path.lastIndexOf(QLatin1String("/Avid MediaFiles/"), -1, Qt::CaseInsensitive);
    if (at >= 0)
        for (const QString& root : roots_) {
            if (root.isEmpty()) continue;
            const QString there = root + path.mid(at);
            if (QFileInfo(there).isFile()) {
                const std::vector<std::string> ids = mxfPackageIds(there.toStdString());
                if (std::find(ids.begin(), ids.end(), mobId) != ids.end()) return there.toStdString();
            }
        }
    if (!scanned_) scan();
    const auto it = files_.find(mobId);
    return it == files_.end() ? std::string() : it->second;
}

void AvidMediaIndex::findRoots() {
    QStringList roots;
    for (QDir d(QString::fromStdString(dir_));;) {
        roots << d.absolutePath();
        if (!d.cdUp()) break;
    }
    // Not network shares: reading every file's header there could take very long.
    static const char* const remote[] = {"nfs", "cifs", "smb", "afp", "webdav", "fuse.sshfs", "9p"};
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        const QByteArray type = v.fileSystemType().toLower();
        if (v.isValid() && v.isReady() &&
            std::none_of(std::begin(remote), std::end(remote), [&](const char* r) { return type.startsWith(r); }))
            roots << v.rootPath();
    }
    for (QString& r : roots)
        if (r.endsWith('/')) r.chop(1);
    roots.removeDuplicates();
    for (const QString& r : roots)
        if (QFileInfo(r + "/Avid MediaFiles/MXF").isDir()) roots_ << r;
    if (roots_.isEmpty()) roots_ << QString();  // looked: none
}

void AvidMediaIndex::scan() {
    scanned_ = true;
    for (const QString& root : roots_) {
        if (root.isEmpty()) continue;
        const QDir mxf(root + "/Avid MediaFiles/MXF");
        for (const QFileInfo& folder : mxf.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            for (const QFileInfo& f : QDir(folder.filePath()).entryInfoList({"*.mxf"}, QDir::Files))
                for (const std::string& id : mxfPackageIds(f.absoluteFilePath().toStdString())) files_.emplace(id, f.absoluteFilePath().toStdString());
    }
}

}  // namespace montage
