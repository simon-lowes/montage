#include "Offload.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace montage {

namespace {

constexpr uint64_t kP1 = 11400714785074694791ULL, kP2 = 14029467366897019727ULL, kP3 = 1609587929392839161ULL,
                   kP4 = 9650029242287828579ULL, kP5 = 2870177450012600261ULL;

uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
uint64_t read64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
uint32_t read32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t round64(uint64_t acc, uint64_t input) { return rotl(acc + input * kP2, 31) * kP1; }
uint64_t merge64(uint64_t acc, uint64_t v) { return (acc ^ round64(0, v)) * kP1 + kP4; }

const char* const kToolVersion = "0.1.0";
// Left out of offloads and of hash lists (ASC MHL's own folder, and what the operating systems leave on cards).
const QStringList kIgnore = {".DS_Store", "._*", "Thumbs.db", ".Spotlight-V100", ".Trashes", ".fseventsd", "ascmhl", "ascmhl/"};
const QStringList kFormats = {"c4", "md5", "sha1", "xxh128", "xxh3", "xxh64"};  // the order ASC MHL's schema wants

QString now() { return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss+00:00")); }
QString stamp(const QDateTime& t) { return t.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss+00:00")); }

struct Ignore {
    std::vector<QRegularExpression> patterns;
    explicit Ignore(const QStringList& extra = {}) {
        QStringList all = kIgnore;
        all += extra;
        for (QString p : all) {
            if (p.endsWith('/')) p.chop(1);
            if (!p.isEmpty() && !p.contains('/')) patterns.emplace_back(QRegularExpression::wildcardToRegularExpression(p));
        }
    }
    bool matches(const QString& name) const {
        return std::any_of(patterns.begin(), patterns.end(), [&](const QRegularExpression& r) { return r.match(name).hasMatch(); });
    }
};

// A folder's contents in ASC MHL's order (by name, each folder's contents where it falls, then the folder).
struct Item {
    QString path;  // relative
    bool dir = false;
    qint64 size = 0;
    QDateTime modified;
};
void walk(const QString& root, const QString& rel, const Ignore& ignore, std::vector<Item>& out) {
    QDir d(rel.isEmpty() ? root : root + '/' + rel);
    const QFileInfoList list = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    std::vector<QFileInfo> sorted(list.begin(), list.end());
    std::sort(sorted.begin(), sorted.end(), [](const QFileInfo& a, const QFileInfo& b) { return a.fileName().toUtf8() < b.fileName().toUtf8(); });
    for (const QFileInfo& fi : sorted) {
        if (fi.isSymLink() || ignore.matches(fi.fileName())) continue;
        const QString path = rel.isEmpty() ? fi.fileName() : rel + '/' + fi.fileName();
        if (fi.isDir()) {
            walk(root, path, ignore, out);
            out.push_back({path, true, 0, fi.lastModified()});
        } else if (fi.isFile()) {
            out.push_back({path, false, fi.size(), fi.lastModified()});
        }
    }
}

// Reads a file past the system's cache where it can (a copy is checked on the disk, not in memory).
void dropCache(QFile& f) {
#if defined(__linux__)
    posix_fadvise(f.handle(), 0, 0, POSIX_FADV_DONTNEED);
#elif defined(__APPLE__)
    fcntl(f.handle(), F_NOCACHE, 1);
#else
    (void)f;
#endif
}

// Hashes in the given formats (xxh64 always) as one read. False when it cannot be read or `stop` says so.
struct Hasher {
    Xxh64 xxh;
    std::unique_ptr<QCryptographicHash> md5, sha1, sha512;
    explicit Hasher(const std::set<QString>& formats) {
        if (formats.count("md5")) md5 = std::make_unique<QCryptographicHash>(QCryptographicHash::Md5);
        if (formats.count("sha1")) sha1 = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha1);
        if (formats.count("c4")) sha512 = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha512);
    }
    void add(const char* data, qint64 n) {
        xxh.update(data, size_t(n));
        if (md5) md5->addData(QByteArrayView(data, n));
        if (sha1) sha1->addData(QByteArrayView(data, n));
        if (sha512) sha512->addData(QByteArrayView(data, n));
    }
    std::map<QString, QString> result() const {
        std::map<QString, QString> out{{"xxh64", QString::fromStdString(xxh64Hex(xxh.digest()))}};
        if (md5) out["md5"] = QString::fromLatin1(md5->result().toHex());
        if (sha1) out["sha1"] = QString::fromLatin1(sha1->result().toHex());
        if (sha512) out["c4"] = QString::fromStdString(c4FromDigest(sha512->result()));
        return out;
    }
    static std::string c4FromDigest(const QByteArray& digest);
};

constexpr qint64 kChunk = 8 << 20;

bool hashFile(const QString& path, const std::set<QString>& formats, bool uncached, std::map<QString, QString>& out,
              const std::function<bool(qint64)>& read) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (uncached) dropCache(f);
    Hasher h(formats);
    QByteArray buf(int(kChunk), Qt::Uninitialized);
    for (;;) {
        const qint64 n = f.read(buf.data(), kChunk);
        if (n < 0) return false;
        if (n == 0) break;
        h.add(buf.constData(), n);
        if (read && !read(n)) return false;
    }
    out = h.result();
    return true;
}

// The record a hash list keeps for a path: the latest generation's.
std::map<QString, MhlEntry> latestRecords(const std::vector<MhlGeneration>& history) {
    std::map<QString, MhlEntry> out;
    for (const MhlGeneration& g : history)
        for (const MhlEntry& e : g.entries) out[e.path] = e;
    return out;
}

// The hash a record is best checked by (xxh64 first), or null when it has none Montage computes.
const MhlHash* checkable(const MhlEntry& e) {
    const MhlHash* best = nullptr;
    for (const MhlHash& h : e.hashes) {
        if (h.format == "xxh64") return &h;
        if (!best && (h.format == "md5" || h.format == "sha1" || h.format == "c4")) best = &h;
    }
    return best;
}

std::set<QString> formatsFor(const MhlEntry* e) {
    std::set<QString> out{"xxh64"};
    if (e)
        if (const MhlHash* h = checkable(*e)) out.insert(h->format);
    return out;
}

// Hashes a list's file records need for the directory hashes: XXH64 of the sorted, decoded child hashes.
uint64_t hashOfHashes(std::vector<uint64_t> hashes) {
    std::sort(hashes.begin(), hashes.end());
    Xxh64 x;
    for (uint64_t h : hashes) {
        unsigned char b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(h >> (56 - 8 * i));
        x.update(b, 8);
    }
    return x.digest();
}
uint64_t nameHash(const QString& name, uint64_t h) {
    QByteArray b = name.toUtf8();
    for (int i = 0; i < 8; ++i) b.append(char(static_cast<unsigned char>(h >> (56 - 8 * i))));
    return Xxh64::of(b.constData(), size_t(b.size()));
}

struct Record {
    QString path;
    qint64 size = 0;
    QDateTime modified;
    std::vector<MhlHash> hashes;  // with xxh64 among them
};

struct ChainEntry {
    int number = 0;
    QString path, c4;
};
std::vector<ChainEntry> readChain(const QString& folder) {
    std::vector<ChainEntry> out;
    QFile f(folder + "/ascmhl_chain.xml");
    if (!f.open(QIODevice::ReadOnly)) return out;
    QXmlStreamReader x(&f);
    ChainEntry cur;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QStringView n = x.name();
            if (n == u"hashlist") cur = {x.attributes().value("sequencenr").toInt(), {}, {}};
            else if (n == u"path") cur.path = x.readElementText().trimmed();
            else if (n == u"c4") cur.c4 = x.readElementText().trimmed();
        } else if (x.isEndElement() && x.name() == u"hashlist") {
            out.push_back(cur);
        }
    }
    return out;
}

QStringList historyIgnores(const QString& root) {
    QStringList out;
    QDir d(root + "/ascmhl");
    for (const QString& name : d.entryList({"*.mhl"}, QDir::Files)) {
        QFile f(d.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QXmlStreamReader x(&f);
        while (!x.atEnd())
            if (x.readNext() == QXmlStreamReader::StartElement && x.name() == u"pattern") out << x.readElementText().trimmed();
    }
    out.removeDuplicates();
    return out;
}

// Writes a generation of `root`'s hash list and adds it to the chain. `items` is the walk of `root`; files without a
// record are left out of the list (and of the folder hashes).
QString writeGeneration(const QString& root, const std::vector<Item>& items, const std::map<QString, Record>& records,
                        const QString& process, const OffloadSettings& settings, QString* error) {
    const QString folder = root + "/ascmhl";
    if (!QDir().mkpath(folder)) {
        if (error) *error = QStringLiteral("Cannot make %1").arg(folder);
        return {};
    }
    std::vector<ChainEntry> chain = readChain(folder);
    int number = 0;
    for (const ChainEntry& c : chain) number = std::max(number, c.number);
    if (chain.empty())  // a history without a chain: count its lists
        number = int(QDir(folder).entryList({"*.mhl"}, QDir::Files).size());
    ++number;
    const QDateTime t = QDateTime::currentDateTimeUtc();
    const QString when = stamp(t), name = QFileInfo(root).fileName();
    const QString file = QStringLiteral("%1_%2_%3Z.mhl").arg(number, 4, 10, QChar('0')).arg(name, t.toString(QStringLiteral("yyyy-MM-dd_HHmmss")));

    // Folder hashes, children before their folder.
    struct Dir {
        std::vector<uint64_t> content, structure;
    };
    std::map<QString, Dir> dirs;
    std::map<QString, std::pair<uint64_t, uint64_t>> dirHashes;
    auto parentOf = [](const QString& p) {
        const int slash = p.lastIndexOf('/');
        return slash < 0 ? QString() : p.left(slash);
    };
    auto baseOf = [](const QString& p) { return p.mid(p.lastIndexOf('/') + 1); };
    for (const Item& it : items) {
        if (it.dir) {
            const Dir& d = dirs[it.path];
            const uint64_t c = hashOfHashes(d.content), s = hashOfHashes(d.structure);
            dirHashes[it.path] = {c, s};
            Dir& up = dirs[parentOf(it.path)];
            up.content.push_back(c);
            up.structure.push_back(nameHash(baseOf(it.path), s));
            continue;
        }
        const auto r = records.find(it.path);
        if (r == records.end()) continue;
        uint64_t h = 0;
        for (const MhlHash& x : r->second.hashes)
            if (x.format == "xxh64") h = x.value.toULongLong(nullptr, 16);
        Dir& up = dirs[parentOf(it.path)];
        up.content.push_back(h);
        up.structure.push_back(nameHash(baseOf(it.path), h));
    }
    const uint64_t rootContent = hashOfHashes(dirs[QString()].content), rootStructure = hashOfHashes(dirs[QString()].structure);

    QByteArray xml;
    QXmlStreamWriter w(&xml);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);
    w.writeStartDocument();
    w.writeStartElement("hashlist");
    w.writeAttribute("version", "2.0");
    w.writeDefaultNamespace("urn:ASC:MHL:v2.0");
    w.writeStartElement("creatorinfo");
    w.writeTextElement("creationdate", when);
    w.writeTextElement("hostname", QSysInfo::machineHostName().isEmpty() ? QStringLiteral("localhost") : QSysInfo::machineHostName());
    w.writeStartElement("tool");
    w.writeAttribute("version", kToolVersion);
    w.writeCharacters("Montage");
    w.writeEndElement();
    if (!settings.author.isEmpty()) w.writeTextElement("author", settings.author);
    if (!settings.location.isEmpty()) w.writeTextElement("location", settings.location);
    if (!settings.comment.isEmpty()) w.writeTextElement("comment", settings.comment);
    w.writeEndElement();
    w.writeStartElement("processinfo");
    w.writeTextElement("process", process);
    auto hashElement = [&](const QString& format, const QString& value, const QString& action) {
        w.writeStartElement(format);
        if (!action.isEmpty()) w.writeAttribute("action", action);
        w.writeAttribute("hashdate", when);
        w.writeCharacters(value);
        w.writeEndElement();
    };
    auto contentStructure = [&](uint64_t c, uint64_t s) {
        w.writeStartElement("content");
        hashElement("xxh64", QString::fromStdString(xxh64Hex(c)), {});
        w.writeEndElement();
        w.writeStartElement("structure");
        hashElement("xxh64", QString::fromStdString(xxh64Hex(s)), {});
        w.writeEndElement();
    };
    w.writeStartElement("roothash");
    contentStructure(rootContent, rootStructure);
    w.writeEndElement();
    w.writeStartElement("ignore");
    QStringList patterns = kIgnore + historyIgnores(root);
    patterns.removeDuplicates();
    for (const QString& p : patterns) w.writeTextElement("pattern", p);
    w.writeEndElement();
    w.writeEndElement();  // processinfo
    if (!items.empty()) {
        w.writeStartElement("hashes");
        for (const Item& it : items) {
            if (it.dir) {
                w.writeStartElement("directoryhash");
                w.writeStartElement("path");
                w.writeAttribute("lastmodificationdate", stamp(it.modified));
                w.writeCharacters(it.path);
                w.writeEndElement();
                contentStructure(dirHashes[it.path].first, dirHashes[it.path].second);
                w.writeEndElement();
                continue;
            }
            const auto r = records.find(it.path);
            if (r == records.end()) continue;
            w.writeStartElement("hash");
            w.writeStartElement("path");
            w.writeAttribute("size", QString::number(r->second.size));
            w.writeAttribute("lastmodificationdate", stamp(r->second.modified));
            w.writeCharacters(it.path);
            w.writeEndElement();
            for (const QString& f : kFormats)
                for (const MhlHash& h : r->second.hashes)
                    if (h.format == f) hashElement(f, h.value, h.action);
            w.writeEndElement();
        }
        w.writeEndElement();
    }
    w.writeEndElement();
    w.writeEndDocument();

    QSaveFile out(folder + '/' + file);
    if (!out.open(QIODevice::WriteOnly) || out.write(xml) != xml.size() || !out.commit()) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(folder + '/' + file);
        return {};
    }
    // The chain: every generation by its C4 ID.
    if (chain.empty())
        for (const QString& old : QDir(folder).entryList({"*.mhl"}, QDir::Files, QDir::Name)) {
            if (old == file) continue;
            QFile f(folder + '/' + old);
            if (!f.open(QIODevice::ReadOnly)) continue;
            chain.push_back({int(chain.size()) + 1, old, QString::fromStdString(c4Id(f.readAll()))});
        }
    chain.push_back({number, file, QString::fromStdString(c4Id(xml))});
    QByteArray cx;
    QXmlStreamWriter c(&cx);
    c.setAutoFormatting(true);
    c.setAutoFormattingIndent(2);
    c.writeStartDocument();
    c.writeStartElement("ascmhldirectory");
    c.writeDefaultNamespace("urn:ASC:MHL:DIRECTORY:v2.0");
    for (const ChainEntry& e : chain) {
        c.writeStartElement("hashlist");
        c.writeAttribute("sequencenr", QString::number(e.number));
        c.writeTextElement("path", e.path);
        c.writeTextElement("c4", e.c4);
        c.writeEndElement();
    }
    c.writeEndElement();
    c.writeEndDocument();
    QSaveFile cf(folder + "/ascmhl_chain.xml");
    if (!cf.open(QIODevice::WriteOnly) || cf.write(cx) != cx.size() || !cf.commit()) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(folder + "/ascmhl_chain.xml");
        return {};
    }
    return file;
}


}  // namespace

std::string Hasher::c4FromDigest(const QByteArray& digest) {
    static const char* kAlphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::vector<unsigned char> n(digest.begin(), digest.end());
    std::string out;
    // Repeated division of the 512-bit number by 58.
    while (std::any_of(n.begin(), n.end(), [](unsigned char b) { return b != 0; })) {
        int rem = 0;
        for (unsigned char& b : n) {
            const int cur = rem * 256 + b;
            b = static_cast<unsigned char>(cur / 58);
            rem = cur % 58;
        }
        out.insert(out.begin(), kAlphabet[rem]);
    }
    while (out.size() < 88) out.insert(out.begin(), '1');
    return "c4" + out;
}

Xxh64::Xxh64() : v_{kP1 + kP2, kP2, 0, uint64_t(0) - kP1}, buf_{} {}

void Xxh64::update(const void* data, size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    total_ += size;
    if (buffered_ + size < 32) {
        std::memcpy(buf_ + buffered_, p, size);
        buffered_ += size;
        return;
    }
    if (buffered_) {
        const size_t take = 32 - buffered_;
        std::memcpy(buf_ + buffered_, p, take);
        for (int i = 0; i < 4; ++i) v_[i] = round64(v_[i], read64(buf_ + 8 * i));
        p += take;
        size -= take;
        buffered_ = 0;
    }
    while (size >= 32) {
        for (int i = 0; i < 4; ++i) v_[i] = round64(v_[i], read64(p + 8 * i));
        p += 32;
        size -= 32;
    }
    std::memcpy(buf_, p, size);
    buffered_ = size;
}

uint64_t Xxh64::digest() const {
    uint64_t h;
    if (total_ >= 32) {
        h = rotl(v_[0], 1) + rotl(v_[1], 7) + rotl(v_[2], 12) + rotl(v_[3], 18);
        for (int i = 0; i < 4; ++i) h = merge64(h, v_[i]);
    } else {
        h = kP5;
    }
    h += total_;
    const unsigned char* p = buf_;
    size_t n = buffered_;
    while (n >= 8) {
        h = rotl(h ^ round64(0, read64(p)), 27) * kP1 + kP4;
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        h = rotl(h ^ (uint64_t(read32(p)) * kP1), 23) * kP2 + kP3;
        p += 4;
        n -= 4;
    }
    while (n--) h = rotl(h ^ (*p++ * kP5), 11) * kP1;
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

uint64_t Xxh64::of(const void* data, size_t size) {
    Xxh64 x;
    x.update(data, size);
    return x.digest();
}

std::string xxh64Hex(uint64_t h) {
    char s[17];
    std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(h));
    return s;
}

std::string c4Id(const QByteArray& data) { return Hasher::c4FromDigest(QCryptographicHash::hash(data, QCryptographicHash::Sha512)); }

std::vector<MhlGeneration> readMhlHistory(const QString& root, QString* error) {
    std::vector<MhlGeneration> out;
    const QString folder = root + "/ascmhl";
    if (!QDir(folder).exists()) return out;
    std::vector<ChainEntry> chain = readChain(folder);
    if (chain.empty()) {
        int k = 0;
        for (const QString& name : QDir(folder).entryList({"*.mhl"}, QDir::Files, QDir::Name)) chain.push_back({++k, name, {}});
    }
    std::sort(chain.begin(), chain.end(), [](const ChainEntry& a, const ChainEntry& b) { return a.number < b.number; });
    for (const ChainEntry& c : chain) {
        QFile f(folder + '/' + c.path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("The ASC MHL history names %1, which cannot be read").arg(c.path);
            return {};
        }
        MhlGeneration g;
        g.number = c.number;
        g.file = c.path;
        QXmlStreamReader x(&f);
        MhlEntry e;
        bool inHash = false;
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement()) {
                const QString n = x.name().toString();
                if (n == "hash") {
                    inHash = true;
                    e = MhlEntry{};
                } else if (n == "directoryhash" || n == "roothash") {
                    x.skipCurrentElement();
                } else if (inHash && n == "path") {
                    const QXmlStreamAttributes a = x.attributes();
                    e.size = a.hasAttribute("size") ? a.value("size").toLongLong() : -1;
                    e.modified = a.value("lastmodificationdate").toString();
                    e.path = x.readElementText().trimmed();
                } else if (inHash && kFormats.contains(n)) {
                    MhlHash h;
                    h.format = n;
                    h.action = x.attributes().value("action").toString();
                    h.value = x.readElementText().trimmed().toLower();
                    e.hashes.push_back(h);
                } else if (n == "creationdate") {
                    g.created = x.readElementText().trimmed();
                } else if (n == "tool") {
                    g.tool = x.readElementText().trimmed();
                } else if (n == "process") {
                    g.process = x.readElementText().trimmed();
                }
            } else if (x.isEndElement() && x.name() == u"hash") {
                inHash = false;
                if (!e.path.isEmpty()) g.entries.push_back(e);
            }
        }
        if (x.hasError()) {
            if (error) *error = QStringLiteral("%1 is not a readable hash list: %2").arg(c.path, x.errorString());
            return {};
        }
        out.push_back(std::move(g));
    }
    return out;
}

OffloadResult offloadCard(const QString& source, const QStringList& destinations, const OffloadSettings& settings,
                          const std::function<bool(double, const QString&)>& progress) {
    OffloadResult res;
    const QFileInfo card(source);
    if (!card.isDir()) {
        res.error = QStringLiteral("%1 is not a folder").arg(source);
        return res;
    }
    if (destinations.isEmpty()) {
        res.error = QStringLiteral("Choose where to copy the card");
        return res;
    }
    const QString name = card.fileName().isEmpty() ? QStringLiteral("Card") : card.fileName();
    const QString cardPath = card.canonicalFilePath();
    // Every destination checked before anything is made (nothing is ever written to the card).
    for (const QString& d : destinations) {
        // The destination's nearest existing folder, resolved, with the rest of its path after it.
        QString existing = QDir::cleanPath(QFileInfo(d).absoluteFilePath()), rest;
        while (!QFileInfo::exists(existing) && existing.contains('/')) {
            rest.prepend('/' + existing.mid(existing.lastIndexOf('/') + 1));
            existing = existing.left(existing.lastIndexOf('/'));
        }
        const QString resolved = QFileInfo(existing).canonicalFilePath() + rest;
        if (resolved == cardPath || resolved.startsWith(cardPath + '/')) {
            res.error = QStringLiteral("%1 is on the card itself").arg(d);
            return res;
        }
        const QString copy = QDir(d).absoluteFilePath(name);
        if (res.copies.contains(copy)) {
            res.error = QStringLiteral("%1 is named twice").arg(d);
            return res;
        }
        res.copies << copy;
    }
    for (const QString& d : destinations)
        if (!QDir().mkpath(d)) {
            res.error = QStringLiteral("Cannot use %1").arg(d);
            return res;
        }

    // What the card holds, and what its hash list says it should.
    QString historyError;
    const std::vector<MhlGeneration> cardHistory = readMhlHistory(source, &historyError);
    if (!historyError.isEmpty()) {
        res.error = historyError;
        return res;
    }
    const std::map<QString, MhlEntry> cardRecords = latestRecords(cardHistory);
    std::vector<Item> items;
    walk(source, {}, Ignore(historyIgnores(source)), items);
    qint64 total = 0;
    std::set<QString> onCard;
    for (const Item& it : items)
        if (!it.dir) {
            total += it.size;
            ++res.files;
            onCard.insert(it.path);
        }
    res.bytes = total;
    const int n = int(res.copies.size());
    const double work = double(std::max<qint64>(1, total)) * (1 + (settings.verify ? n : 0));
    double done = 0;
    QString current;
    auto tick = [&](qint64 bytes) {
        done += double(bytes);
        return !progress || progress(std::min(1.0, done / work), current);
    };
    for (const auto& [path, rec] : cardRecords)
        if (!onCard.count(path)) res.issues.push_back({path, QStringLiteral("In the card's hash list but not on the card")});

    // Each copy's history: its own when it has one (an offload resumed), else the card's (carried over at the end, so
    // a stopped offload leaves none).
    std::vector<std::map<QString, MhlEntry>> known(static_cast<size_t>(n));
    std::vector<char> carry(static_cast<size_t>(n), 0);
    for (int k = 0; k < n; ++k) {
        const QString copy = res.copies[k];
        if (!QDir().mkpath(copy)) {
            res.error = QStringLiteral("Cannot make %1").arg(copy);
            return res;
        }
        const std::vector<MhlGeneration> own = readMhlHistory(copy);
        if (!own.empty()) {
            known[size_t(k)] = latestRecords(own);
        } else if (!cardHistory.empty()) {
            known[size_t(k)] = cardRecords;
            carry[size_t(k)] = 1;
        }
    }

    std::vector<std::map<QString, Record>> records(static_cast<size_t>(n));
    QByteArray buf(int(kChunk), Qt::Uninitialized);
    for (const Item& it : items) {
        if (it.dir) {
            for (const QString& copy : res.copies) QDir().mkpath(copy + '/' + it.path);
            continue;
        }
        current = it.path;
        const auto earlier = cardRecords.find(it.path);
        const MhlEntry* cardRecord = earlier == cardRecords.end() ? nullptr : &earlier->second;
        // The formats to hash in: XXH64, and whatever the hash lists checking this file use.
        std::set<QString> formats = formatsFor(cardRecord);
        for (const auto& kn : known)
            if (const auto e = kn.find(it.path); e != kn.end())
                for (const QString& f : formatsFor(&e->second)) formats.insert(f);

        struct Target {
            QString path, part;
            std::unique_ptr<QFile> file;
            bool existing = false, failed = false;
        };
        std::vector<Target> targets(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k) {
            Target& t = targets[size_t(k)];
            t.path = res.copies[k] + '/' + it.path;
            if (QFileInfo::exists(t.path)) {
                t.existing = true;
                continue;
            }
            QDir().mkpath(QFileInfo(t.path).absolutePath());
            t.part = t.path + ".montage-part";
            t.file = std::make_unique<QFile>(t.part);
            if (!t.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                res.issues.push_back({it.path, QStringLiteral("Cannot write to %1").arg(res.copies[k])});
                t.failed = true;
                t.file.reset();
            }
        }
        auto abandon = [&] {
            for (Target& t : targets)
                if (t.file) {
                    t.file->close();
                    QFile::remove(t.part);
                }
        };
        QFile in(source + '/' + it.path);
        if (!in.open(QIODevice::ReadOnly)) {
            res.issues.push_back({it.path, QStringLiteral("Cannot be read from the card")});
            abandon();
            continue;
        }
        Hasher h(formats);
        bool readOk = true;
        for (;;) {
            const qint64 got = in.read(buf.data(), kChunk);
            if (got < 0) {
                readOk = false;
                break;
            }
            if (got == 0) break;
            h.add(buf.constData(), got);
            for (int k = 0; k < n; ++k) {
                Target& t = targets[size_t(k)];
                if (t.file && t.file->write(buf.constData(), got) != got) {
                    res.issues.push_back({it.path, QStringLiteral("Writing to %1 failed (is it full?)").arg(res.copies[k])});
                    t.file->close();
                    QFile::remove(t.part);
                    t.file.reset();
                    t.failed = true;
                }
            }
            if (!tick(got)) {
                abandon();
                res.error = QStringLiteral("Cancelled");
                return res;
            }
        }
        if (!readOk) {
            res.issues.push_back({it.path, QStringLiteral("Cannot be read from the card")});
            abandon();
            continue;
        }
        const std::map<QString, QString> hashes = h.result();
        const QString xxh = hashes.at("xxh64");
        // Changed on the card since its hash list was made?
        bool cardChanged = false;
        if (cardRecord)
            if (const MhlHash* rec = checkable(*cardRecord); rec && hashes.count(rec->format) && hashes.at(rec->format) != rec->value) {
                cardChanged = true;
                res.issues.push_back({it.path, QStringLiteral("Differs from the card's hash list (changed since it was hashed)")});
            }
        for (int k = 0; k < n; ++k) {
            Target& t = targets[size_t(k)];
            if (t.failed) continue;
            if (t.file) {
                t.file->flush();
#if defined(__unix__) || defined(__APPLE__)
                if (settings.verify) ::fsync(t.file->handle());
#endif
                t.file->setFileTime(it.modified, QFileDevice::FileModificationTime);
                t.file->close();
                QFile::remove(t.path);
                if (!QFile::rename(t.part, t.path)) {
                    res.issues.push_back({it.path, QStringLiteral("Cannot finish the copy in %1").arg(res.copies[k])});
                    QFile::remove(t.part);
                    continue;
                }
            }
            // Read back (what was already there always is).
            if (t.existing || settings.verify) {
                std::map<QString, QString> back;
                if (!hashFile(t.path, {"xxh64"}, true, back, [&](qint64 b) { return t.existing || tick(b); })) {
                    if (progress && !progress(std::min(1.0, done / work), current)) {
                        res.error = QStringLiteral("Cancelled");
                        return res;
                    }
                    res.issues.push_back({it.path, QStringLiteral("Cannot read the copy in %1 back").arg(res.copies[k])});
                    continue;
                }
                if (back.at("xxh64") != xxh) {
                    res.issues.push_back({it.path, t.existing ? QStringLiteral("A different file is already in %1; left as it was").arg(res.copies[k])
                                                              : QStringLiteral("The copy in %1 does not match the card").arg(res.copies[k])});
                    continue;
                }
                if (t.existing) {
                    ++res.alreadyThere;
                    if (settings.verify) tick(it.size);  // counted as if read back
                }
            }
            // How this copy's hash list records it: against what the list knew, XXH64 always.
            Record r{it.path, it.size, QFileInfo(t.path).lastModified(), {}};
            const auto e = known[size_t(k)].find(it.path);
            const MhlHash* prior = e == known[size_t(k)].end() ? nullptr : checkable(e->second);
            if (prior && prior->format != "xxh64")
                r.hashes.push_back({prior->format, hashes.at(prior->format), hashes.at(prior->format) == prior->value ? "verified" : "failed"});
            const QString action = cardChanged ? QStringLiteral("failed")
                                   : prior && prior->format == "xxh64" ? (prior->value == xxh ? QStringLiteral("verified") : QStringLiteral("failed"))
                                                                       : QStringLiteral("original");
            r.hashes.push_back({"xxh64", xxh, action});
            records[size_t(k)][it.path] = r;
        }
    }
    if (settings.mhl)
        for (int k = 0; k < n; ++k) {
            const QString copy = res.copies[k];
            if (carry[size_t(k)]) {
                QDir().mkpath(copy + "/ascmhl");
                bool copied = true;
                for (const QString& f : QDir(source + "/ascmhl").entryList(QDir::Files)) {
                    QFile::remove(copy + "/ascmhl/" + f);
                    copied = QFile::copy(source + "/ascmhl/" + f, copy + "/ascmhl/" + f) && copied;
                }
                if (!copied) {
                    res.issues.push_back({QString(), QStringLiteral("Cannot copy the card's hash list to %1").arg(copy)});
                    continue;
                }
            }
            std::vector<Item> copyItems;
            walk(res.copies[k], {}, Ignore(historyIgnores(res.copies[k])), copyItems);
            QString err;
            if (writeGeneration(res.copies[k], copyItems, records[size_t(k)], QStringLiteral("transfer"), settings, &err).isEmpty())
                res.issues.push_back({QString(), err});
        }
    if (progress) progress(1.0, QString());
    res.ok = res.issues.empty();
    return res;
}

MhlVerifyResult verifyMhl(const QString& root, bool writeGeneration_, const OffloadSettings& settings,
                          const std::function<bool(double, const QString&)>& progress) {
    MhlVerifyResult res;
    if (!QFileInfo(root).isDir()) {
        res.error = QStringLiteral("%1 is not a folder").arg(root);
        return res;
    }
    const std::vector<MhlGeneration> history = readMhlHistory(root, &res.error);
    if (!res.error.isEmpty()) return res;
    if (history.empty() && !writeGeneration_) {
        res.error = QStringLiteral("%1 has no ASC MHL history (an ascmhl folder)").arg(root);
        return res;
    }
    const std::map<QString, MhlEntry> recorded = latestRecords(history);
    std::vector<Item> items;
    walk(root, {}, Ignore(historyIgnores(root)), items);
    std::set<QString> present;
    qint64 total = 0;
    for (const Item& it : items)
        if (!it.dir) {
            present.insert(it.path);
            total += it.size;
        }
    for (const auto& [path, e] : recorded)
        if (!present.count(path)) res.missing << path;
    double done = 0;
    std::map<QString, Record> records;
    for (const Item& it : items) {
        if (it.dir) continue;
        const auto e = recorded.find(it.path);
        const MhlEntry* rec = e == recorded.end() ? nullptr : &e->second;
        std::map<QString, QString> hashes;
        const bool read = hashFile(root + '/' + it.path, formatsFor(rec), false, hashes, [&](qint64 b) {
            done += double(b);
            return !progress || progress(total > 0 ? std::min(1.0, done / double(total)) : 1.0, it.path);
        });
        if (!read) {
            if (progress && !progress(total > 0 ? done / double(total) : 1.0, it.path)) {
                res.error = QStringLiteral("Cancelled");
                return res;
            }
            res.changed << it.path;  // unreadable counts as not matching
            continue;
        }
        Record r{it.path, it.size, it.modified, {}};
        const MhlHash* check = rec ? checkable(*rec) : nullptr;
        QString xxhAction = QStringLiteral("original");
        if (!rec) {
            res.added << it.path;
        } else if (!check) {
            res.unchecked << it.path;
        } else {
            const bool same = hashes.at(check->format) == check->value;
            if (same) ++res.verified;
            else res.changed << it.path;
            if (check->format == "xxh64") xxhAction = same ? QStringLiteral("verified") : QStringLiteral("failed");
            else r.hashes.push_back({check->format, hashes.at(check->format), same ? QStringLiteral("verified") : QStringLiteral("failed")});
        }
        r.hashes.push_back({"xxh64", hashes.at("xxh64"), xxhAction});
        records[it.path] = r;
    }
    res.ok = res.missing.isEmpty() && res.changed.isEmpty();
    if (writeGeneration_) {
        res.generation = writeGeneration(root, items, records, QStringLiteral("in-place"), settings, &res.error);
        if (res.generation.isEmpty()) res.ok = false;
    }
    if (progress) progress(1.0, QString());
    return res;
}

}  // namespace montage
