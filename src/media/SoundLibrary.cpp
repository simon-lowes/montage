#include "SoundLibrary.h"

#include "FieldRecorder.h"
#include "SoundSearch.h"
#include "core/Ucs.h"
#include "core/VisualIndex.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include <sqlite3.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace montage {

struct SoundLibrary::Row {
    std::string path, folder, name;
    int64_t size = 0, mtime = 0;
    std::string fingerprint;
    double duration = 0;
    int channels = 0, rate = 0;
    std::string description, catId, category, subCategory, fxName, creatorId, sourceId, keywords;
};

namespace {

// A prepared statement, finalized when it goes.
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return s != nullptr; }
    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(s, i, v.data(), int(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, int64_t v) {
        sqlite3_bind_int64(s, i, v);
        return *this;
    }
    Stmt& bind(int i, double v) {
        sqlite3_bind_double(s, i, v);
        return *this;
    }
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    // Runs it to the end and makes it ready to run again.
    bool run() {
        if (!s) return false;
        const int r = sqlite3_step(s);
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        return r == SQLITE_DONE || r == SQLITE_ROW;
    }
    std::string text(int i) const {
        const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return t ? std::string(t, size_t(sqlite3_column_bytes(s, i))) : std::string();
    }
    int64_t integer(int i) const { return sqlite3_column_int64(s, i); }
    double real(int i) const { return sqlite3_column_double(s, i); }
};

// Tag text, as UTF-8 where it reads as UTF-8 (as libraries write it) and otherwise as Latin-1.
QString tagText(const QByteArray& bytes) {
    QByteArray b = bytes;
    if (const qsizetype nul = b.indexOf('\0'); nul >= 0) b.truncate(nul);
    QString s = QString::fromUtf8(b);
    if (s.contains(QChar::ReplacementCharacter)) s = QString::fromLatin1(b);
    return s.trimmed();
}

void addWords(std::string& to, const std::string& words) {
    if (words.empty()) return;
    // Not again where it is already there as whole words.
    for (size_t at = to.find(words); at != std::string::npos; at = to.find(words, at + 1))
        if ((at == 0 || to[at - 1] == ' ') && (at + words.size() == to.size() || to[at + words.size()] == ' ')) return;
    if (!to.empty()) to += ' ';
    to += words;
}

uint32_t le16(const char* p) { return uint32_t(uint8_t(p[0])) | uint32_t(uint8_t(p[1])) << 8; }
uint32_t le32(const char* p) { return le16(p) | le16(p + 2) << 16; }
uint64_t le64(const char* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

// A WAV's format and length and its RIFF INFO words, read from its chunks (much faster than opening it with FFmpeg,
// which matters across a library of thousands). False if it is not RIFF/RF64/BW64 WAVE or has no format or data.
bool readWav(const QString& path, int& channels, int& rate, double& duration, std::map<std::string, std::string>& info) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    char head[12];
    if (file.read(head, 12) != 12) return false;
    const bool rf64 = std::memcmp(head, "RF64", 4) == 0 || std::memcmp(head, "BW64", 4) == 0;
    if ((!rf64 && std::memcmp(head, "RIFF", 4) != 0) || std::memcmp(head + 8, "WAVE", 4) != 0) return false;
    uint64_t dataSize64 = 0, dataSize = 0;
    uint32_t byteRate = 0;
    bool haveFormat = false, haveData = false;
    while (!file.atEnd()) {
        char ch[8];
        if (file.read(ch, 8) != 8) break;
        uint64_t size = le32(ch + 4);
        const qint64 body = file.pos();
        if (std::memcmp(ch, "ds64", 4) == 0 && size >= 16) {
            const QByteArray d = file.read(16);
            if (d.size() == 16) dataSize64 = le64(d.constData() + 8);
        } else if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            const QByteArray d = file.read(16);
            if (d.size() == 16) {
                channels = int(le16(d.constData() + 2));
                rate = int(le32(d.constData() + 4));
                byteRate = le32(d.constData() + 8);
                haveFormat = true;
            }
        } else if (std::memcmp(ch, "data", 4) == 0) {
            if (rf64 && size == 0xFFFFFFFFu) size = dataSize64;
            // A recorder stopped before it could write the size leaves 0 (or too much): the rest of the file.
            const uint64_t rest = uint64_t(std::max<qint64>(0, file.size() - body));
            if (size == 0 || size > rest) size = rest;
            dataSize = size;
            haveData = true;
        } else if (std::memcmp(ch, "LIST", 4) == 0 && size >= 4 && size < (1u << 20)) {
            const QByteArray list = file.read(qint64(size));
            if (list.startsWith("INFO")) {
                for (qsizetype at = 4; at + 8 <= list.size();) {
                    const uint32_t n = le32(list.constData() + at + 4);
                    if (at + 8 + qsizetype(n) > list.size()) break;
                    info[list.mid(at, 4).toStdString()] = tagText(list.mid(at + 8, n)).toStdString();
                    at += 8 + qsizetype(n) + (n & 1);
                }
            }
        }
        if (!file.seek(body + qint64(size + (size & 1)))) break;  // chunks are padded to even sizes
    }
    if (!haveFormat || !haveData || rate <= 0) return false;
    duration = byteRate > 0 ? double(dataSize) / byteRate : 0;
    return true;
}

// Any other sound file through FFmpeg: its main audio stream and its container's and stream's tags (lower-case keys).
bool probeSound(const std::string& path, int& channels, int& rate, double& duration, std::map<std::string, std::string>& tags) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    bool ok = false;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (si >= 0) {
            const AVStream* st = fmt->streams[si];
            channels = st->codecpar->ch_layout.nb_channels;
            rate = st->codecpar->sample_rate;
            duration = fmt->duration > 0                                    ? double(fmt->duration) / AV_TIME_BASE
                       : st->duration > 0 && st->time_base.den > 0 ? double(st->duration) * av_q2d(st->time_base)
                                                                    : 0;
            for (const AVDictionary* m : {static_cast<const AVDictionary*>(fmt->metadata), static_cast<const AVDictionary*>(st->metadata)}) {
                const AVDictionaryEntry* e = nullptr;
                while ((e = av_dict_get(m, "", e, AV_DICT_IGNORE_SUFFIX))) {
                    std::string key = QString::fromUtf8(e->key).toLower().toStdString();
                    if (!tags.count(key)) tags[key] = QString::fromUtf8(e->value).trimmed().toStdString();
                }
            }
            ok = rate > 0;
        }
    }
    avformat_close_input(&fmt);
    return ok;
}

// Size and the bytes at each end: what a renamed or moved file keeps.
std::string fingerprint(const QString& path, int64_t size) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    constexpr qint64 kEnd = 64 * 1024;
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(file.read(kEnd));
    if (size > kEnd && file.seek(std::max(kEnd, size - kEnd))) h.addData(file.read(kEnd));
    return std::to_string(size) + ':' + h.result().toHex().toStdString();
}

// An FTS5 query: every word's start (each word split as the index splits text: at anything not a letter or digit).
std::string ftsQuery(const std::string& words) {
    QStringList terms;
    QString term;
    const QString s = QString::fromStdString(words);
    for (qsizetype i = 0; i <= s.size(); ++i) {
        if (i < s.size() && (s[i].isLetterOrNumber() || s[i].isMark())) {
            term += s[i];
        } else if (!term.isEmpty()) {
            terms << QStringLiteral("\"%1\"*").arg(term);
            term.clear();
        }
    }
    return terms.join(' ').toStdString();
}

constexpr const char* kColumns =
    "id, path, name, folder, size, duration, channels, rate, description, catid, category, subcategory, fxname, creator, "
    "source, keywords, heard IS NOT NULL";

SoundLibraryEntry entryFrom(const Stmt& q) {
    SoundLibraryEntry e;
    e.id = q.integer(0);
    e.path = q.text(1);
    e.name = q.text(2);
    e.folder = q.text(3);
    e.size = q.integer(4);
    e.duration = q.real(5);
    e.channels = int(q.integer(6));
    e.sampleRate = int(q.integer(7));
    e.description = q.text(8);
    e.catId = q.text(9);
    e.category = q.text(10);
    e.subCategory = q.text(11);
    e.fxName = q.text(12);
    e.creatorId = q.text(13);
    e.sourceId = q.text(14);
    e.keywords = q.text(15);
    e.heard = q.integer(16) != 0;
    return e;
}

}  // namespace

bool isSoundLibraryFile(const std::string& path) {
    static const std::set<QString> kinds = {"wav",  "bwf", "rf64", "w64", "aif", "aiff", "aifc", "flac", "mp3",
                                            "ogg",  "oga", "opus", "m4a", "caf", "wv"};
    return kinds.count(QFileInfo(QString::fromStdString(path)).suffix().toLower()) > 0;
}

SoundLibrary::SoundLibrary() = default;

SoundLibrary::~SoundLibrary() {
    if (db_) sqlite3_close_v2(db_);
}

std::string SoundLibrary::defaultPath() {
    const QString env = qEnvironmentVariable("MONTAGE_SOUND_LIBRARY");
    if (!env.isEmpty()) return env.toStdString();
    return (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sound-library.sqlite")).toStdString();
}

std::shared_ptr<SoundLibrary> SoundLibrary::shared() {
    static std::mutex m;
    static std::shared_ptr<SoundLibrary> library;
    std::lock_guard lock(m);
    if (!library) {
        auto l = std::make_shared<SoundLibrary>();
        if (l->open(defaultPath())) library = l;
    }
    return library;
}

bool SoundLibrary::exec(const char* sql) const {
    char* message = nullptr;
    const bool ok = sqlite3_exec(db_, sql, nullptr, nullptr, &message) == SQLITE_OK;
    sqlite3_free(message);
    return ok;
}

bool SoundLibrary::open(const std::string& file, std::string* error) {
    std::lock_guard lock(mutex_);
    if (db_) sqlite3_close_v2(db_), db_ = nullptr;
    QDir().mkpath(QFileInfo(QString::fromStdString(file)).absolutePath());
    if (sqlite3_open_v2(file.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (error) *error = db_ ? sqlite3_errmsg(db_) : "cannot open " + file;
        if (db_) sqlite3_close_v2(db_), db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    // `heard`: CLAP's index of the sound (core/VisualIndex.h as JSON), NULL until it has listened.
    const bool ok = exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
                         "CREATE TABLE IF NOT EXISTS folders(path TEXT PRIMARY KEY);"
                         "CREATE TABLE IF NOT EXISTS sounds(id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, folder TEXT, "
                         "name TEXT, size INTEGER, mtime INTEGER, fingerprint TEXT, duration REAL, channels INTEGER, "
                         "rate INTEGER, description TEXT, catid TEXT, category TEXT, subcategory TEXT, fxname TEXT, "
                         "creator TEXT, source TEXT, keywords TEXT, heard TEXT);"
                         "CREATE INDEX IF NOT EXISTS sounds_fingerprint ON sounds(fingerprint);"
                         "CREATE INDEX IF NOT EXISTS sounds_folder ON sounds(folder);"
                         "CREATE VIRTUAL TABLE IF NOT EXISTS sounds_fts USING fts5(name, description, catid, category, "
                         "subcategory, fxname, keywords, tokenize='unicode61 remove_diacritics 2');"
                         "PRAGMA user_version=1;");
    if (!ok) {
        if (error) *error = sqlite3_errmsg(db_);
        sqlite3_close_v2(db_), db_ = nullptr;
        return false;
    }
    file_ = file;
    return true;
}

bool SoundLibrary::isOpen() const {
    std::lock_guard lock(mutex_);
    return db_ != nullptr;
}

std::string SoundLibrary::file() const {
    std::lock_guard lock(mutex_);
    return file_;
}

std::vector<std::string> SoundLibrary::folders() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    if (!db_) return out;
    Stmt q(db_, "SELECT path FROM folders ORDER BY path");
    while (q.row()) out.push_back(q.text(0));
    return out;
}

bool SoundLibrary::addFolder(const std::string& folder, std::string* error) {
    const QFileInfo info(QString::fromStdString(folder));
    if (!info.isDir()) {
        if (error) *error = "not a folder: " + folder;
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!db_) return false;
    Stmt q(db_, "INSERT OR IGNORE INTO folders(path) VALUES(?)");
    return q.bind(1, QDir::cleanPath(info.absoluteFilePath()).toStdString()).run();
}

bool SoundLibrary::removeFolder(const std::string& folder) {
    std::lock_guard scanning(scanMutex_);  // (not under a scan's feet)
    std::lock_guard lock(mutex_);
    if (!db_) return false;
    const std::string path = QDir::cleanPath(QFileInfo(QString::fromStdString(folder)).absoluteFilePath()).toStdString();
    if (!exec("BEGIN IMMEDIATE")) return false;
    Stmt fts(db_, "DELETE FROM sounds_fts WHERE rowid IN (SELECT id FROM sounds WHERE folder = ?)");
    Stmt sounds(db_, "DELETE FROM sounds WHERE folder = ?");
    Stmt q(db_, "DELETE FROM folders WHERE path = ?");
    const bool ok = fts.bind(1, path).run() && sounds.bind(1, path).run() && q.bind(1, path).run() && sqlite3_changes(db_) > 0;
    exec(ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

bool SoundLibrary::readFile(const std::string& path, Row& r) const {
    const QString qpath = QString::fromStdString(path);
    std::map<std::string, std::string> tags;  // lower-case keys
    const QString suffix = QFileInfo(qpath).suffix().toLower();
    std::map<std::string, std::string> info;
    if ((suffix == "wav" || suffix == "bwf" || suffix == "rf64") && readWav(qpath, r.channels, r.rate, r.duration, info)) {
        static const std::map<std::string, std::string> kInfo = {{"INAM", "title"},   {"ICMT", "comment"}, {"IKEY", "keywords"},
                                                                 {"IGNR", "genre"},   {"ISBJ", "subject"}, {"IPRD", "album"},
                                                                 {"IART", "artist"}};
        for (const auto& [id, text] : info)
            if (const auto it = kInfo.find(id); it != kInfo.end()) tags[it->second] = text;
    } else if (!probeSound(path, r.channels, r.rate, r.duration, tags)) {
        return false;
    }
    FieldRecording f;
    readFieldRecording(path, f);
    const auto user = [&](const char* key) {
        const auto it = f.user.find(key);
        return it == f.user.end() ? std::string() : it->second;
    };
    const auto tag = [&](const char* key) {
        const auto it = tags.find(key);
        return it == tags.end() ? std::string() : it->second;
    };
    const auto first = [](std::initializer_list<std::string> v) {
        for (const std::string& s : v)
            if (!s.empty()) return s;
        return std::string();
    };

    r.description = first({f.description, user("DESCRIPTION"), tag("description"), tag("comment")});
    UcsName ucs;
    const bool named = parseUcsFileName(QFileInfo(qpath).fileName().toStdString(), ucs);
    const UcsCategory* c = named ? ucs.category : ucsCategory(user("CATID"));
    if (c) {
        r.catId = c->catId;
        r.category = c->category;
        r.subCategory = c->subCategory;
    } else {
        r.category = user("CATEGORY");
        r.subCategory = user("SUBCATEGORY");
    }
    r.fxName = first({ucs.fxName, user("FXNAME"), tag("title")});
    r.creatorId = first({ucs.creatorId, user("CREATORID")});
    r.sourceId = first({ucs.sourceId, user("SOURCEID")});
    r.keywords.clear();
    for (const std::string& w : {ucs.userCategory, ucs.userData, user("KEYWORDS"), user("LIBRARY"), user("TRACKTITLE"),
                                 user("SHOW"), user("NOTES"), user("CATEGORYFULL"), user("MICROPHONE"), user("RECTYPE"),
                                 tag("keywords"), tag("genre"), tag("subject"), tag("album"), tag("artist")})
        addWords(r.keywords, w);
    return true;
}

int64_t SoundLibrary::put(const Row& r, int64_t id) {
    if (id == 0) {
        Stmt q(db_, "INSERT INTO sounds(path, folder, name, size, mtime, fingerprint, duration, channels, rate, description, "
                    "catid, category, subcategory, fxname, creator, source, keywords) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        q.bind(1, r.path).bind(2, r.folder).bind(3, r.name).bind(4, r.size).bind(5, r.mtime).bind(6, r.fingerprint);
        q.bind(7, r.duration).bind(8, int64_t(r.channels)).bind(9, int64_t(r.rate)).bind(10, r.description).bind(11, r.catId);
        q.bind(12, r.category).bind(13, r.subCategory).bind(14, r.fxName).bind(15, r.creatorId).bind(16, r.sourceId).bind(17, r.keywords);
        if (!q.run()) return 0;
        id = sqlite3_last_insert_rowid(db_);
    } else {
        Stmt q(db_, "UPDATE sounds SET path=?, folder=?, name=?, size=?, mtime=?, fingerprint=?, duration=?, channels=?, rate=?, "
                    "description=?, catid=?, category=?, subcategory=?, fxname=?, creator=?, source=?, keywords=? WHERE id=?");
        q.bind(1, r.path).bind(2, r.folder).bind(3, r.name).bind(4, r.size).bind(5, r.mtime).bind(6, r.fingerprint);
        q.bind(7, r.duration).bind(8, int64_t(r.channels)).bind(9, int64_t(r.rate)).bind(10, r.description).bind(11, r.catId);
        q.bind(12, r.category).bind(13, r.subCategory).bind(14, r.fxName).bind(15, r.creatorId).bind(16, r.sourceId).bind(17, r.keywords);
        q.bind(18, id);
        if (!q.run() || sqlite3_changes(db_) != 1) return 0;
        Stmt del(db_, "DELETE FROM sounds_fts WHERE rowid = ?");
        if (!del.bind(1, id).run()) return 0;
    }
    Stmt fts(db_, "INSERT INTO sounds_fts(rowid, name, description, catid, category, subcategory, fxname, keywords) VALUES(?,?,?,?,?,?,?,?)");
    const std::string name = QFileInfo(QString::fromStdString(r.name)).completeBaseName().toStdString();
    fts.bind(1, id).bind(2, name).bind(3, r.description).bind(4, r.catId).bind(5, r.category).bind(6, r.subCategory);
    fts.bind(7, r.fxName).bind(8, r.keywords + (r.creatorId.empty() ? "" : " " + r.creatorId) + (r.sourceId.empty() ? "" : " " + r.sourceId));
    return fts.run() ? id : 0;
}

SoundLibraryScan SoundLibrary::scan(const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    std::lock_guard scanning(scanMutex_);
    const auto started = std::chrono::steady_clock::now();
    SoundLibraryScan result;
    struct Known {
        int64_t id, size, mtime;
        std::string fingerprint, folder;
    };
    std::vector<std::string> roots;
    std::unordered_map<std::string, Known> known;
    {
        std::lock_guard lock(mutex_);
        if (!db_) return result;
        Stmt f(db_, "SELECT path FROM folders");
        while (f.row()) roots.push_back(f.text(0));
        Stmt q(db_, "SELECT id, path, size, mtime, fingerprint, folder FROM sounds");
        while (q.row()) known[q.text(1)] = {q.integer(0), q.integer(2), q.integer(3), q.text(4), q.text(5)};
    }
    // The deepest folder first: a file in a folder inside another library folder is that folder's.
    std::sort(roots.begin(), roots.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size() || (a.size() == b.size() && a < b); });

    // What is on disk now. A folder that is not there, or lists nothing while it has sounds (a drive not mounted, a
    // mount point without its share), keeps its sounds.
    struct OnDisk {
        std::string path, folder;
        int64_t size, mtime;
    };
    std::vector<OnDisk> files;
    std::set<std::string> online, seen;
    std::map<std::string, int> had;
    for (const auto& [path, k] : known) ++had[k.folder];
    for (const std::string& root : roots) {
        const QString dir = QString::fromStdString(root);
        if (!QFileInfo(dir).isDir()) continue;
        const size_t before = files.size();
        QDirIterator it(dir, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        int listed = 0;
        while (it.hasNext()) {
            const QString p = it.next();
            ++listed;
            const std::string path = QDir::cleanPath(p).toStdString();
            if (!isSoundLibraryFile(path) || !seen.insert(path).second) continue;
            const QFileInfo info = it.fileInfo();
            files.push_back({path, root, int64_t(info.size()), int64_t(info.lastModified().toMSecsSinceEpoch())});
        }
        if (listed == 0 && had[root] > 0) {
            files.resize(before);
            continue;
        }
        online.insert(root);
    }
    result.files = int(files.size());

    // Read what is new or changed (outside the lock, so searching goes on meanwhile).
    struct Read {
        Row row;
        int64_t id = 0;        // the entry it updates (0: a new one)
        bool changed = false;  // its sound changed, so what CLAP heard goes
        bool moved = false;    // a renamed or moved file's entry
    };
    std::vector<Read> reads;
    bool cancelled = false;
    std::vector<Read> fresh;  // new paths, perhaps renamed or moved files
    for (size_t i = 0; i < files.size(); ++i) {
        if (cancel && *cancel) {
            cancelled = true;
            break;
        }
        if (progress && (i % 64) == 0) progress(double(i) / double(files.size()));
        const OnDisk& d = files[i];
        const auto k = known.find(d.path);
        if (k != known.end() && k->second.size == d.size && k->second.mtime == d.mtime && k->second.folder == d.folder) continue;
        Read r;
        r.row.path = d.path;
        r.row.folder = d.folder;
        r.row.name = QFileInfo(QString::fromStdString(d.path)).fileName().toStdString();
        r.row.size = d.size;
        r.row.mtime = d.mtime;
        r.row.fingerprint = fingerprint(QString::fromStdString(d.path), d.size);
        if (!readFile(d.path, r.row)) continue;
        if (k != known.end()) {
            r.id = k->second.id;
            r.changed = k->second.fingerprint != r.row.fingerprint;
            reads.push_back(std::move(r));
        } else {
            fresh.push_back(std::move(r));
        }
    }

    // Entries whose file has gone from a folder that is there: renamed or moved, if a new file has its fingerprint;
    // else gone, as are the sounds of folders no longer in the library. (A cancelled scan, not having seen every
    // file, neither adds nor drops any.)
    std::set<int64_t> gone;
    if (!cancelled) {
        std::unordered_multimap<std::string, int64_t> vanished;
        for (const auto& [path, k] : known) {
            if (online.count(k.folder) && !seen.count(path)) vanished.emplace(k.fingerprint, k.id), gone.insert(k.id);
            if (std::find(roots.begin(), roots.end(), k.folder) == roots.end()) gone.insert(k.id);
        }
        for (Read& r : fresh)
            if (const auto it = vanished.find(r.row.fingerprint); it != vanished.end() && !r.row.fingerprint.empty()) {
                r.id = it->second;
                r.moved = true;
                gone.erase(r.id);
                vanished.erase(it);
            }
    } else {
        fresh.clear();
    }

    // Written a few hundred at a time, so searching is held up only briefly.
    std::vector<std::function<bool(SoundLibraryScan&)>> steps;
    for (const Read& r : reads)
        steps.push_back([this, &r](SoundLibraryScan& n) {
            if (!put(r.row, r.id)) return false;
            if (r.changed) {
                Stmt q(db_, "UPDATE sounds SET heard = NULL WHERE id = ?");
                if (!q.bind(1, r.id).run()) return false;
            }
            ++n.updated;
            return true;
        });
    for (const Read& r : fresh)
        steps.push_back([this, &r](SoundLibraryScan& n) {
            if (!put(r.row, r.id)) return false;
            ++(r.moved ? n.moved : n.added);
            return true;
        });
    for (int64_t id : gone)
        steps.push_back([this, id](SoundLibraryScan& n) {
            Stmt fts(db_, "DELETE FROM sounds_fts WHERE rowid = ?");
            Stmt q(db_, "DELETE FROM sounds WHERE id = ?");
            if (!fts.bind(1, id).run() || !q.bind(1, id).run()) return false;
            ++n.removed;
            return true;
        });
    for (size_t at = 0; at < steps.size();) {
        std::lock_guard lock(mutex_);
        if (!db_) break;
        if (!exec("BEGIN IMMEDIATE")) {
            result.error = sqlite3_errmsg(db_);
            break;
        }
        SoundLibraryScan batch;
        const size_t end = std::min(steps.size(), at + 400);
        bool ok = true;
        for (; at < end && ok; ++at) ok = steps[at](batch);
        if (!ok || !exec("COMMIT")) {
            result.error = sqlite3_errmsg(db_);
            exec("ROLLBACK");
            break;
        }
        result.added += batch.added, result.updated += batch.updated, result.moved += batch.moved, result.removed += batch.removed;
    }
    if (progress) progress(1);
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

int SoundLibrary::count() const {
    std::lock_guard lock(mutex_);
    if (!db_) return 0;
    Stmt q(db_, "SELECT count(*) FROM sounds");
    return q.row() ? int(q.integer(0)) : 0;
}

std::vector<SoundLibraryEntry> SoundLibrary::select(const std::string& where, const std::vector<std::string>& args, size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<SoundLibraryEntry> out;
    if (!db_) return out;
    const std::string sql = std::string("SELECT ") + kColumns + " FROM sounds " + where + " LIMIT ?";
    Stmt q(db_, sql.c_str());
    if (!q) return out;
    int i = 1;
    for (const std::string& a : args) q.bind(i++, a);
    q.bind(i, int64_t(std::min<size_t>(limit, size_t(INT64_MAX))));
    while (q.row()) out.push_back(entryFrom(q));
    return out;
}

std::vector<SoundLibraryEntry> SoundLibrary::search(const std::string& words, size_t limit) const {
    const std::string query = ftsQuery(words);
    if (query.empty()) return select("ORDER BY name COLLATE NOCASE, path", {}, limit);
    return select("WHERE id IN (SELECT rowid FROM sounds_fts WHERE sounds_fts MATCH ?) ORDER BY name COLLATE NOCASE, path", {query}, limit);
}

std::optional<SoundLibraryEntry> SoundLibrary::entry(int64_t id) const {
    const auto v = select("WHERE id = ?", {std::to_string(id)}, 1);
    if (v.empty()) return std::nullopt;
    return v.front();
}

std::optional<SoundLibraryEntry> SoundLibrary::entryForPath(const std::string& path) const {
    const auto v = select("WHERE path = ?", {QDir::cleanPath(QString::fromStdString(path)).toStdString()}, 1);
    if (v.empty()) return std::nullopt;
    return v.front();
}

int SoundLibrary::unheard() const {
    std::lock_guard lock(mutex_);
    if (!db_) return 0;
    Stmt q(db_, "SELECT id FROM sounds WHERE heard IS NULL");
    int n = 0;
    while (q.row()) n += unheardable_.count(q.integer(0)) ? 0 : 1;
    return n;
}

int SoundLibrary::listen(const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    if (!SoundModel::load(error)) return -1;
    struct Todo {
        int64_t id;
        std::string path;
        double duration;
    };
    std::vector<Todo> todo;
    {
        std::lock_guard lock(mutex_);
        if (!db_) return 0;
        Stmt q(db_, "SELECT id, path, duration FROM sounds WHERE heard IS NULL ORDER BY id");
        while (q.row()) todo.push_back({q.integer(0), q.text(1), q.real(2)});
    }
    int heard = 0;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (cancel && *cancel) break;
        if (progress) progress(int(i), int(todo.size()));
        {
            std::lock_guard lock(mutex_);
            if (unheardable_.count(todo[i].id)) continue;
        }
        VisualIndex index;
        // A file that cannot be heard (not there, as on a drive not mounted, or unreadable, or the model failing) is
        // left unheard, and not tried again while this library is open.
        const bool ok = indexSound(todo[i].path, todo[i].duration, index, 0, {}, cancel);
        if (cancel && *cancel) break;
        std::lock_guard lock(mutex_);
        if (!db_) break;
        if (!ok) {
            unheardable_.insert(todo[i].id);
            continue;
        }
        Stmt q(db_, "UPDATE sounds SET heard = ? WHERE id = ?");
        q.bind(1, visualIndexToJson(index)).bind(2, todo[i].id).run();
        ++heard;
    }
    if (progress) progress(heard, int(todo.size()));
    return heard;
}

namespace {

// The best window of one heard sound for any of the query embeddings.
SoundLibrary::Match bestWindow(const VisualIndex& index, const std::vector<std::vector<float>>& queries) {
    SoundLibrary::Match m;
    m.score = -2;
    for (size_t i = 0; i < index.samples.size(); ++i)
        for (const std::vector<float>& q : queries)
            if (const float s = index.similarity(i, q); s > m.score) m.score = s, m.at = std::max(0.0, index.samples[i].time - index.span / 2);
    return m;
}

std::vector<float> embedding(const VisualIndex::Sample& s) {
    std::vector<float> e(s.values.size());
    double len = 0;
    for (size_t k = 0; k < e.size(); ++k) e[k] = float(s.values[k]) * s.scale, len += double(e[k]) * e[k];
    if (len > 0)
        for (float& v : e) v = float(v / std::sqrt(len));
    return e;
}

}  // namespace

std::vector<SoundLibrary::Match> SoundLibrary::nearest(const std::vector<std::vector<float>>& queries, int64_t leaveOut, size_t limit) const {
    std::vector<Match> out;
    if (queries.empty()) return out;
    std::vector<std::pair<int64_t, float>> scores;
    std::map<int64_t, double> at;
    // Read a few hundred at a time and compared outside the lock, so the library stays free meanwhile.
    for (int64_t after = INT64_MIN;;) {
        std::vector<std::pair<int64_t, std::string>> page;
        {
            std::lock_guard lock(mutex_);
            if (!db_) return out;
            Stmt q(db_, "SELECT id, heard FROM sounds WHERE heard IS NOT NULL AND id > ? ORDER BY id LIMIT 256");
            q.bind(1, after);
            while (q.row()) page.emplace_back(q.integer(0), q.text(1));
        }
        if (page.empty()) break;
        after = page.back().first;
        for (const auto& [id, json] : page) {
            VisualIndex index;
            if (id == leaveOut || !visualIndexFromJson(json, index) || index.samples.empty()) continue;
            const Match m = bestWindow(index, queries);
            scores.push_back({id, m.score});
            at[id] = m.at;
        }
    }
    std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
    if (scores.size() > limit) scores.resize(limit);
    for (const auto& [id, score] : scores) {
        if (const auto e = entry(id)) out.push_back({*e, score, at[id]});
    }
    return out;
}

std::vector<SoundLibrary::Match> SoundLibrary::searchByMeaning(const std::vector<float>& query, size_t limit) const {
    if (query.empty()) return {};
    return nearest({query}, 0, limit);
}

std::vector<SoundLibrary::Match> SoundLibrary::moreLike(int64_t id, size_t limit) const {
    VisualIndex index;
    {
        std::lock_guard lock(mutex_);
        if (!db_) return {};
        Stmt q(db_, "SELECT heard FROM sounds WHERE id = ? AND heard IS NOT NULL");
        q.bind(1, id);
        if (!q.row() || !visualIndexFromJson(q.text(0), index)) return {};
    }
    // Each of its windows (at most 16, spread through it) as a query: a sound like any part of it.
    std::vector<std::vector<float>> queries;
    const size_t n = index.samples.size(), take = std::min<size_t>(n, 16);
    for (size_t i = 0; i < take; ++i) queries.push_back(embedding(index.samples[take > 1 ? i * (n - 1) / (take - 1) : 0]));
    return nearest(queries, id, limit);
}

}  // namespace montage
