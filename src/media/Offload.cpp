#include "Offload.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
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
// What the operating systems leave on cards: never copied, never hashed.
const QStringList kJunk = {".DS_Store", "._*", "Thumbs.db", ".Spotlight-V100", ".Trashes", ".fseventsd"};
// Left out of hash lists (and written as their ignore patterns): the junk, and ASC MHL's own folders at any depth.
const QStringList kHashIgnore = kJunk + QStringList{"ascmhl", "ascmhl/"};
const QStringList kFormats = {"c4", "md5", "sha1", "xxh128", "xxh3", "xxh64"};  // the order ASC MHL's schema wants

#if defined(_WIN32) || defined(__APPLE__)
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;  // NTFS and APFS as usually formatted
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString now() { return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss+00:00")); }
QString stamp(const QDateTime& t) { return t.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss+00:00")); }

struct Ignore {
    std::vector<QRegularExpression> patterns;
    explicit Ignore(const QStringList& list) {
        for (QString p : list) {
            if (p.endsWith('/')) p.chop(1);
            if (!p.isEmpty() && !p.contains('/')) patterns.emplace_back(QRegularExpression::wildcardToRegularExpression(p));
        }
    }
    bool matches(const QString& name) const {
        return std::any_of(patterns.begin(), patterns.end(), [&](const QRegularExpression& r) { return r.match(name).hasMatch(); });
    }
    // Whether any folder or file name along a relative path is ignored.
    bool covers(const QString& path) const {
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);
        return std::any_of(parts.begin(), parts.end(), [&](const QString& n) { return matches(n); });
    }
};

// A folder's contents in ASC MHL's order (by name, each folder's contents where it falls, then the folder).
struct Item {
    QString path;  // relative
    bool dir = false;
    qint64 size = 0;
    QDateTime modified;
};
struct Walk {
    std::vector<Item> items;
    QStringList unreadable;  // folders that could not be listed
    QStringList skipped;     // links and special files, never followed or copied
    int files() const {
        return int(std::count_if(items.begin(), items.end(), [](const Item& i) { return !i.dir; }));
    }
};
// Names as hash lists write them: NFC on macOS (whose file systems hand back decomposed names), as they are elsewhere.
QString listName(const QString& n) {
#if defined(__APPLE__)
    return n.normalized(QString::NormalizationForm_C);
#else
    return n;
#endif
}
// Everything but the junk, and but the ASC MHL folder at the top (a hash list's own history, handled apart).
void walk(const QString& root, const QString& rel, const Ignore& junk, Walk& out) {
    const QString here = rel.isEmpty() ? root : root + '/' + rel;
    QDir d(here);
    if (!QFileInfo(here).isReadable() || !d.exists()) {
        out.unreadable << (rel.isEmpty() ? QStringLiteral(".") : rel);
        return;
    }
    const QFileInfoList list = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    std::vector<QFileInfo> sorted(list.begin(), list.end());
    std::sort(sorted.begin(), sorted.end(), [](const QFileInfo& a, const QFileInfo& b) { return a.fileName().toUtf8() < b.fileName().toUtf8(); });
    for (const QFileInfo& fi : sorted) {
        if (junk.matches(fi.fileName())) continue;
        if (rel.isEmpty() && fi.fileName() == QLatin1String("ascmhl") && fi.isDir()) continue;
        const QString path = (rel.isEmpty() ? QString() : rel + '/') + listName(fi.fileName());
        if (fi.isSymLink()) {
            out.skipped << path;
        } else if (fi.isDir()) {
            walk(root, path, junk, out);
            out.items.push_back({path, true, 0, fi.lastModified()});
        } else if (fi.isFile()) {
            out.items.push_back({path, false, fi.size(), fi.lastModified()});
        } else {
            out.skipped << path;
        }
    }
}
// What a hash list of `root` covers: the walk without what its ignore patterns leave out.
std::vector<Item> hashed(const std::vector<Item>& items, const Ignore& ignore) {
    std::vector<Item> out;
    for (const Item& it : items)
        if (!ignore.covers(it.path)) out.push_back(it);
    return out;
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

// Everything written reaches the disk, or the failure is known: Qt's buffer, the system's and, on macOS, the drive's
// own cache (F_FULLFSYNC). On macOS the writer also bypasses the cache, so reading the copy back reads the disk.
bool flushToDisk(QFile& f) {
    if (!f.flush()) return false;
#if defined(__APPLE__)
    if (fcntl(f.handle(), F_FULLFSYNC) != 0 && ::fsync(f.handle()) != 0) return false;
#elif defined(__unix__)
    if (::fsync(f.handle()) != 0) return false;
#endif
    return true;
}
void writeUncached(QFile& f) {
#if defined(__APPLE__)
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

// What a hash list holds a file to be, as ascmhl compares: for each hash format the earliest value not marked failed
// (a later "failed" record holds the damage, not the file), with the latest size.
std::map<QString, MhlEntry> referenceRecords(const std::vector<MhlGeneration>& history) {
    std::map<QString, MhlEntry> out;
    for (const MhlGeneration& g : history)
        for (const MhlEntry& e : g.entries) {
            MhlEntry& r = out[e.path];
            r.path = e.path;
            if (e.size >= 0) r.size = e.size;
            if (!e.modified.isEmpty()) r.modified = e.modified;
            for (const MhlHash& h : e.hashes)
                if (h.action != QLatin1String("failed") &&
                    std::none_of(r.hashes.begin(), r.hashes.end(), [&](const MhlHash& x) { return x.format == h.format; }))
                    r.hashes.push_back(h);
        }
    // A file only ever recorded as failed has no reference.
    for (auto it = out.begin(); it != out.end();) it = it->second.hashes.empty() ? out.erase(it) : std::next(it);
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
    QStringList patterns = kHashIgnore + historyIgnores(root);
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
                    e.path = x.readElementText();  // names may begin or end with spaces
                } else if (inHash && kFormats.contains(n)) {
                    MhlHash h;
                    h.format = n;
                    h.action = x.attributes().value("action").toString();
                    h.value = x.readElementText().trimmed();
                    if (n != QLatin1String("c4")) h.value = h.value.toLower();  // hex; C4 IDs are base 58, case and all
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

namespace {

// A destination's path as it will be once made: its nearest existing folder resolved (links and all), the rest as given.
QString resolvedPath(const QString& path) {
    QString existing = QDir::cleanPath(QFileInfo(path).absoluteFilePath()), rest;
    while (!QFileInfo::exists(existing) && existing.contains('/')) {
        rest.prepend('/' + existing.mid(existing.lastIndexOf('/') + 1));
        existing = existing.left(existing.lastIndexOf('/'));
        if (existing.endsWith(':')) existing += '/';  // a Windows drive
    }
    QString out = QFileInfo(existing).canonicalFilePath() + rest;
    while (out.contains(QLatin1String("//"))) out.replace(QLatin1String("//"), QLatin1String("/"));
    return out;
}
bool samePath(const QString& a, const QString& b) { return QString::compare(a, b, kPathCase) == 0; }
bool within(const QString& path, const QString& folder) {
    const QString prefix = folder.endsWith('/') ? folder : folder + '/';
    return samePath(path, folder) || path.startsWith(prefix, kPathCase);
}

// Whether `folder` is empty or an earlier (perhaps interrupted) copy of this card: some of the card's files there at
// their sizes (or half written), none at another size.
bool copyOfThisCard(const QString& folder, const std::vector<Item>& card) {
    if (!QFileInfo(folder).isDir()) return true;
    Walk there;
    walk(folder, {}, Ignore(kJunk), there);
    if (there.items.empty() && there.skipped.isEmpty()) return true;
    int matches = 0;
    for (const Item& it : card) {
        if (it.dir) continue;
        const QFileInfo f(folder + '/' + it.path);
        if (f.isFile()) {
            if (f.size() != it.size) return false;
            ++matches;
        } else if (QFileInfo::exists(folder + '/' + it.path + ".montage-part")) {
            ++matches;
        }
    }
    return matches > 0;
}

// Copies `from` to `part` (written uncached, flushed to the disk, its time carried), hashing it; false with `why`.
// `bytes` is what was read; `stop` may cancel.
bool copyOne(const QString& from, const QString& part, const QDateTime& modified, std::map<QString, QString>& hashes,
             const std::set<QString>& formats, qint64& bytes, QString& why, const std::function<bool(qint64)>& tick) {
    QFile in(from), out(part);
    if (!in.open(QIODevice::ReadOnly)) {
        why = QStringLiteral("cannot be read from the card");
        return false;
    }
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered)) {
        why = QStringLiteral("cannot be written");
        return false;
    }
    writeUncached(out);
    Hasher h(formats);
    QByteArray buf(int(kChunk), Qt::Uninitialized);
    bytes = 0;
    for (;;) {
        const qint64 got = in.read(buf.data(), kChunk);
        if (got < 0) {
            why = QStringLiteral("cannot be read from the card");
            return false;
        }
        if (got == 0) break;
        h.add(buf.constData(), got);
        if (out.write(buf.constData(), got) != got) {
            why = QStringLiteral("writing failed (is the drive full?)");
            return false;
        }
        bytes += got;
        if (tick && !tick(got)) {
            why = QStringLiteral("cancelled");
            return false;
        }
    }
    const bool flushed = flushToDisk(out);
    out.setFileTime(modified, QFileDevice::FileModificationTime);
    out.close();
    if (!flushed || out.error() != QFileDevice::NoError || QFileInfo(part).size() != bytes) {
        why = QStringLiteral("writing failed (is the drive full?)");
        return false;
    }
    hashes = h.result();
    return true;
}

}  // namespace

OffloadResult offloadCard(const QString& source, const QStringList& destinations, const OffloadSettings& settings,
                          const std::function<bool(double, const QString&)>& progress) {
    OffloadResult res;
    const QString src = QDir::cleanPath(QFileInfo(source).absoluteFilePath());
    const QFileInfo card(src);
    if (!card.isDir()) {
        res.error = QStringLiteral("%1 is not a folder").arg(source);
        return res;
    }
    if (destinations.isEmpty()) {
        res.error = QStringLiteral("Choose where to copy the card");
        return res;
    }
    // The copy's name: the card's folder, or for a drive's root its volume label.
    QString name = card.fileName();
    if (name.isEmpty()) {
        const QStorageInfo volume(src);
        name = volume.displayName();
        if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.contains(':')) name = volume.name();
        if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.contains(':')) name = QStringLiteral("Card");
    }
    const QString cardPath = card.canonicalFilePath();
    // Every destination checked before anything is made: never on the card, never the card itself, never twice.
    QStringList resolved;
    for (const QString& d : destinations) {
        const QString r = resolvedPath(d), copy = r + '/' + name;
        if (within(r, cardPath)) {
            res.error = QStringLiteral("%1 is on the card itself").arg(d);
            return res;
        }
        if (within(cardPath, copy)) {
            res.error = QStringLiteral("Copying into %1 would write onto the card itself").arg(d);
            return res;
        }
        for (const QString& other : resolved)
            if (samePath(other, r)) {
                res.error = QStringLiteral("%1 is named twice").arg(d);
                return res;
            }
        resolved << r;
    }

    // What the card holds, and what its hash list says it should.
    Walk cw;
    walk(src, {}, Ignore(kJunk), cw);
    for (const QString& f : cw.unreadable) res.issues.push_back({f, QStringLiteral("This folder cannot be read (check its permissions); nothing in it was copied")});
    for (const QString& f : cw.skipped) res.issues.push_back({f, QStringLiteral("A link or special file: not copied")});
    res.files = cw.files();
    if (res.files == 0) {
        res.error = cw.unreadable.isEmpty() ? QStringLiteral("%1 holds no files").arg(source) : QStringLiteral("%1 cannot be read").arg(source);
        return res;
    }
    QString historyError;
    const std::vector<MhlGeneration> cardHistory = readMhlHistory(src, &historyError);
    if (!historyError.isEmpty()) {
        res.error = historyError;
        return res;
    }
    const std::map<QString, MhlEntry> cardRecords = referenceRecords(cardHistory);
    const Ignore cardIgnore(kHashIgnore + historyIgnores(src));
    qint64 total = 0;
    std::set<QString> onCard;
    for (const Item& it : cw.items)
        if (!it.dir) {
            total += it.size;
            onCard.insert(it.path);
        }
    res.bytes = total;
    for (const auto& [path, rec] : cardRecords)
        if (!onCard.count(path) && !cardIgnore.covers(path))
            res.issues.push_back({path, QStringLiteral("In the card's hash list but not on the card")});

    // Where each copy goes: <destination>/<name>, or "<name> 2"... when that folder holds another card.
    for (const QString& d : destinations) {
        if (!QDir().mkpath(d)) {
            res.error = QStringLiteral("Cannot use %1").arg(d);
            return res;
        }
        const QString base = QDir(d).absoluteFilePath(name);
        QString copy = base;
        for (int n = 2; !copyOfThisCard(copy, cw.items); ++n) copy = base + QStringLiteral(" %1").arg(n);
        if (!QDir().mkpath(copy)) {
            res.error = QStringLiteral("Cannot make %1").arg(copy);
            return res;
        }
        res.copies << copy;
    }
    const int n = int(res.copies.size());
    // Each copy's history: its own when it has one (an offload resumed), else the card's (carried over at the end, so a
    // stopped offload leaves none). A history that cannot be read is left alone, and no generation is added to it.
    std::vector<std::map<QString, MhlEntry>> known(static_cast<size_t>(n));
    std::vector<char> carry(static_cast<size_t>(n), 0), noList(static_cast<size_t>(n), 0);
    for (int k = 0; k < n; ++k) {
        QString err;
        const std::vector<MhlGeneration> own = readMhlHistory(res.copies[k], &err);
        if (!err.isEmpty()) {
            res.issues.push_back({QString(), QStringLiteral("%1: %2; no hash list was added to it").arg(res.copies[k], err)});
            noList[size_t(k)] = 1;
        } else if (!own.empty()) {
            known[size_t(k)] = referenceRecords(own);
        } else if (!cardHistory.empty()) {
            known[size_t(k)] = cardRecords;
            carry[size_t(k)] = 1;
        }
    }

    const double work = double(std::max<qint64>(1, total)) * (1 + (settings.verify ? n : 0));
    double done = 0;
    QString current;
    auto tick = [&](qint64 bytes) {
        done += double(bytes);
        return !progress || progress(std::min(1.0, done / work), current);
    };
    std::vector<std::map<QString, Record>> records(static_cast<size_t>(n));
    QByteArray buf(int(kChunk), Qt::Uninitialized);
    for (const Item& it : cw.items) {
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
            t.part = t.path + ".montage-part";
            if (QFileInfo::exists(t.path)) {
                t.existing = true;
                continue;
            }
            QDir().mkpath(QFileInfo(t.path).absolutePath());
            t.file = std::make_unique<QFile>(t.part);
            if (!t.file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered)) {
                res.issues.push_back({it.path, QStringLiteral("Cannot write to %1").arg(res.copies[k])});
                t.failed = true;
                t.file.reset();
            } else {
                writeUncached(*t.file);
            }
        }
        auto abandon = [&] {
            for (Target& t : targets)
                if (t.file) {
                    t.file->close();
                    QFile::remove(t.part);
                    t.file.reset();
                }
        };
        auto cancelled = [&] {
            abandon();
            res.error = QStringLiteral("Cancelled");
            return res;
        };
        QFile in(src + '/' + it.path);
        if (!in.open(QIODevice::ReadOnly)) {
            res.issues.push_back({it.path, QStringLiteral("Cannot be read from the card")});
            abandon();
            continue;
        }
        Hasher h(formats);
        bool readOk = true;
        qint64 read = 0;
        for (;;) {
            const qint64 got = in.read(buf.data(), kChunk);
            if (got < 0) {
                readOk = false;
                break;
            }
            if (got == 0) break;
            h.add(buf.constData(), got);
            read += got;
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
            if (!tick(got)) return cancelled();
        }
        if (!readOk) {
            res.issues.push_back({it.path, QStringLiteral("Cannot be read from the card")});
            abandon();
            continue;
        }
        const std::map<QString, QString> hashes = h.result();
        const QString xxh = hashes.at("xxh64");
        if (read != it.size)
            res.issues.push_back({it.path, QStringLiteral("Changed size while it was copied (%1 bytes, then %2)").arg(it.size).arg(read)});
        // Changed on the card since its hash list was made?
        bool cardChanged = false;
        if (cardRecord) {
            if (const MhlHash* rec = checkable(*cardRecord); rec && hashes.count(rec->format) && hashes.at(rec->format) != rec->value) {
                cardChanged = true;
                res.issues.push_back({it.path, QStringLiteral("Differs from the card's hash list (changed since it was hashed)")});
            } else if (!rec) {
                res.notes << QStringLiteral("%1: the card's hash list records it only in a hash Montage does not compute (xxh3, xxh128)").arg(it.path);
            }
        }
        for (int k = 0; k < n; ++k) {
            Target& t = targets[size_t(k)];
            if (t.failed) continue;
            bool wrote = false;
            if (t.file) {
                const bool flushed = flushToDisk(*t.file);
                t.file->setFileTime(it.modified, QFileDevice::FileModificationTime);
                t.file->close();
                const bool good = flushed && t.file->error() == QFileDevice::NoError && QFileInfo(t.part).size() == read;
                t.file.reset();
                if (!good || !QFile::rename(t.part, t.path)) {
                    res.issues.push_back({it.path, QStringLiteral("Writing to %1 failed (is it full?)").arg(res.copies[k])});
                    QFile::remove(t.part);
                    continue;
                }
                wrote = true;
            }
            // Read back (what was already there always is).
            if (t.existing || settings.verify) {
                std::map<QString, QString> back;
                if (!hashFile(t.path, {"xxh64"}, true, back, [&](qint64 b) { return t.existing || tick(b); })) {
                    if (progress && !progress(std::min(1.0, done / work), current)) return cancelled();
                    res.issues.push_back({it.path, QStringLiteral("Cannot read the copy in %1 back").arg(res.copies[k])});
                    if (wrote) QFile::remove(t.path);  // never leave a copy that may be bad under its name
                    continue;
                }
                if (back.at("xxh64") != xxh && t.existing) {
                    // Already there but different: a damaged earlier copy of this file (its own hash list says the
                    // card's content belongs here) is copied again; anything else is left alone.
                    const auto rec = known[size_t(k)].find(it.path);
                    const MhlHash* mine = rec == known[size_t(k)].end() ? nullptr : checkable(rec->second);
                    if (!mine || mine->format != QLatin1String("xxh64") || mine->value != xxh) {
                        res.issues.push_back({it.path, QStringLiteral("A different file is already in %1; left as it was").arg(res.copies[k])});
                        continue;
                    }
                    std::map<QString, QString> again;
                    qint64 bytes = 0;
                    QString why;
                    if (!copyOne(src + '/' + it.path, t.part, it.modified, again, {"xxh64"}, bytes, why, {}) || again.at("xxh64") != xxh ||
                        !QFile::remove(t.path) || !QFile::rename(t.part, t.path)) {
                        QFile::remove(t.part);
                        res.issues.push_back({it.path, QStringLiteral("A damaged copy in %1 could not be replaced (%2)").arg(res.copies[k], why.isEmpty() ? QStringLiteral("it changed") : why)});
                        continue;
                    }
                    std::map<QString, QString> check;
                    if (!hashFile(t.path, {"xxh64"}, true, check, {}) || check.at("xxh64") != xxh) {
                        QFile::remove(t.path);
                        res.issues.push_back({it.path, QStringLiteral("The copy in %1 does not match the card; it was removed").arg(res.copies[k])});
                        continue;
                    }
                    res.notes << QStringLiteral("%1: a damaged copy in %2 was replaced").arg(it.path, res.copies[k]);
                } else if (back.at("xxh64") != xxh) {
                    QFile::remove(t.path);
                    res.issues.push_back({it.path, QStringLiteral("The copy in %1 does not match the card; it was removed").arg(res.copies[k])});
                    continue;
                } else if (t.existing) {
                    ++res.alreadyThere;
                    if (settings.verify) tick(it.size);  // counted as if read back
                }
            }
            if (cardIgnore.covers(it.path)) continue;  // copied, but left out of hash lists
            // How this copy's hash list records it: against what the list knew, XXH64 always.
            Record r{it.path, read, QFileInfo(t.path).lastModified(), {}};
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
    for (int k = 0; k < n; ++k) {
        const QString copy = res.copies[k];
        if (noList[size_t(k)]) continue;
        // The card's history goes with the copy (whether or not a generation is added).
        if (carry[size_t(k)]) {
            QDir().mkpath(copy + "/ascmhl");
            bool copied = true;
            for (const QString& f : QDir(src + "/ascmhl").entryList(QDir::Files)) {
                QFile::remove(copy + "/ascmhl/" + f);
                copied = QFile::copy(src + "/ascmhl/" + f, copy + "/ascmhl/" + f) && copied;
            }
            if (!copied) {
                res.issues.push_back({QString(), QStringLiteral("Cannot copy the card's hash list to %1").arg(copy)});
                continue;
            }
        }
        if (!settings.mhl) continue;
        Walk there;
        walk(copy, {}, Ignore(kJunk), there);
        QString err;
        if (writeGeneration(copy, hashed(there.items, Ignore(kHashIgnore + historyIgnores(copy))), records[size_t(k)], QStringLiteral("transfer"),
                            settings, &err)
                .isEmpty())
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
    const std::map<QString, MhlEntry> recorded = referenceRecords(history);
    const Ignore ignore(kHashIgnore + historyIgnores(root));
    Walk w;
    walk(root, {}, Ignore(kJunk), w);
    for (const QString& f : w.unreadable) res.missing << f + QStringLiteral(" (cannot be read)");
    const std::vector<Item> items = hashed(w.items, ignore);
    std::set<QString> present;
    qint64 total = 0;
    for (const Item& it : items)
        if (!it.dir) {
            present.insert(it.path);
            total += it.size;
        }
    for (const auto& [path, e] : recorded)
        if (!present.count(path) && !ignore.covers(path)) res.missing << path;
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
        const MhlHash* check = rec ? checkable(*rec) : nullptr;
        if (rec && !check) {
            res.unchecked << it.path;  // and not recorded: an unchecked hash must never become the reference
            continue;
        }
        Record r{it.path, it.size, it.modified, {}};
        QString xxhAction = QStringLiteral("original");
        if (!rec) {
            res.added << it.path;
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
    // Nothing checkable is not a pass.
    res.ok = res.missing.isEmpty() && res.changed.isEmpty() && (res.verified > 0 || res.unchecked.isEmpty());
    if (writeGeneration_) {
        res.generation = writeGeneration(root, items, records, QStringLiteral("in-place"), settings, &res.error);
        if (res.generation.isEmpty()) res.ok = false;
    }
    if (progress) progress(1.0, QString());
    return res;
}

}  // namespace montage
