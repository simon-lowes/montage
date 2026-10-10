#include "PackageFiles.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QXmlStreamReader>

namespace montage {

QString packageFileHash(const QString& path, qint64* size, const std::function<bool(qint64)>& read) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (size) *size = f.size();
    QCryptographicHash h(QCryptographicHash::Sha1);
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(1 << 20);
        h.addData(chunk);
        if (read && !read(chunk.size())) return {};
    }
    return QString::fromLatin1(h.result().toBase64());
}

QString packageTimestamp() { return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) + QStringLiteral("+00:00"); }

std::vector<std::pair<QString, QString>> packageXmlItems(const QString& file, QString* root) {
    std::vector<std::pair<QString, QString>> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return out;
    QXmlStreamReader r(&f);
    QStringList stack;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            if (stack.isEmpty() && root) *root = r.name().toString();
            stack << r.name().toString();
            out.push_back({stack.join('/'), {}});
        } else if (r.isCharacters() && !r.isWhitespace() && !out.empty()) {
            out.back().second += r.text().toString().trimmed();
        } else if (r.isEndElement()) {
            if (!stack.isEmpty()) stack.removeLast();
        }
    }
    if (r.hasError()) out.clear();
    return out;
}

QString packageBareId(QString id) { return id.remove(QStringLiteral("urn:uuid:")).toLower(); }

}  // namespace montage
