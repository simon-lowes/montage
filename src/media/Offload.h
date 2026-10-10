// Montage — card offload (Resolve's Clone tool, Silverstack, ShotPut Pro, Hedge): a camera card copied to one or more
// destinations, each file read once and written to all of them while it is hashed, every copy read back and compared,
// and an ASC MHL generation (the ASC's Media Hash List v2.0, XXH64) written into each copy so the files can be checked
// again at any later stage by any tool that reads ASC MHL. A folder can also be verified against its ASC MHL history,
// whoever wrote it, with a new generation recording what was found. Premiere, Final Cut and Media Composer leave
// offloading to other applications.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace montage {

// XXH64 (seed 0), ASC MHL's usual hash, fed in pieces.
class Xxh64 {
public:
    Xxh64();
    void update(const void* data, size_t size);
    uint64_t digest() const;
    static uint64_t of(const void* data, size_t size);

private:
    uint64_t v_[4];
    unsigned char buf_[32];
    size_t buffered_ = 0;
    uint64_t total_ = 0;
};
std::string xxh64Hex(uint64_t h);  // 16 lowercase hex digits, as ASC MHL writes it
// The C4 ID (SMPTE ST 2114) of some bytes: their SHA-512 in base 58, "c4" and 88 characters. ASC MHL's chain file
// names each generation by it.
std::string c4Id(const QByteArray& data);

// A file as one ASC MHL generation records it.
struct MhlHash {
    QString format;  // "xxh64", "md5", "sha1", "c4", "xxh3", "xxh128"
    QString value;
    QString action;  // "original", "verified" or "failed"
};
struct MhlEntry {
    QString path;  // relative to the folder, '/' between names
    qint64 size = -1;
    QString modified;  // xs:dateTime, as written
    std::vector<MhlHash> hashes;
};
struct MhlGeneration {
    int number = 0;
    QString file;  // in the ascmhl folder
    QString created, tool, process;
    std::vector<MhlEntry> entries;
};
// The ASC MHL history of `root` (its ascmhl folder), oldest generation first; empty when it has none, or with
// `error` when it cannot be read.
std::vector<MhlGeneration> readMhlHistory(const QString& root, QString* error = nullptr);

struct OffloadSettings {
    bool verify = true;  // read every copy back and compare it with the card
    bool mhl = true;     // write an ASC MHL generation into each copy
    QString author, location, comment;  // the MHL's creator information
};

struct OffloadIssue {
    QString path;  // the file, relative to the card
    QString problem;
};

struct OffloadResult {
    bool ok = false;     // every file copied and matching, at every destination
    QString error;       // why it stopped, when it did
    QStringList copies;  // the card's folder at each destination
    int files = 0;       // files on the card
    qint64 bytes = 0;
    int alreadyThere = 0;  // files a destination already had, identical (counted once per destination)
    std::vector<OffloadIssue> issues;
};

// Copies the folder `source` (a camera card, or any folder) into each of `destinations` as <destination>/<its name>,
// reading each file once and writing it to every destination while hashing it (XXH64), carrying modification times.
// A file a destination already has is hashed: identical, it is kept (an interrupted offload resumes); different, it
// is reported and left alone. With `verify`, each copy is read back (past the system's cache where it can) and
// compared. A card with an ASC MHL history is checked against it (a file changed since it was hashed, or gone, is
// reported) and the history goes with each copy. With `mhl`, each copy gets a generation (process "transfer")
// recording every file that arrived intact: verified when the history knew it, else original. Junk the operating
// systems leave (.DS_Store, ._ files, Thumbs.db, .Spotlight-V100, .Trashes, .fseventsd) is not copied. `progress`
// hears the fraction done and the file at hand, and may return false to stop.
OffloadResult offloadCard(const QString& source, const QStringList& destinations, const OffloadSettings& settings = {},
                          const std::function<bool(double, const QString&)>& progress = {});

struct MhlVerifyResult {
    bool ok = false;  // every file the history records is there and matches
    QString error;
    int verified = 0;
    QStringList missing, changed, added;
    QStringList unchecked;  // recorded only in a hash Montage does not compute (xxh3, xxh128)
    QString generation;     // the generation written, if one was
};
// Checks `root` against its ASC MHL history: the latest record of each file is compared with the file as it is now
// (missing, changed), and files the history does not know are listed as added. With `writeGeneration`, a generation
// (process "in-place") records every file as verified, failed or original; a folder without a history gets its
// first generation that way (with no history and no generation it is an error).
MhlVerifyResult verifyMhl(const QString& root, bool writeGeneration, const OffloadSettings& settings = {},
                          const std::function<bool(double, const QString&)>& progress = {});

}  // namespace montage
