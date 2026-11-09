// Montage — AV2 through dav2d and AVM (see Av2.h).
#include "Av2.h"

#include "render/ColorSpace.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <climits>
#include <cstddef>
#include <cstring>
#include <map>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#ifdef MONTAGE_WITH_AV2
#include <avm/avm_encoder.h>
#include <avm/avmcx.h>
// dav2d's headers.h defines its tile layout struct inside the sequence header and names it again in the frame header,
// which C reads as the same struct and C++ as a second one that is never completed. The same struct at file scope
// first lets the frame header's use find it; it is checked against the header's own below.
struct Dav2dTileInfo {
    uint8_t uniform;
    uint8_t min_log2_cols, max_log2_cols, log2_cols, cols;
    uint8_t min_log2_rows, max_log2_rows, log2_rows, rows;
    uint16_t col_start_sb[64 + 1];
    uint16_t row_start_sb[64 + 1];
};
#include <dav2d/dav2d.h>
#include <type_traits>
using Dav2dSequenceTileInfo = decltype(Dav2dSequenceHeader::tiling.t);
static_assert(DAV2D_MAX_TILE_COLS == 64 && DAV2D_MAX_TILE_ROWS == 64, "dav2d's tile limits changed: update Dav2dTileInfo above");
static_assert(sizeof(Dav2dSequenceTileInfo) == sizeof(Dav2dTileInfo) && alignof(Dav2dSequenceTileInfo) == alignof(Dav2dTileInfo) &&
                  offsetof(Dav2dSequenceTileInfo, row_start_sb) == offsetof(Dav2dTileInfo, row_start_sb),
              "dav2d's tile layout changed: update Dav2dTileInfo above");
static_assert(std::is_same_v<decltype(Dav2dFrameHeader::tiling.t), Dav2dTileInfo>, "dav2d's frame header names another tile struct");
#endif

namespace montage {

namespace {

// AV2's OBU types (as dav2d and AVM number them).
constexpr int kObuSequenceHeader = 1, kObuTemporalDelimiter = 2, kObuClosedLoopKey = 4, kObuMetadata = 8, kObuMetadataGroup = 9,
              kObuContentInterpretation = 24, kObuPadding = 25;
// The OBUs that code a frame: the first of them in a temporal unit says whether it can be decoded from.
[[maybe_unused]] bool codesFrame(int type) { return (type >= 4 && type <= 7) || (type >= 10 && type <= 14) || type == 19 || type == 21; }

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

[[maybe_unused]] uint32_t rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) << 32 | rd32(p + 4); }
[[maybe_unused]] uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
[[maybe_unused]] uint64_t le64(const uint8_t* p) { return uint64_t(le32(p + 4)) << 32 | le32(p); }

// A leb128 at `p` (at most `n` bytes): its value and length, or 0 when it is cut off or too long.
int leb128(const uint8_t* p, size_t n, uint64_t& v) {
    v = 0;
    for (int i = 0; i < 8 && size_t(i) < n; ++i) {
        v |= uint64_t(p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) return i + 1;
    }
    return 0;
}

// One OBU of a length-delimited run: where it starts (its size field), its whole length and its type.
struct ObuAt {
    size_t at = 0, size = 0;
    int type = 0;
};
// The OBUs of `data` (each a leb128 size, then the OBU: its header and payload); false if one runs past the end.
[[maybe_unused]] bool splitObus(const uint8_t* data, size_t n, std::vector<ObuAt>& out) {
    out.clear();
    for (size_t at = 0; at < n;) {
        uint64_t size = 0;
        const int l = leb128(data + at, n - at, size);
        if (!l || size < 1 || size > n - at - size_t(l)) return false;
        out.push_back({at, size_t(l) + size_t(size), (data[at + size_t(l)] >> 2) & 31});
        at += size_t(l) + size_t(size);
    }
    return true;
}

const std::vector<uint8_t> kTemporalDelimiter{0x01, uint8_t(kObuTemporalDelimiter << 2)};

std::string lowerSuffix(const std::string& file) { return QFileInfo(QString::fromStdString(file)).suffix().toLower().toStdString(); }

// ---- MP4 boxes ----

struct Box {
    std::string type;
    qint64 at = 0, body = 0, end = 0;  // the box, its contents, past it
};
// The boxes between `from` and `to` of `f`.
std::vector<Box> boxesIn(QFile& f, qint64 from, qint64 to) {
    std::vector<Box> out;
    for (qint64 at = from; at + 8 <= to;) {
        uint8_t h[16];
        if (!f.seek(at) || f.read(reinterpret_cast<char*>(h), 8) != 8) break;
        uint64_t size = rd32(h);
        qint64 body = at + 8;
        if (size == 1) {
            if (f.read(reinterpret_cast<char*>(h + 8), 8) != 8) break;
            size = rd64(h + 8);
            body = at + 16;
        } else if (size == 0) {
            size = uint64_t(to - at);
        }
        if (size < uint64_t(body - at) || qint64(size) > to - at) break;
        out.push_back({std::string(reinterpret_cast<const char*>(h + 4), 4), at, body, at + qint64(size)});
        at += qint64(size);
    }
    return out;
}
const Box* findBox(const std::vector<Box>& boxes, const char* type) {
    for (const Box& b : boxes)
        if (b.type == type) return &b;
    return nullptr;
}
[[maybe_unused]] QByteArray readBody(QFile& f, const Box& b, qint64 limit = qint64(64) << 20) {
    if (b.end - b.body > limit || !f.seek(b.body)) return {};
    return f.read(b.end - b.body);
}

bool isAv2Entry(const std::string& t) { return t == "av02" || t == "av2E" || t == "av2e"; }

#ifdef MONTAGE_WITH_AV2  // (what only reading needs)

// What an AV2 track in an MP4 gives: its samples, its configuration's OBUs and what its sample entry states.
struct Mp4Track {
    std::string entry;               // "av02" or "av2E"
    std::vector<uint8_t> configObus;  // length-delimited
    std::vector<qint64> offsets;
    std::vector<uint32_t> sizes;
    std::vector<bool> sync;           // (all, when the track has no 'stss')
    uint32_t timescale = 0;
    uint32_t delta = 0;               // the most common sample duration
    int width = 0, height = 0;
    bool colr = false;
    int primaries = 2, transfer = 2, matrix = 2;
    bool fullRange = false;
    int maxCll = 0, maxFall = 0;
    double mastering[8] = {};
    double masteringMax = 0, masteringMin = 0;
};

bool readMp4Track(const std::string& file, Mp4Track& t, std::string* error) {
    QFile f(QString::fromStdString(file));
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Cannot open " + file);
    const std::vector<Box> top = boxesIn(f, 0, f.size());
    const Box* moov = findBox(top, "moov");
    if (!moov) return fail(error, file + " has no movie box");
    for (const Box& trak : boxesIn(f, moov->body, moov->end)) {
        if (trak.type != "trak") continue;
        const std::vector<Box> tb = boxesIn(f, trak.body, trak.end);
        const Box* mdia = findBox(tb, "mdia");
        if (!mdia) continue;
        const std::vector<Box> mb = boxesIn(f, mdia->body, mdia->end);
        const Box *mdhd = findBox(mb, "mdhd"), *minf = findBox(mb, "minf");
        if (!mdhd || !minf) continue;
        const std::vector<Box> nb = boxesIn(f, minf->body, minf->end);
        const Box* stbl = findBox(nb, "stbl");
        if (!stbl) continue;
        const std::vector<Box> sb = boxesIn(f, stbl->body, stbl->end);
        const Box* stsd = findBox(sb, "stsd");
        if (!stsd) continue;
        const QByteArray sd = readBody(f, *stsd);
        if (sd.size() < 16) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(sd.constData());
        const std::string entry(reinterpret_cast<const char*>(p + 12), 4);
        if (!isAv2Entry(entry)) continue;
        // (the first sample entry: its size, then a visual sample entry's 78 bytes, then its boxes)
        const uint32_t entrySize = rd32(p + 8);
        if (entrySize < 86 || entrySize > uint32_t(sd.size()) - 8) return fail(error, "The AV2 sample entry of " + file + " is cut off");
        t = Mp4Track{};
        t.entry = entry;
        t.width = int(rd16(p + 8 + 32));
        t.height = int(rd16(p + 8 + 34));
        for (size_t at = 8 + 86; at + 8 <= 8 + size_t(entrySize);) {
            const uint32_t size = rd32(p + at);
            if (size < 8 || at + size > 8 + size_t(entrySize)) break;
            const std::string type(reinterpret_cast<const char*>(p + at + 4), 4);
            const uint8_t* b = p + at + 8;
            const size_t n = size - 8;
            if (type == "av2C") {
                // 'av02' (the binding's draft as av2-tools writes it): a version, the sequence header's fields (9 bytes
                // in all), then the OBUs; 'av2E' (a later draft): a reserved byte and the OBUs' count first.
                const size_t skip = entry == "av02" ? 9 : 2;
                if (n >= skip) t.configObus.assign(b + skip, b + n);
            } else if (type == "colr" && n >= 11 && std::memcmp(b, "nclx", 4) == 0) {
                t.colr = true;
                t.primaries = int(rd16(b + 4));
                t.transfer = int(rd16(b + 6));
                t.matrix = int(rd16(b + 8));
                t.fullRange = (b[10] & 0x80) != 0;
            } else if (type == "clli" && n >= 4) {
                t.maxCll = int(rd16(b));
                t.maxFall = int(rd16(b + 2));
            } else if (type == "mdcv" && n >= 24) {
                // G, B, R then the white point, in 0.00002; luminances in 0.0001 nits.
                const int order[3] = {1, 2, 0};
                for (int i = 0; i < 3; ++i) {
                    t.mastering[order[i] * 2] = rd16(b + 4 * i) * 0.00002;
                    t.mastering[order[i] * 2 + 1] = rd16(b + 4 * i + 2) * 0.00002;
                }
                t.mastering[6] = rd16(b + 12) * 0.00002;
                t.mastering[7] = rd16(b + 14) * 0.00002;
                t.masteringMax = rd32(b + 16) * 0.0001;
                t.masteringMin = rd32(b + 20) * 0.0001;
            }
            at += size;
        }
        {
            const QByteArray h = readBody(f, *mdhd);
            if (h.size() >= 24) {
                const auto* q = reinterpret_cast<const uint8_t*>(h.constData());
                t.timescale = q[0] == 1 ? (h.size() >= 32 ? rd32(q + 20) : 0) : rd32(q + 12);
            }
        }
        // The sample table: sizes, chunks and their offsets, which samples are sync samples, their durations.
        auto table = [&](const char* type) -> QByteArray {
            const Box* b = findBox(sb, type);
            return b ? readBody(f, *b, qint64(512) << 20) : QByteArray();
        };
        const QByteArray stsz = table("stsz"), stsc = table("stsc"), stco = table("stco"), co64 = table("co64"), stss = table("stss"),
                         stts = table("stts");
        if (stsz.size() < 12 || stsc.size() < 8 || (stco.size() < 8 && co64.size() < 8))
            return fail(error, "The AV2 track of " + file + " has no sample table");
        const auto* z = reinterpret_cast<const uint8_t*>(stsz.constData());
        const uint32_t fixed = rd32(z + 4), count = rd32(z + 8);
        if (!fixed && uint64_t(stsz.size()) < 12 + uint64_t(count) * 4) return fail(error, "The sample sizes of " + file + " are cut off");
        if (fixed && uint64_t(count) * fixed > uint64_t(f.size())) return fail(error, "The samples of " + file + " are past its end");
        t.sizes.resize(count);
        for (uint32_t i = 0; i < count; ++i) t.sizes[i] = fixed ? fixed : rd32(z + 12 + 4 * size_t(i));
        std::vector<qint64> chunks;
        if (co64.size() >= 8) {
            const auto* c = reinterpret_cast<const uint8_t*>(co64.constData());
            const uint32_t n = std::min<uint32_t>(rd32(c + 4), uint32_t((co64.size() - 8) / 8));
            for (uint32_t i = 0; i < n; ++i) chunks.push_back(qint64(rd64(c + 8 + 8 * size_t(i))));
        } else {
            const auto* c = reinterpret_cast<const uint8_t*>(stco.constData());
            const uint32_t n = std::min<uint32_t>(rd32(c + 4), uint32_t((stco.size() - 8) / 4));
            for (uint32_t i = 0; i < n; ++i) chunks.push_back(qint64(rd32(c + 8 + 4 * size_t(i))));
        }
        const auto* sc = reinterpret_cast<const uint8_t*>(stsc.constData());
        const uint32_t runs = std::min<uint32_t>(rd32(sc + 4), uint32_t((stsc.size() - 8) / 12));
        t.offsets.reserve(count);
        uint32_t sample = 0;
        for (uint32_t r = 0; r < runs && sample < count; ++r) {
            const uint32_t first = rd32(sc + 8 + 12 * size_t(r)), per = rd32(sc + 12 + 12 * size_t(r));
            const uint32_t last = r + 1 < runs ? rd32(sc + 8 + 12 * size_t(r + 1)) : uint32_t(chunks.size()) + 1;
            for (uint32_t c = first; c < last && c >= 1 && c <= chunks.size() && sample < count; ++c) {
                qint64 at = chunks[c - 1];
                for (uint32_t k = 0; k < per && sample < count; ++k, ++sample) {
                    t.offsets.push_back(at);
                    at += t.sizes[sample];
                }
            }
        }
        if (t.offsets.size() != count) return fail(error, "The sample table of " + file + " does not add up");
        t.sync.assign(count, stss.size() < 8);
        if (stss.size() >= 8) {
            const auto* s = reinterpret_cast<const uint8_t*>(stss.constData());
            const uint32_t n = std::min<uint32_t>(rd32(s + 4), uint32_t((stss.size() - 8) / 4));
            for (uint32_t i = 0; i < n; ++i)
                if (const uint32_t k = rd32(s + 8 + 4 * size_t(i)); k >= 1 && k <= count) t.sync[k - 1] = true;
        }
        if (stts.size() >= 16) {
            const auto* s = reinterpret_cast<const uint8_t*>(stts.constData());
            const uint32_t n = std::min<uint32_t>(rd32(s + 4), uint32_t((stts.size() - 8) / 8));
            uint32_t most = 0;
            for (uint32_t i = 0; i < n; ++i)
                if (rd32(s + 8 + 8 * size_t(i)) > most) most = rd32(s + 8 + 8 * size_t(i)), t.delta = rd32(s + 12 + 8 * size_t(i));
        }
        return true;
    }
    return fail(error, file + " has no AV2 track");
}

// ---- the temporal units of a file ----

struct Unit {
    qint64 at = 0;     // in the file
    uint32_t size = 0;
    bool sync = false;
    int sequence = -1;  // the sequence header in effect (State::sequences)
};

struct Stream {
    Av2Container container = Av2Container::None;
    std::vector<Unit> units;
    std::vector<std::vector<uint8_t>> sequences;  // each sequence header OBU (length-delimited)
    std::vector<uint8_t> configObus;             // MP4: the 'av2C' box's
    int fpsNum = 25, fpsDen = 1;
    Mp4Track mp4;
};

// The temporal units of a run of OBUs read from `f` at `at`: split at each temporal delimiter (an IVF frame can hold
// several), each one's sync and its sequence header noted.
bool scanObus(const std::vector<uint8_t>& data, qint64 at, Stream& s, std::string* error, const std::string& file) {
    std::vector<ObuAt> obus;
    if (!splitObus(data.data(), data.size(), obus)) return fail(error, "The OBUs of " + file + " are cut off");
    for (size_t i = 0; i < obus.size(); ++i) {
        const ObuAt& o = obus[i];
        if (o.type == kObuTemporalDelimiter || s.units.empty()) {
            Unit u;
            u.at = at + qint64(o.at);
            u.sequence = s.units.empty() ? -1 : s.units.back().sequence;
            s.units.push_back(u);
        }
        Unit& u = s.units.back();
        u.size = uint32_t(at + qint64(o.at + o.size) - u.at);
        if (o.type == kObuSequenceHeader) {
            const std::vector<uint8_t> sh(data.begin() + ptrdiff_t(o.at), data.begin() + ptrdiff_t(o.at + o.size));
            if (s.sequences.empty() || s.sequences.back() != sh) s.sequences.push_back(sh);
            u.sequence = int(s.sequences.size()) - 1;
        }
        // (sync: its first frame is a closed-loop key frame; temporal units are looked at once, at their first frame)
        if (codesFrame(o.type)) {
            bool first = true;
            for (size_t k = i; k-- > 0 && obus[k].type != kObuTemporalDelimiter;)
                if (codesFrame(obus[k].type)) first = false;
            if (first) u.sync = o.type == kObuClosedLoopKey;
        }
    }
    return true;
}

bool readStream(const std::string& file, Stream& s, std::string* error) {
    s = Stream{};
    s.container = av2Container(file);
    if (s.container == Av2Container::None) return fail(error, file + " holds no AV2");
    if (s.container == Av2Container::Mp4) {
        if (!readMp4Track(file, s.mp4, error)) return false;
        std::vector<ObuAt> obus;
        if (!splitObus(s.mp4.configObus.data(), s.mp4.configObus.size(), obus))
            return fail(error, "The AV2 configuration of " + file + " is cut off");
        s.configObus = s.mp4.configObus;
        for (const ObuAt& o : obus)
            if (o.type == kObuSequenceHeader) s.sequences.emplace_back(s.configObus.begin() + ptrdiff_t(o.at), s.configObus.begin() + ptrdiff_t(o.at + o.size));
        for (size_t i = 0; i < s.mp4.sizes.size(); ++i)
            s.units.push_back({s.mp4.offsets[i], s.mp4.sizes[i], bool(s.mp4.sync[i]), s.sequences.empty() ? -1 : 0});
        if (s.mp4.timescale && s.mp4.delta) s.fpsNum = int(s.mp4.timescale), s.fpsDen = int(s.mp4.delta);
        return true;
    }
    QFile f(QString::fromStdString(file));
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Cannot open " + file);
    if (s.container == Av2Container::Ivf) {
        uint8_t h[32];
        if (f.read(reinterpret_cast<char*>(h), 32) != 32) return fail(error, file + " is cut off");
        const uint32_t headerSize = h[6] | uint32_t(h[7]) << 8, rate = le32(h + 16), scale = le32(h + 20);
        if (!f.seek(std::max<uint32_t>(32, headerSize))) return fail(error, file + " is cut off");
        uint64_t step = 0, last = 0;
        bool firstFrame = true;
        for (;;) {
            uint8_t fh[12];
            if (f.read(reinterpret_cast<char*>(fh), 12) != 12) break;
            const uint32_t size = le32(fh);
            const uint64_t ts = le64(fh + 4);
            if (!firstFrame && ts > last && (!step || ts - last < step)) step = ts - last;
            firstFrame = false;
            last = ts;
            const qint64 at = f.pos();
            if (size > f.size() - at) return fail(error, "A frame of " + file + " is cut off");
            const QByteArray b = f.read(size);
            const std::vector<uint8_t> data(b.begin(), b.end());
            if (!scanObus(data, at, s, error, file)) return false;
        }
        // (a frame a tick, unless the frames' times step further)
        if (rate > 0 && scale > 0) {
            const uint64_t num = rate, den = uint64_t(scale) * std::max<uint64_t>(1, step);
            if (num <= uint64_t(INT32_MAX) && den <= uint64_t(INT32_MAX)) s.fpsNum = int(num), s.fpsDen = int(den);
        }
    } else {
        // (read 64 MB at a time, a run's last unfinished OBU carried into the next)
        constexpr qint64 kBlock = qint64(64) << 20;
        std::vector<uint8_t> data;
        qint64 base = 0;
        for (;;) {
            const QByteArray b = f.read(kBlock);
            data.insert(data.end(), b.begin(), b.end());
            const bool end = b.size() < kBlock;
            size_t whole = 0;
            while (whole < data.size()) {
                uint64_t size = 0;
                const int l = leb128(data.data() + whole, data.size() - whole, size);
                if (!l || size > data.size() - whole - size_t(l)) break;
                whole += size_t(l) + size_t(size);
            }
            if (end && whole != data.size()) return fail(error, "The last OBU of " + file + " is cut off");
            const std::vector<uint8_t> head(data.begin(), data.begin() + ptrdiff_t(whole));
            if (!scanObus(head, base, s, error, file)) return false;
            data.erase(data.begin(), data.begin() + ptrdiff_t(whole));
            base += qint64(whole);
            if (end) break;
        }
    }
    if (s.units.empty()) return fail(error, file + " holds no frames");
    return true;
}

// Temporal unit `i` as a decoder takes it.
bool unitBytes(QFile& f, const Stream& s, int64_t i, bool start, std::vector<uint8_t>& out, std::string* error) {
    const Unit& u = s.units[size_t(i)];
    out.clear();
    std::vector<uint8_t> body(u.size);
    if (!f.seek(u.at) || f.read(reinterpret_cast<char*>(body.data()), u.size) != qint64(u.size))
        return fail(error, "Cannot read " + f.fileName().toStdString());
    if (s.container == Av2Container::Mp4) {
        // A temporal delimiter, the configuration's OBUs before a sync sample (or the first decoded), then the sample.
        out = kTemporalDelimiter;
        if (u.sync || start) out.insert(out.end(), s.configObus.begin(), s.configObus.end());
        out.insert(out.end(), body.begin(), body.end());
        return true;
    }
    // (a stream started where its sequence header is not repeated: the header in effect first, after the delimiter)
    std::vector<ObuAt> obus;
    if (start && u.sequence >= 0 && splitObus(body.data(), body.size(), obus) &&
        std::none_of(obus.begin(), obus.end(), [](const ObuAt& o) { return o.type == kObuSequenceHeader; })) {
        const size_t td = !obus.empty() && obus[0].type == kObuTemporalDelimiter ? obus[0].size : 0;
        out.insert(out.end(), body.begin(), body.begin() + ptrdiff_t(td));
        if (!td) out = kTemporalDelimiter;
        out.insert(out.end(), s.sequences[size_t(u.sequence)].begin(), s.sequences[size_t(u.sequence)].end());
        out.insert(out.end(), body.begin() + ptrdiff_t(td), body.end());
        return true;
    }
    out = std::move(body);
    return true;
}

std::string spaceForTags(int primaries, int transfer) {
    const char* pri = av_color_primaries_name(AVColorPrimaries(primaries));
    const char* trc = av_color_transfer_name(AVColorTransferCharacteristic(transfer));
    std::string id = colorSpaceFromTags(pri ? pri : "", trc ? trc : "");
    return id == "rec709" ? std::string() : id;
}

#endif

}  // namespace

Av2Container av2Container(const std::string& file) {
    QFile f(QString::fromStdString(file));
    if (!f.open(QIODevice::ReadOnly)) return Av2Container::None;
    const QByteArray head = f.read(64);
    const auto* p = reinterpret_cast<const uint8_t*>(head.constData());
    const size_t n = size_t(head.size());
    if (n >= 32 && std::memcmp(p, "DKIF", 4) == 0) return std::memcmp(p + 8, "AV02", 4) == 0 ? Av2Container::Ivf : Av2Container::None;
    if (n >= 12 && std::memcmp(p + 4, "ftyp", 4) == 0) {
        // An MP4 whose first AV2 sample entry is in its movie box (looked for in the sample descriptions).
        const std::vector<Box> top = boxesIn(f, 0, f.size());
        const Box* moov = findBox(top, "moov");
        if (!moov) return Av2Container::None;
        for (const Box& trak : boxesIn(f, moov->body, moov->end)) {
            if (trak.type != "trak") continue;
            const Box* mdia = nullptr;
            const std::vector<Box> tb = boxesIn(f, trak.body, trak.end);
            if (!(mdia = findBox(tb, "mdia"))) continue;
            const std::vector<Box> mb = boxesIn(f, mdia->body, mdia->end);
            const Box* minf = findBox(mb, "minf");
            if (!minf) continue;
            const std::vector<Box> nb = boxesIn(f, minf->body, minf->end);
            const Box* stbl = findBox(nb, "stbl");
            if (!stbl) continue;
            const std::vector<Box> sb = boxesIn(f, stbl->body, stbl->end);
            const Box* stsd = findBox(sb, "stsd");
            if (!stsd || stsd->end - stsd->body < 16 || !f.seek(stsd->body + 12)) continue;
            if (isAv2Entry(f.read(4).toStdString())) return Av2Container::Mp4;
        }
        return Av2Container::None;
    }
    // OBUs: a temporal delimiter (its size, 1, and its header), then a sequence header.
    if (n >= 4 && p[0] == 0x01 && ((p[1] >> 2) & 31) == kObuTemporalDelimiter && !(p[1] & 0x80)) {
        uint64_t size = 0;
        const int l = leb128(p + 2, n - 2, size);
        if (l && 2 + size_t(l) < n && ((p[2 + l] >> 2) & 31) == kObuSequenceHeader) return Av2Container::Obu;
    }
    return Av2Container::None;
}

Av2Container av2ContainerForPath(const std::string& file) {
    const std::string ext = lowerSuffix(file);
    return ext == "ivf" ? Av2Container::Ivf : ext == "obu" ? Av2Container::Obu : Av2Container::Mp4;
}

#ifndef MONTAGE_WITH_AV2

bool av2Available() { return false; }
bool av2EncoderAvailable() { return false; }
std::string av2Versions() { return {}; }
bool readAv2Info(const std::string&, Av2Info&, std::string* error) { return fail(error, "This build of Montage reads no AV2 (dav2d)"); }
struct Av2Reader::State {
    Av2Info info;
};
Av2Reader::Av2Reader() : s_(std::make_unique<State>()) {}
Av2Reader::~Av2Reader() = default;
bool Av2Reader::open(const std::string&, std::string* error) { return fail(error, "This build of Montage reads no AV2 (dav2d)"); }
const Av2Info& Av2Reader::info() const { return s_->info; }
bool Av2Reader::frame(int64_t, AVFrame*, std::string* error) { return fail(error, "This build of Montage reads no AV2 (dav2d)"); }
int64_t Av2Reader::temporalUnits() const { return 0; }
bool Av2Reader::temporalUnit(int64_t, std::vector<uint8_t>&, bool*, std::string* error) {
    return fail(error, "This build of Montage reads no AV2 (dav2d)");
}
struct Av2Writer::State {
    std::string path;
};
Av2Writer::Av2Writer() : s_(std::make_unique<State>()) {}
Av2Writer::~Av2Writer() = default;
bool Av2Writer::open(const std::string&, const Av2WriteSettings&, std::string* error) {
    return fail(error, "This build of Montage writes no AV2 (AVM)");
}
bool Av2Writer::addAudio(const AVCodecParameters*, int, int, std::string* error) { return fail(error, "This build of Montage writes no AV2 (AVM)"); }
bool Av2Writer::encode(const AVFrame*, std::string* error) { return fail(error, "This build of Montage writes no AV2 (AVM)"); }
bool Av2Writer::audio(const AVPacket*, std::string* error) { return fail(error, "This build of Montage writes no AV2 (AVM)"); }
void Av2Writer::setLightLevels(int, int) {}
bool Av2Writer::finish(std::string* error) { return fail(error, "This build of Montage writes no AV2 (AVM)"); }
int64_t Av2Writer::framesWritten() const { return 0; }
const std::string& Av2Writer::path() const { return s_->path; }

#else

bool av2Available() { return true; }
bool av2EncoderAvailable() { return true; }
std::string av2Versions() { return std::string("dav2d ") + dav2d_version() + ", AVM " + avm_codec_version_str(); }

namespace {

void quiet(void*, const char*, va_list) {}

// A dav2d decoder: one shown picture at a time (so where decoding starts decides which frame comes out first).
Dav2dContext* openDecoder(std::string* error) {
    Dav2dSettings ds;
    dav2d_default_settings(&ds);
    ds.n_threads = int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
    ds.max_frame_delay = 1;
    ds.logger.callback = quiet;
    Dav2dContext* c = nullptr;
    if (dav2d_open(&c, &ds) < 0 || !c) {
        fail(error, "Cannot open the AV2 decoder (dav2d)");
        return nullptr;
    }
    return c;
}

// Feeds `tu` to `c`, handing each picture that comes out to `got` (false from it stops). False on a decoding error.
template <typename Got>
bool decodeUnit(Dav2dContext* c, const std::vector<uint8_t>* tu, Got&& got, std::string* error) {
    Dav2dData data{};
    if (tu) {
        uint8_t* p = dav2d_data_create(&data, tu->size());
        if (!p) return fail(error, "Out of memory");
        std::memcpy(p, tu->data(), tu->size());
    }
    for (;;) {
        if (data.sz) {
            const int rc = dav2d_send_data(c, &data);
            if (rc < 0 && rc != DAV2D_ERR(EAGAIN)) {
                dav2d_data_unref(&data);
                return fail(error, "The AV2 stream cannot be decoded (dav2d: " + std::string(std::strerror(-rc)) + ")");
            }
        }
        Dav2dPicture pic{};
        const int rc = dav2d_get_picture(c, &pic);
        if (rc == 0) {
            if (!got(pic)) {
                dav2d_data_unref(&data);
                return true;
            }
            continue;
        }
        if (rc != DAV2D_ERR(EAGAIN)) {
            dav2d_data_unref(&data);
            return fail(error, "The AV2 stream cannot be decoded (dav2d: " + std::string(std::strerror(-rc)) + ")");
        }
        if (!data.sz) return true;  // (all of it taken, nothing more out)
    }
}

// The colour a picture's content interpretation states (CICP), its shorthands as AVM's reference software reads them:
// dav2d 0.0.1 takes the Rec.709 shorthand's matrix as 5 and HLG's transfer as 14, where AVM writes 1 and 18.
void pictureColour(const Dav2dPicture& pic, Av2Info& info) {
    if (const Dav2dContentInterpretation* ci = pic.ci; ci && ci->color_description_present) {
        switch (ci->color.type) {
        case DAV2D_COLOR_DESC_BT709SDR: info.primaries = 1, info.transfer = 1, info.matrix = 1; break;
        case DAV2D_COLOR_DESC_BT2100PQ: info.primaries = 9, info.transfer = 16, info.matrix = 9; break;
        case DAV2D_COLOR_DESC_BT2100HLG: info.primaries = 9, info.transfer = 18, info.matrix = 9; break;
        case DAV2D_COLOR_DESC_SRGB: info.primaries = 1, info.transfer = 13, info.matrix = 0; break;
        case DAV2D_COLOR_DESC_SRGBSYCC: info.primaries = 1, info.transfer = 13, info.matrix = 5; break;
        default: info.primaries = ci->color.pri, info.transfer = ci->color.trc, info.matrix = ci->color.mtrx; break;
        }
        info.fullRange = ci->color.range != 0;
    }
    if (const Dav2dContentLightLevel* cl = pic.content_light) info.maxCll = cl->max_content_light_level, info.maxFall = cl->max_frame_average_light_level;
    if (const Dav2dMasteringDisplay* md = pic.mastering_display) {
        for (int i = 0; i < 3; ++i) info.mastering[2 * i] = md->primaries[i][0] / 65536.0, info.mastering[2 * i + 1] = md->primaries[i][1] / 65536.0;
        info.mastering[6] = md->white_point[0] / 65536.0;
        info.mastering[7] = md->white_point[1] / 65536.0;
        info.masteringMax = md->max_luminance / 256.0;
        info.masteringMin = md->min_luminance / 16384.0;
    }
}

// HDR light levels and the mastering display from the metadata OBUs of a temporal unit (one metadata OBU each, or a
// group of them as AVM writes them, which dav2d 0.0.1 passes over), where the decoder stated none.
void unitHdrMetadata(const std::vector<uint8_t>& tu, Av2Info& info) {
    std::vector<ObuAt> obus;
    if (!splitObus(tu.data(), tu.size(), obus)) return;
    auto take = [&](uint64_t type, const uint8_t* p, size_t n) {
        if (type == 1 && n >= 4 && info.maxCll <= 0) {  // (content light level)
            info.maxCll = int(rd16(p));
            info.maxFall = int(rd16(p + 2));
        } else if (type == 2 && n >= 24 && info.masteringMax <= 0) {  // (mastering display: R, G, B, white; luminances)
            for (int i = 0; i < 8; ++i) info.mastering[i] = rd16(p + 2 * i) / 65536.0;
            info.masteringMax = rd32(p + 16) / 256.0;
            info.masteringMin = rd32(p + 20) / 16384.0;
        }
    };
    for (const ObuAt& o : obus) {
        if (o.type != kObuMetadata && o.type != kObuMetadataGroup) continue;
        uint64_t size = 0;
        const int l = leb128(tu.data() + o.at, o.size, size);
        const uint8_t* p = tu.data() + o.at + size_t(l);
        const uint8_t* end = p + size;
        p += (p[0] & 0x80) ? 2 : 1;  // (the OBU's header, and its extension)
        uint64_t v = 0;
        int k = 0;
        if (o.type == kObuMetadata) {
            if (p >= end || !(k = leb128(p, size_t(end - p), v))) continue;
            take(v, p + k, size_t(end - (p + k)));
            continue;
        }
        // A group: its suffix, necessity and application byte, how many units less one, then each unit: its type, its
        // header's size (7 bits) and cancel flag, then (uncancelled) its payload's size and the rest of its header.
        if (end - p < 2 || !(k = leb128(p + 1, size_t(end - p - 1), v))) continue;
        p += 1 + k;
        for (uint64_t u = 0; u <= v && p < end; ++u) {
            uint64_t type = 0, sz = 0;
            if (!(k = leb128(p, size_t(end - p), type)) || p + k >= end) break;
            p += k;
            const int headerSize = p[0] >> 1;
            const bool cancel = p[0] & 1;
            const uint8_t* header = p + 1;
            if (header + headerSize > end) break;
            if (cancel) {
                p = header + headerSize;
                continue;
            }
            if (!(k = leb128(header, size_t(headerSize), sz)) || sz > uint64_t(end - (header + headerSize))) break;
            take(type, header + headerSize, size_t(sz));
            p = header + headerSize + sz;
        }
    }
}

int pixelFormat(int layout, int bpc) {
    const int k = bpc > 10 ? 2 : bpc > 8 ? 1 : 0;
    switch (layout) {
    case DAV2D_PIXEL_LAYOUT_I400: return k == 2 ? AV_PIX_FMT_GRAY12LE : k ? AV_PIX_FMT_GRAY10LE : AV_PIX_FMT_GRAY8;
    case DAV2D_PIXEL_LAYOUT_I422: return k == 2 ? AV_PIX_FMT_YUV422P12LE : k ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
    case DAV2D_PIXEL_LAYOUT_I444: return k == 2 ? AV_PIX_FMT_YUV444P12LE : k ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
    default: return k == 2 ? AV_PIX_FMT_YUV420P12LE : k ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    }
}

AVChromaLocation chromaLocation(const Dav2dPicture& pic) {
    if (!pic.ci || !pic.ci->chroma_sample_position_present) return AVCHROMA_LOC_UNSPECIFIED;
    switch (pic.ci->chr[0]) {
    case DAV2D_CHR_LEFT: return AVCHROMA_LOC_LEFT;
    case DAV2D_CHR_CENTER: return AVCHROMA_LOC_CENTER;
    case DAV2D_CHR_TOPLEFT: return AVCHROMA_LOC_TOPLEFT;
    case DAV2D_CHR_TOP: return AVCHROMA_LOC_TOP;
    case DAV2D_CHR_BOTTOMLEFT: return AVCHROMA_LOC_BOTTOMLEFT;
    case DAV2D_CHR_BOTTOM: return AVCHROMA_LOC_BOTTOM;
    default: return AVCHROMA_LOC_UNSPECIFIED;
    }
}

}  // namespace

struct Av2Reader::State {
    std::string file;
    QFile f;
    Stream stream;
    Av2Info info;
    // Where decoding can start: each sync unit, and how many frames are shown before it (its first shown frame's number).
    std::vector<std::pair<int64_t, int64_t>> starts;
    Dav2dContext* dec = nullptr;
    int64_t nextUnit = -1;  // the unit read next (-1: none yet)
    bool fromStart = false; // nextUnit is where decoding started
    Dav2dData pending{};    // what the decoder has not taken yet of the last unit read
    int64_t shown = 0;      // frames out so far (the next one's number)
    Dav2dPicture pic{};     // the last frame out, number shown - 1
    bool havePic = false;
    ~State() {
        if (pending.sz) dav2d_data_unref(&pending);
        if (havePic) dav2d_picture_unref(&pic);
        if (dec) dav2d_close(&dec);
    }
    void restart(int64_t unit, int64_t shownBefore) {
        if (pending.sz) dav2d_data_unref(&pending);
        if (havePic) dav2d_picture_unref(&pic);
        havePic = false;
        if (dec) dav2d_flush(dec);
        nextUnit = unit;
        fromStart = true;
        shown = shownBefore;
    }
};

Av2Reader::Av2Reader() : s_(std::make_unique<State>()) {}
Av2Reader::~Av2Reader() = default;
const Av2Info& Av2Reader::info() const { return s_->info; }
int64_t Av2Reader::temporalUnits() const { return int64_t(s_->stream.units.size()); }

bool Av2Reader::temporalUnit(int64_t i, std::vector<uint8_t>& out, bool* sync, std::string* error) {
    if (i < 0 || i >= temporalUnits()) return fail(error, "No such temporal unit");
    if (sync) *sync = s_->stream.units[size_t(i)].sync;
    return unitBytes(s_->f, s_->stream, i, false, out, error);
}

bool Av2Reader::open(const std::string& file, std::string* error) {
    s_ = std::make_unique<State>();
    State& s = *s_;
    s.file = file;
    if (!readStream(file, s.stream, error)) return false;
    s.f.setFileName(QString::fromStdString(file));
    if (!s.f.open(QIODevice::ReadOnly)) return fail(error, "Cannot open " + file);
    const Stream& st = s.stream;
    if (st.sequences.empty()) return fail(error, file + " has no AV2 sequence header");
    Av2Info& in = s.info;
    in.container = st.container;
    in.fpsNum = st.fpsNum;
    in.fpsDen = st.fpsDen;
    if (!(s.dec = openDecoder(error))) return false;
    // The first frame decoded for the stream's facts: its sequence header (as the decoder read it), size and colour.
    bool firstSeen = false;
    auto first = [&](const Dav2dPicture& pic) {
        if (firstSeen) return;
        firstSeen = true;
        if (const Dav2dSequenceHeader* sh = pic.seq_hdr) {
            in.profile = sh->profile;
            in.level = sh->level;
            in.tier = sh->tier;
            in.monotonic = sh->monotonic != 0;
        }
        in.chroma = pic.p.layout == DAV2D_PIXEL_LAYOUT_I400 ? 1 : pic.p.layout == DAV2D_PIXEL_LAYOUT_I444 ? 2 : pic.p.layout == DAV2D_PIXEL_LAYOUT_I422 ? 3 : 0;
        in.width = pic.p.w;
        in.height = pic.p.h;
        in.bits = pic.p.bpc;
        pictureColour(pic, in);
        // (a bare stream's timing info, when it states one)
        if (st.container == Av2Container::Obu && pic.ci && pic.ci->timing_info_present && pic.ci->timing.num_units_in_display_tick > 0 &&
            pic.ci->timing.time_scale > 0 && pic.ci->timing.time_scale <= uint32_t(INT32_MAX) && pic.ci->timing.num_units_in_display_tick <= uint32_t(INT32_MAX))
            in.fpsNum = int(pic.ci->timing.time_scale), in.fpsDen = int(pic.ci->timing.num_units_in_display_tick);
    };
    std::vector<uint8_t> tu;
    for (int64_t i = 0; !firstSeen && i < int64_t(st.units.size()); ++i) {
        if (!unitBytes(s.f, st, i, i == 0, tu, error)) return false;
        if (!decodeUnit(s.dec, &tu, [&](Dav2dPicture& pic) {
                first(pic);
                dav2d_picture_unref(&pic);
                return false;
            }, error))
            return false;
    }
    if (!firstSeen && !decodeUnit(s.dec, nullptr, [&](Dav2dPicture& pic) {
            first(pic);
            dav2d_picture_unref(&pic);
            return false;
        }, error))
        return false;
    // Monotonic output: a frame a temporal unit, so each sync unit's number is its first frame's. Otherwise the frames
    // are counted by decoding the whole stream once.
    if (firstSeen && in.monotonic) {
        for (size_t i = 0; i < st.units.size(); ++i)
            if (st.units[i].sync || i == 0) s.starts.push_back({int64_t(i), int64_t(i)});
        in.frames = int64_t(st.units.size());
    } else if (firstSeen) {
        dav2d_flush(s.dec);
        int64_t shown = 0;
        auto count = [&](Dav2dPicture& pic) {
            dav2d_picture_unref(&pic);
            ++shown;
            return true;
        };
        for (int64_t i = 0; i < int64_t(st.units.size()); ++i) {
            if (st.units[size_t(i)].sync || i == 0) s.starts.push_back({i, shown});
            if (!unitBytes(s.f, st, i, i == 0, tu, error) || !decodeUnit(s.dec, &tu, count, error)) return false;
        }
        if (!decodeUnit(s.dec, nullptr, count, error)) return false;
        in.frames = shown;
    }
    if (!firstSeen) return fail(error, file + " shows no frame");
    // (light levels and the mastering display the decoder passed over, in the stream's first unit)
    if (st.container != Av2Container::Mp4 && unitBytes(s.f, st, 0, true, tu, nullptr)) unitHdrMetadata(tu, in);
    // The container's colour and light levels are the ones that count (MP4 carries no content interpretation).
    if (st.container == Av2Container::Mp4) {
        const Mp4Track& t = st.mp4;
        if (t.colr) {
            if (t.primaries != 2) in.primaries = t.primaries;
            if (t.transfer != 2) in.transfer = t.transfer;
            if (t.matrix != 2) in.matrix = t.matrix;
            in.fullRange = t.fullRange;
        }
        if (t.maxCll > 0) in.maxCll = t.maxCll, in.maxFall = t.maxFall;
        if (t.masteringMax > 0) {
            std::copy(std::begin(t.mastering), std::end(t.mastering), std::begin(in.mastering));
            in.masteringMax = t.masteringMax;
            in.masteringMin = t.masteringMin;
        }
    }
    in.colorSpace = spaceForTags(in.primaries, in.transfer);
    s.restart(0, 0);
    return true;
}

bool Av2Reader::frame(int64_t n, AVFrame* into, std::string* error) {
    State& s = *s_;
    if (!s.dec) return fail(error, "No AV2 file is open");
    if (n < 0 || n >= s.info.frames) return fail(error, "No frame " + std::to_string(n) + " in " + s.file);
    // From the start before it, unless it is the one out already or comes later in what is being decoded.
    const auto start = std::upper_bound(s.starts.begin(), s.starts.end(), n,
                                        [](int64_t v, const std::pair<int64_t, int64_t>& p) { return v < p.second; });
    const std::pair<int64_t, int64_t> from = start == s.starts.begin() ? s.starts.front() : *(start - 1);
    const bool have = s.havePic && s.shown - 1 == n;
    if (!have && (s.nextUnit < 0 || n < s.shown || from.second > s.shown)) s.restart(from.first, from.second);
    std::vector<uint8_t> tu;
    auto broken = [&](int rc) { return fail(error, "The AV2 stream of " + s.file + " cannot be decoded (dav2d: " + std::strerror(-rc) + ")"); };
    // Pictures out until frame n is: the decoder fed what it has not taken, then the next unit, then drained.
    while (!(s.havePic && s.shown - 1 == n)) {
        Dav2dPicture pic{};
        int rc = dav2d_get_picture(s.dec, &pic);
        if (rc == 0) {
            if (s.havePic) dav2d_picture_unref(&s.pic);
            s.pic = pic;
            s.havePic = true;
            ++s.shown;
            continue;
        }
        if (rc != DAV2D_ERR(EAGAIN)) return broken(rc);
        if (s.pending.sz) {
            const size_t before = s.pending.sz;
            if ((rc = dav2d_send_data(s.dec, &s.pending)) < 0 && rc != DAV2D_ERR(EAGAIN)) {
                dav2d_data_unref(&s.pending);
                return broken(rc);
            }
            if (rc == DAV2D_ERR(EAGAIN) && s.pending.sz == before && dav2d_get_picture(s.dec, &pic) == DAV2D_ERR(EAGAIN))
                return fail(error, "The AV2 decoder (dav2d) took nothing more of " + s.file);
            if (pic.p.w) {  // (out after all)
                if (s.havePic) dav2d_picture_unref(&s.pic);
                s.pic = pic;
                s.havePic = true;
                ++s.shown;
            }
            continue;
        }
        if (s.nextUnit >= int64_t(s.stream.units.size()))
            return fail(error, "Frame " + std::to_string(n) + " of " + s.file + " never came out of the decoder");
        if (!unitBytes(s.f, s.stream, s.nextUnit, s.fromStart, tu, error)) return false;
        s.fromStart = false;
        ++s.nextUnit;
        uint8_t* d = dav2d_data_create(&s.pending, tu.size());
        if (!d) return fail(error, "Out of memory");
        std::memcpy(d, tu.data(), tu.size());
    }
    const Dav2dPicture& p = s.pic;
    av_frame_unref(into);
    into->width = p.p.w;
    into->height = p.p.h;
    into->format = pixelFormat(p.p.layout, p.p.bpc);
    if (av_frame_get_buffer(into, 0) < 0) return fail(error, "Out of memory");
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(into->format));
    const int bytes = p.p.bpc > 8 ? 2 : 1;
    for (int c = 0; c < (p.p.layout == DAV2D_PIXEL_LAYOUT_I400 ? 1 : 3); ++c) {
        const int w = c ? AV_CEIL_RSHIFT(p.p.w, d->log2_chroma_w) : p.p.w, h = c ? AV_CEIL_RSHIFT(p.p.h, d->log2_chroma_h) : p.p.h;
        av_image_copy_plane(into->data[c], into->linesize[c], static_cast<const uint8_t*>(p.data[c]), int(p.stride[c ? 1 : 0]), w * bytes, h);
    }
    const Av2Info& in = s.info;
    into->color_primaries = AVColorPrimaries(in.primaries);
    into->color_trc = AVColorTransferCharacteristic(in.transfer);
    into->colorspace = AVColorSpace(in.matrix);
    into->color_range = in.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    into->chroma_location = chromaLocation(p);
    into->pts = n;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
    if (p.frame_hdr && p.frame_hdr->frame_type == DAV2D_FRAME_TYPE_KEY) into->flags |= AV_FRAME_FLAG_KEY;
#endif
    return true;
}

bool readAv2Info(const std::string& file, Av2Info& out, std::string* error) {
    Av2Reader r;
    if (!r.open(file, error)) return false;
    out = r.info();
    return true;
}

// ---- writing ----

namespace {

void put8(std::vector<uint8_t>& b, unsigned v) { b.push_back(uint8_t(v)); }
void put16(std::vector<uint8_t>& b, unsigned v) { put8(b, v >> 8), put8(b, v); }
void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, v >> 16), put16(b, v & 0xffff); }
void put64(std::vector<uint8_t>& b, uint64_t v) { put32(b, uint32_t(v >> 32)), put32(b, uint32_t(v)); }
void putLe32(std::vector<uint8_t>& b, uint32_t v) { put8(b, v), put8(b, v >> 8), put8(b, v >> 16), put8(b, v >> 24); }
void putLe64(std::vector<uint8_t>& b, uint64_t v) { putLe32(b, uint32_t(v)), putLe32(b, uint32_t(v >> 32)); }
void putType(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }
std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    put32(b, uint32_t(payload.size() + 8));
    putType(b, type);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
std::vector<uint8_t> fullBox(const char* type, int version, uint32_t flags, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    put32(b, uint32_t(version) << 24 | (flags & 0xffffff));
    b.insert(b.end(), payload.begin(), payload.end());
    return box(type, b);
}
std::vector<uint8_t> operator+(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
void putMatrix(std::vector<uint8_t>& b) {
    for (uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) put32(b, v);
}
// An MPEG-4 descriptor: its tag and its length (in as many bytes as it needs).
std::vector<uint8_t> descriptor(int tag, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> b{uint8_t(tag)};
    const size_t n = body.size();
    for (int shift = 21; shift > 0; shift -= 7)
        if (n >> shift) b.push_back(uint8_t(0x80 | ((n >> shift) & 0x7f)));
    b.push_back(uint8_t(n & 0x7f));
    return b + body;
}

struct Sample {
    uint64_t at = 0;
    uint32_t size = 0;
    uint32_t duration = 0;
    bool sync = true;
};

}  // namespace

struct Av2Writer::State {
    std::string path;
    Av2WriteSettings set;
    avm_codec_ctx_t enc{};
    bool encOpen = false;
    avm_image_t img{};
    bool imgAlloc = false;
    QFile f;
    bool finished = false;
    int64_t framesIn = 0;
    int64_t units = 0;  // temporal units written
    // MP4
    uint64_t mdatAt = 0;
    std::vector<uint8_t> sequenceHeader;  // the first (length-delimited), for the 'av2C' box
    std::vector<uint8_t> firstUnit;       // as AVM gave it (its sequence header's fields read by decoding it)
    std::vector<Sample> video, sound;
    bool hasAudio = false;
    std::vector<uint8_t> audioConfig;
    int sampleRate = 48000, channels = 2;
    int audioTbNum = 1, audioTbDen = 48000;
    int64_t audioFirstPts = INT64_MIN, audioLastEnd = 0;
    int64_t audioBitrate = 0;
    std::vector<uint8_t> pending;  // what AVM gave that is not yet a whole temporal unit

    ~State() {
        if (imgAlloc) avm_img_free(&img);
        if (encOpen) avm_codec_destroy(&enc);
        if (f.isOpen() && !finished) {
            f.close();
            f.remove();
        }
    }
    bool write(const std::vector<uint8_t>& b) { return f.write(reinterpret_cast<const char*>(b.data()), qint64(b.size())) == qint64(b.size()); }
    bool unit(const uint8_t* data, size_t n, std::string* error);
    bool drain(std::string* error);
    bool moov(std::string* error);
};

int64_t Av2Writer::framesWritten() const { return s_->units; }
const std::string& Av2Writer::path() const { return s_->path; }

Av2Writer::Av2Writer() : s_(std::make_unique<State>()) {}
Av2Writer::~Av2Writer() = default;

bool Av2Writer::open(const std::string& path, const Av2WriteSettings& settings, std::string* error) {
    s_ = std::make_unique<State>();
    State& s = *s_;
    s.path = path;
    s.set = settings;
    const Av2WriteSettings& st = s.set;
    if (st.width <= 0 || st.height <= 0 || st.width > 65535 || st.height > 65535) return fail(error, "An AV2 picture is 1 to 65535 pixels across and down");
    if (st.bits != 8 && st.bits != 10) return fail(error, "AV2 is written at 8 or 10 bits");
    if (st.fpsNum <= 0 || st.fpsDen <= 0) return fail(error, "No frame rate");
    if (st.container == Av2Container::None) return fail(error, "No AV2 container");
    avm_codec_iface_t* iface = avm_codec_av2_cx();
    avm_codec_enc_cfg_t cfg;
    if (avm_codec_enc_config_default(iface, &cfg, AVM_USAGE_GOOD_QUALITY) != AVM_CODEC_OK) return fail(error, "AVM has no default configuration");
    cfg.g_w = unsigned(st.width);
    cfg.g_h = unsigned(st.height);
    cfg.g_timebase.num = st.fpsDen;
    cfg.g_timebase.den = st.fpsNum;
    cfg.g_bit_depth = st.bits == 10 ? AVM_BITS_10 : AVM_BITS_8;
    cfg.g_input_bit_depth = unsigned(st.bits);
    cfg.g_threads = unsigned(st.threads > 0 ? st.threads : int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u)));
    cfg.g_pass = AVM_RC_ONE_PASS;
    cfg.kf_mode = AVM_KF_AUTO;
    cfg.kf_min_dist = 0;
    cfg.kf_max_dist = unsigned(st.gop > 0 ? st.gop : std::max(1, int(std::lround(2.0 * st.fpsNum / st.fpsDen))));
    if (st.bitrate > 0) {
        cfg.rc_end_usage = AVM_VBR;
        cfg.rc_target_bitrate = unsigned(std::clamp<int64_t>(st.bitrate / 1000, 1, INT32_MAX));
    } else {
        cfg.rc_end_usage = AVM_Q;
    }
    if (avm_codec_enc_init(&s.enc, iface, &cfg, 0) != AVM_CODEC_OK)
        return fail(error, std::string("Cannot open AVM's AV2 encoder: ") + avm_codec_error_detail(&s.enc));
    s.encOpen = true;
    // A frame each temporal unit, shown as it is decoded (the monotonic output order, which the MP4 binding's sample
    // times assume), at the speed preset and quantiser asked.
    const int qp = st.qp > 0 ? std::min(st.qp, 255) : std::clamp(st.crf * 4, 0, 255);
    const bool ok = avm_codec_control(&s.enc, AVME_SET_CPUUSED, std::clamp(st.speed, 0, 6)) == AVM_CODEC_OK &&
                    avm_codec_control(&s.enc, AV2E_SET_ENABLE_KEYFRAME_FILTERING, 0) == AVM_CODEC_OK &&
                    avm_codec_control(&s.enc, AV2E_SET_MONOTONIC_OUTPUT_ORDER, 1) == AVM_CODEC_OK &&
                    (st.bitrate > 0 || avm_codec_control(&s.enc, AVME_SET_QP, unsigned(qp)) == AVM_CODEC_OK) &&
                    avm_codec_control(&s.enc, AV2E_SET_ROW_MT, 1u) == AVM_CODEC_OK &&
                    avm_codec_control(&s.enc, AV2E_SET_COLOR_PRIMARIES, st.primaries) == AVM_CODEC_OK &&
                    avm_codec_control(&s.enc, AV2E_SET_TRANSFER_CHARACTERISTICS, st.transfer) == AVM_CODEC_OK &&
                    avm_codec_control(&s.enc, AV2E_SET_MATRIX_COEFFICIENTS, st.matrix) == AVM_CODEC_OK &&
                    avm_codec_control(&s.enc, AV2E_SET_COLOR_RANGE, st.fullRange ? 1 : 0) == AVM_CODEC_OK;
    if (!ok) return fail(error, std::string("AVM refused a setting: ") + avm_codec_error_detail(&s.enc));
    if (!avm_img_alloc(&s.img, AVM_IMG_FMT_I42016, unsigned(st.width), unsigned(st.height), 32)) return fail(error, "Out of memory");
    s.imgAlloc = true;
    s.f.setFileName(QString::fromStdString(path));
    if (!s.f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail(error, "Cannot write " + path);
    if (st.container == Av2Container::Ivf) {
        std::vector<uint8_t> h;
        putType(h, "DKIF");
        h.push_back(0), h.push_back(0), h.push_back(32), h.push_back(0);
        putType(h, "AV02");
        h.push_back(uint8_t(st.width)), h.push_back(uint8_t(st.width >> 8)), h.push_back(uint8_t(st.height)), h.push_back(uint8_t(st.height >> 8));
        putLe32(h, uint32_t(st.fpsNum));
        putLe32(h, uint32_t(st.fpsDen));
        putLe32(h, 0);  // (the frame count, set at the end)
        putLe32(h, 0);
        if (!s.write(h)) return fail(error, "Cannot write " + path);
    } else if (st.container == Av2Container::Mp4) {
        // 'ftyp', then the 'mdat' (its size set at the end, 64 bits wide from the start); the 'moov' after it.
        std::vector<uint8_t> ftyp;
        putType(ftyp, "iso6");
        put32(ftyp, 0);
        for (const char* b : {"iso6", "av02", "mp41"}) putType(ftyp, b);
        std::vector<uint8_t> head = box("ftyp", ftyp);
        s.mdatAt = head.size();
        put32(head, 1);
        putType(head, "mdat");
        put64(head, 0);
        if (!s.write(head)) return fail(error, "Cannot write " + path);
    }
    return true;
}

bool Av2Writer::addAudio(const AVCodecParameters* par, int timeBaseNum, int timeBaseDen, std::string* error) {
    State& s = *s_;
    if (s.set.container != Av2Container::Mp4) return fail(error, "IVF and OBU streams hold no sound: export AV2 as MP4 for sound, or choose no audio codec");
    if (!par || par->codec_id != AV_CODEC_ID_AAC) return fail(error, "AV2 in MP4 takes AAC sound");
    if (s.framesIn > 0 || s.hasAudio) return fail(error, "The sound is added before the pictures");
    s.hasAudio = true;
    s.audioConfig.assign(par->extradata, par->extradata + par->extradata_size);
    s.sampleRate = par->sample_rate;
    s.channels = par->ch_layout.nb_channels;
    s.audioTbNum = timeBaseNum;
    s.audioTbDen = timeBaseDen;
    s.audioBitrate = par->bit_rate;
    if (s.sampleRate <= 0 || s.channels <= 0 || timeBaseNum <= 0 || timeBaseDen <= 0) return fail(error, "The sound has no rate or channels");
    return true;
}

bool Av2Writer::audio(const AVPacket* pkt, std::string* error) {
    State& s = *s_;
    if (!s.hasAudio || !s.f.isOpen() || s.finished) return fail(error, "No sound stream to write to");
    if (pkt->size <= 0) return true;
    // (its times in samples)
    const int64_t pts = pkt->pts == AV_NOPTS_VALUE ? s.audioLastEnd : av_rescale(pkt->pts, int64_t(s.audioTbNum) * s.sampleRate, s.audioTbDen);
    const int64_t duration = pkt->duration > 0 ? av_rescale(pkt->duration, int64_t(s.audioTbNum) * s.sampleRate, s.audioTbDen) : 1024;
    if (s.audioFirstPts == INT64_MIN) s.audioFirstPts = pts;
    Sample a;
    a.at = uint64_t(s.f.pos());
    a.size = uint32_t(pkt->size);
    a.duration = uint32_t(std::max<int64_t>(1, duration));
    if (s.f.write(reinterpret_cast<const char*>(pkt->data), pkt->size) != pkt->size) return fail(error, "Cannot write " + s.path + " (is the disk full?)");
    s.sound.push_back(a);
    s.audioLastEnd = pts + duration;
    return true;
}

// A temporal unit as AVM made it, written to the container: as it is to an IVF (a frame of its own) or an OBU stream;
// to an MP4 as the binding's sample: without its temporal delimiter, content interpretation (said by 'colr') or
// padding, its sequence header in the 'av2C' box (and left out of the samples while it stays the same).
bool Av2Writer::State::unit(const uint8_t* data, size_t n, std::string* error) {
    const int64_t index = units++;
    if (index == 0) firstUnit.assign(data, data + n);
    if (set.container == Av2Container::Ivf) {
        std::vector<uint8_t> h;
        putLe32(h, uint32_t(n));
        putLe64(h, uint64_t(index));
        if (!write(h) || f.write(reinterpret_cast<const char*>(data), qint64(n)) != qint64(n)) return fail(error, "Cannot write " + path + " (is the disk full?)");
        return true;
    }
    if (set.container == Av2Container::Obu) {
        if (f.write(reinterpret_cast<const char*>(data), qint64(n)) != qint64(n)) return fail(error, "Cannot write " + path + " (is the disk full?)");
        return true;
    }
    std::vector<ObuAt> obus;
    if (!splitObus(data, n, obus)) return fail(error, "AVM gave a temporal unit that is cut off");
    std::vector<uint8_t> sample;
    bool sync = false, firstFrame = true;
    for (const ObuAt& o : obus) {
        if (o.type == kObuTemporalDelimiter || o.type == kObuContentInterpretation || o.type == kObuPadding) continue;
        if (o.type == kObuSequenceHeader) {
            const std::vector<uint8_t> sh(data + o.at, data + o.at + o.size);
            if (sequenceHeader.empty()) sequenceHeader = sh;
            if (sh == sequenceHeader) continue;
            return fail(error, "The AV2 sequence header changed during the stream, which one MP4 sample entry cannot say");
        }
        if (codesFrame(o.type) && firstFrame) {
            firstFrame = false;
            sync = o.type == kObuClosedLoopKey;
        }
        sample.insert(sample.end(), data + o.at, data + o.at + o.size);
    }
    Sample v;
    v.at = uint64_t(f.pos());
    v.size = uint32_t(sample.size());
    v.duration = uint32_t(set.fpsDen);
    v.sync = sync;
    if (!write(sample)) return fail(error, "Cannot write " + path + " (is the disk full?)");
    video.push_back(v);
    return true;
}

bool Av2Writer::State::drain(std::string* error) {
    avm_codec_iter_t it = nullptr;
    while (const avm_codec_cx_pkt_t* pkt = avm_codec_get_cx_data(&enc, &it)) {
        if (pkt->kind != AVM_CODEC_CX_FRAME_PKT) continue;
        const auto* b = static_cast<const uint8_t*>(pkt->data.frame.buf);
        pending.insert(pending.end(), b, b + pkt->data.frame.sz);
    }
    // Whole temporal units out of what has come (a packet can hold several; a unit ends where the next begins).
    std::vector<ObuAt> obus;
    if (!splitObus(pending.data(), pending.size(), obus)) return fail(error, "AVM gave OBUs that are cut off");
    size_t start = 0;
    for (size_t i = 1; i < obus.size(); ++i)
        if (obus[i].type == kObuTemporalDelimiter) {
            if (!unit(pending.data() + start, obus[i].at - start, error)) return false;
            start = obus[i].at;
        }
    pending.erase(pending.begin(), pending.begin() + ptrdiff_t(start));
    return true;
}

bool Av2Writer::encode(const AVFrame* frame, std::string* error) {
    State& s = *s_;
    if (!s.encOpen || s.finished) return fail(error, "No AV2 file is open");
    const Av2WriteSettings& st = s.set;
    const AVPixelFormat want = st.bits == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    if (frame->format != want || frame->width != st.width || frame->height != st.height)
        return fail(error, "An AV2 frame is " + std::to_string(st.width) + " x " + std::to_string(st.height) + " " + av_get_pix_fmt_name(want));
    // (AVM takes 16-bit samples whatever the depth)
    for (int c = 0; c < 3; ++c) {
        const int w = c ? (st.width + 1) / 2 : st.width, h = c ? (st.height + 1) / 2 : st.height;
        for (int y = 0; y < h; ++y) {
            auto* out = reinterpret_cast<uint16_t*>(s.img.planes[c] + size_t(y) * size_t(s.img.stride[c]));
            const uint8_t* in = frame->data[c] + size_t(y) * size_t(frame->linesize[c]);
            if (st.bits == 8)
                for (int x = 0; x < w; ++x) out[x] = in[x];
            else
                std::memcpy(out, in, size_t(w) * 2);
        }
    }
    // HDR light levels and the mastering display as metadata OBUs on each key frame, in IVF and OBU streams (an MP4
    // says them in its sample entry).
    avm_img_remove_metadata(&s.img);
    if (st.container != Av2Container::Mp4) {
        if (st.maxCll > 0) {
            const uint8_t cll[4] = {uint8_t(st.maxCll >> 8), uint8_t(st.maxCll), uint8_t(st.maxFall >> 8), uint8_t(st.maxFall)};
            avm_img_add_metadata(&s.img, OBU_METADATA_TYPE_HDR_CLL, cll, 4, AVM_MIF_KEY_FRAME);
        }
        if (st.masteringMax > 0) {
            // P3-D65's red, green and blue and white in 0.16, the luminances in 24.8 and 18.14 nits.
            std::vector<uint8_t> m;
            for (double v : {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290}) put16(m, unsigned(std::lround(v * 65536)) & 0xffff);
            put32(m, uint32_t(std::lround(st.masteringMax * 256)));
            put32(m, uint32_t(std::lround(st.masteringMin * 16384)));
            avm_img_add_metadata(&s.img, OBU_METADATA_TYPE_HDR_MDCV, m.data(), m.size(), AVM_MIF_KEY_FRAME);
        }
    }
    if (avm_codec_encode(&s.enc, &s.img, s.framesIn, 1, 0) != AVM_CODEC_OK)
        return fail(error, std::string("AV2 encoding failed: ") + avm_codec_error_detail(&s.enc));
    ++s.framesIn;
    return s.drain(error);
}

bool Av2Writer::State::moov(std::string* error) {
    // The movie: 1000 ticks a second; the video in its frame rate's, the sound in its sample rate.
    const uint32_t vScale = uint32_t(set.fpsNum);
    uint64_t vDuration = 0;
    for (const Sample& v : video) vDuration += v.duration;
    uint64_t aDuration = 0;
    for (const Sample& a : sound) aDuration += a.duration;
    const int64_t priming = hasAudio && audioFirstPts < 0 ? -audioFirstPts : 0;
    const uint64_t aShown = aDuration > uint64_t(priming) ? aDuration - uint64_t(priming) : 0;
    const uint64_t movie = std::max(vDuration * 1000 / vScale, hasAudio ? aShown * 1000 / uint64_t(sampleRate) : 0);
    const bool wide = std::max(vDuration, aDuration) > 0xffffffffULL;
    const int v1 = wide ? 1 : 0;
    auto putTime = [&](std::vector<uint8_t>& b, uint64_t t) {
        if (wide) put64(b, t);
        else put32(b, uint32_t(t));
    };
    auto sampleTables = [&](const std::vector<Sample>& samples, const std::vector<uint8_t>& stsd, bool syncTable) {
        std::vector<uint8_t> stts, stsc, stsz, chunks, stss;
        // (durations in runs)
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (const Sample& x : samples)
            if (!runs.empty() && runs.back().second == x.duration) ++runs.back().first;
            else runs.push_back({1, x.duration});
        put32(stts, uint32_t(runs.size()));
        for (const auto& r : runs) put32(stts, r.first), put32(stts, r.second);
        put32(stsc, 1), put32(stsc, 1), put32(stsc, 1), put32(stsc, 1);  // (a chunk a sample)
        put32(stsz, 0), put32(stsz, uint32_t(samples.size()));
        for (const Sample& x : samples) put32(stsz, x.size);
        const bool co64 = !samples.empty() && samples.back().at > 0xffffffffULL;
        put32(chunks, uint32_t(samples.size()));
        for (const Sample& x : samples)
            if (co64) put64(chunks, x.at);
            else put32(chunks, uint32_t(x.at));
        std::vector<uint8_t> stbl = fullBox("stsd", 0, 0, stsd) + fullBox("stts", 0, 0, stts);
        if (syncTable && std::any_of(samples.begin(), samples.end(), [](const Sample& x) { return !x.sync; })) {
            uint32_t count = 0;
            for (const Sample& x : samples) count += x.sync;
            put32(stss, count);
            for (size_t i = 0; i < samples.size(); ++i)
                if (samples[i].sync) put32(stss, uint32_t(i + 1));
            stbl = stbl + fullBox("stss", 0, 0, stss);
        }
        stbl = stbl + fullBox("stsc", 0, 0, stsc) + fullBox("stsz", 0, 0, stsz) + fullBox(co64 ? "co64" : "stco", 0, 0, chunks);
        return box("stbl", stbl);
    };
    auto dinf = [] {
        std::vector<uint8_t> dref;
        put32(dref, 1);
        return box("dinf", fullBox("dref", 0, 0, dref + fullBox("url ", 0, 1, {})));
    };
    auto tkhd = [&](uint32_t id, uint64_t duration, bool sound, uint32_t w, uint32_t h) {
        std::vector<uint8_t> b;
        putTime(b, 0), putTime(b, 0);
        put32(b, id), put32(b, 0);
        putTime(b, duration);
        put32(b, 0), put32(b, 0);
        put16(b, 0), put16(b, sound ? 1 : 0), put16(b, sound ? 0x0100 : 0), put16(b, 0);
        putMatrix(b);
        put32(b, w << 16), put32(b, h << 16);
        return fullBox("tkhd", v1, 3, b);
    };
    auto mdhd = [&](uint32_t scale, uint64_t duration) {
        std::vector<uint8_t> b;
        putTime(b, 0), putTime(b, 0);
        put32(b, scale);
        putTime(b, duration);
        put16(b, 0x55c4);  // (language "und")
        put16(b, 0);
        return fullBox("mdhd", v1, 0, b);
    };
    auto hdlr = [](const char* type, const char* name) {
        std::vector<uint8_t> b;
        put32(b, 0);
        putType(b, type);
        put32(b, 0), put32(b, 0), put32(b, 0);
        b.insert(b.end(), name, name + std::strlen(name) + 1);
        return fullBox("hdlr", 0, 0, b);
    };
    if (sequenceHeader.empty()) return fail(error, "AVM gave no sequence header");
    // (the sequence header's fields as dav2d reads them, from the first frame decoded)
    Dav2dSequenceHeader sh{};
    {
        bool got = false;
        std::string why;
        auto take = [&](Dav2dPicture& pic) {
            if (pic.seq_hdr) sh = *pic.seq_hdr, got = true;
            dav2d_picture_unref(&pic);
            return false;
        };
        if (Dav2dContext* c = openDecoder(&why)) {
            if (decodeUnit(c, &firstUnit, take, &why) && !got) decodeUnit(c, nullptr, take, &why);
            dav2d_close(&c);
        }
        if (!got) return fail(error, "AVM's first frame does not decode (dav2d)" + (why.empty() ? std::string() : ": " + why));
    }
    // The sample entry: 'av02' with its 'av2C' (as av2-tools writes the binding's draft: a version, the sequence
    // header's profile, level, tier, chroma format, depth, output order, still picture and film grain flags and its
    // largest size, then the sequence header OBU), 'colr' and, for HDR, 'mdcv' and 'clli'.
    std::vector<uint8_t> av2c;
    put8(av2c, 1);
    const unsigned chromaIdc = sh.layout == DAV2D_PIXEL_LAYOUT_I400 ? 1 : sh.layout == DAV2D_PIXEL_LAYOUT_I444 ? 2 : sh.layout == DAV2D_PIXEL_LAYOUT_I422 ? 3 : 0;
    const unsigned depthIdc = sh.hbd == 0 ? 1 : sh.hbd == 1 ? 0 : 2;
    put16(av2c, unsigned(sh.profile & 31) << 11 | unsigned(sh.level & 31) << 6 | unsigned(sh.tier & 1) << 5 | (chromaIdc & 7) << 2 | (depthIdc >> 1));
    put16(av2c, (depthIdc & 1) << 15 | unsigned(sh.monotonic & 1) << 14 | unsigned(sh.still_picture & 1) << 13 | unsigned(sh.film_grain_present & 1) << 12);
    put16(av2c, unsigned(std::max(1, sh.max_width) - 1));
    put16(av2c, unsigned(std::max(1, sh.max_height) - 1));
    av2c.insert(av2c.end(), sequenceHeader.begin(), sequenceHeader.end());
    std::vector<uint8_t> colr;
    putType(colr, "nclx");
    put16(colr, unsigned(set.primaries)), put16(colr, unsigned(set.transfer)), put16(colr, unsigned(set.matrix));
    put8(colr, set.fullRange ? 0x80 : 0);
    std::vector<uint8_t> entry;
    for (int i = 0; i < 6; ++i) put8(entry, 0);
    put16(entry, 1);  // (data reference 1)
    for (int i = 0; i < 4; ++i) put32(entry, 0);
    put16(entry, unsigned(set.width)), put16(entry, unsigned(set.height));
    put32(entry, 0x00480000), put32(entry, 0x00480000), put32(entry, 0);
    put16(entry, 1);
    const char name[] = "AV2 (AVM)";
    put8(entry, unsigned(std::strlen(name)));
    for (size_t i = 0; i < 31; ++i) put8(entry, i < std::strlen(name) ? unsigned(name[i]) : 0);
    put16(entry, 0x18), put16(entry, 0xffff);
    entry = entry + box("av2C", av2c) + box("colr", colr);
    if (set.masteringMax > 0) {
        std::vector<uint8_t> m;
        // G, B, R, white (P3-D65) in 0.00002; luminances in 0.0001 nits.
        for (double v : {0.265, 0.690, 0.150, 0.060, 0.680, 0.320, 0.3127, 0.3290}) put16(m, unsigned(std::lround(v / 0.00002)));
        put32(m, uint32_t(std::lround(set.masteringMax * 10000)));
        put32(m, uint32_t(std::lround(set.masteringMin * 10000)));
        entry = entry + box("mdcv", m);
    }
    if (set.maxCll > 0) {
        std::vector<uint8_t> c;
        put16(c, unsigned(std::min(set.maxCll, 65535))), put16(c, unsigned(std::min(set.maxFall, 65535)));
        entry = entry + box("clli", c);
    }
    std::vector<uint8_t> vstsd;
    put32(vstsd, 1);
    vstsd = vstsd + box("av02", entry);
    std::vector<uint8_t> vmhd;
    put16(vmhd, 0), put16(vmhd, 0), put16(vmhd, 0), put16(vmhd, 0);
    const std::vector<uint8_t> vtrak =
        box("trak", tkhd(1, vDuration * 1000 / vScale, false, uint32_t(set.width), uint32_t(set.height)) +
                        box("mdia", mdhd(vScale, vDuration) + hdlr("vide", "Video") +
                                        box("minf", fullBox("vmhd", 0, 1, vmhd) + dinf() + sampleTables(video, vstsd, true))));
    std::vector<uint8_t> moov;
    {
        std::vector<uint8_t> mvhd;
        putTime(mvhd, 0), putTime(mvhd, 0);
        put32(mvhd, 1000);
        putTime(mvhd, movie);
        put32(mvhd, 0x00010000), put16(mvhd, 0x0100), put16(mvhd, 0), put32(mvhd, 0), put32(mvhd, 0);
        putMatrix(mvhd);
        for (int i = 0; i < 6; ++i) put32(mvhd, 0);
        put32(mvhd, hasAudio ? 3 : 2);
        moov = fullBox("mvhd", v1, 0, mvhd) + vtrak;
    }
    if (hasAudio) {
        // 'mp4a' with its 'esds': the AudioSpecificConfig in an ES descriptor; the encoder's priming left out by the edit.
        std::vector<uint8_t> dc;
        put8(dc, 0x40);  // (MPEG-4 audio)
        put8(dc, 0x15);  // (an audio stream)
        put8(dc, 0), put16(dc, 0);
        put32(dc, uint32_t(std::max<int64_t>(0, audioBitrate))), put32(dc, uint32_t(std::max<int64_t>(0, audioBitrate)));
        std::vector<uint8_t> es;
        put16(es, 2), put8(es, 0);
        es = es + descriptor(0x04, dc + descriptor(0x05, audioConfig)) + descriptor(0x06, {0x02});
        std::vector<uint8_t> a;
        for (int i = 0; i < 6; ++i) put8(a, 0);
        put16(a, 1);
        put32(a, 0), put32(a, 0);
        put16(a, unsigned(std::min(channels, 2))), put16(a, 16), put16(a, 0), put16(a, 0);
        put32(a, uint32_t(std::min(sampleRate, 65535)) << 16);
        a = a + fullBox("esds", 0, 0, descriptor(0x03, es));
        std::vector<uint8_t> astsd;
        put32(astsd, 1);
        astsd = astsd + box("mp4a", a);
        std::vector<uint8_t> elst;
        put32(elst, 1);
        putTime(elst, aShown * 1000 / uint64_t(sampleRate));
        if (wide) put64(elst, uint64_t(priming));
        else put32(elst, uint32_t(priming));
        put16(elst, 1), put16(elst, 0);
        std::vector<uint8_t> smhd;
        put32(smhd, 0);
        moov = moov + box("trak", tkhd(2, aShown * 1000 / uint64_t(sampleRate), true, 0, 0) + box("edts", fullBox("elst", v1, 0, elst)) +
                                      box("mdia", mdhd(uint32_t(sampleRate), aDuration) + hdlr("soun", "Sound") +
                                                      box("minf", fullBox("smhd", 0, 0, smhd) + dinf() + sampleTables(sound, astsd, false))));
    }
    if (!write(box("moov", moov))) return fail(error, "Cannot write " + path + " (is the disk full?)");
    return true;
}

void Av2Writer::setLightLevels(int maxCll, int maxFall) {
    if (maxCll > 0) s_->set.maxCll = maxCll, s_->set.maxFall = maxFall;
}

bool Av2Writer::finish(std::string* error) {
    State& s = *s_;
    if (!s.encOpen || s.finished) return fail(error, "No AV2 file is open");
    // The encoder's frames held back, then the last unit.
    for (;;) {
        if (avm_codec_encode(&s.enc, nullptr, 0, 1, 0) != AVM_CODEC_OK) return fail(error, std::string("AV2 encoding failed: ") + avm_codec_error_detail(&s.enc));
        const size_t before = s.pending.size();
        const int64_t unitsBefore = s.units;
        if (!s.drain(error)) return false;
        if (s.pending.size() == before && s.units == unitsBefore) break;
    }
    if (!s.pending.empty()) {
        std::vector<uint8_t> last;
        last.swap(s.pending);
        if (!s.unit(last.data(), last.size(), error)) return false;
    }
    if (s.units != s.framesIn)
        return fail(error, "AVM gave " + std::to_string(s.units) + " temporal units for " + std::to_string(s.framesIn) + " frames");
    if (s.set.container == Av2Container::Ivf) {
        std::vector<uint8_t> n;
        putLe32(n, uint32_t(s.units));
        if (!s.f.seek(24) || !s.write(n)) return fail(error, "Cannot write " + s.path);
    } else if (s.set.container == Av2Container::Mp4) {
        const uint64_t mdatEnd = uint64_t(s.f.pos());
        if (!s.moov(error)) return false;
        std::vector<uint8_t> size;
        put64(size, mdatEnd - s.mdatAt);
        if (!s.f.seek(qint64(s.mdatAt + 8)) || !s.write(size)) return fail(error, "Cannot write " + s.path);
    }
    s.f.close();
    if (s.f.error() != QFileDevice::NoError) {
        s.f.remove();  // (as an unfinished one is)
        return fail(error, "Cannot write " + s.path);
    }
    s.finished = true;
    return true;
}

#endif

}  // namespace montage
