#include "SpatialAudio.h"

#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace montage {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
void put64(uint8_t* p, uint64_t v) {
    put32(p, uint32_t(v >> 32));
    put32(p + 4, uint32_t(v));
}
constexpr uint32_t fourcc(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}

struct Box {
    size_t start = 0;  // in the buffer (or the file, for top-level boxes)
    uint64_t size = 0;
    uint32_t type = 0;
    size_t header = 8;
};

// The boxes one after another in [from, to); stops at anything that does not fit.
std::vector<Box> boxesIn(const uint8_t* b, size_t from, size_t to) {
    std::vector<Box> out;
    size_t pos = from;
    while (to >= pos + 8) {
        Box x;
        x.start = pos;
        x.size = be32(b + pos);
        x.type = be32(b + pos + 4);
        if (x.size == 1) {
            if (to < pos + 16) break;
            x.size = be64(b + pos + 8);
            x.header = 16;
        } else if (x.size == 0) {
            x.size = to - pos;
        }
        if (x.size < x.header || x.size > to - pos) break;
        out.push_back(x);
        pos += size_t(x.size);
    }
    return out;
}

std::optional<Box> child(const std::vector<Box>& boxes, uint32_t type) {
    for (const Box& x : boxes)
        if (x.type == type) return x;
    return std::nullopt;
}
std::vector<Box> inside(const uint8_t* b, const Box& x, size_t skip = 0) {
    return boxesIn(b, x.start + x.header + skip, x.start + size_t(x.size));
}

// The top-level boxes of the file (offsets in the file).
std::vector<Box> topLevel(QFile& f) {
    std::vector<Box> out;
    const qint64 n = f.size();
    qint64 pos = 0;
    uint8_t h[16];
    while (pos + 8 <= n) {
        if (!f.seek(pos) || f.read(reinterpret_cast<char*>(h), 8) != 8) break;
        Box x;
        x.start = size_t(pos);
        x.size = be32(h);
        x.type = be32(h + 4);
        if (x.size == 1) {
            if (f.read(reinterpret_cast<char*>(h + 8), 8) != 8) break;
            x.size = be64(h + 8);
            x.header = 16;
        } else if (x.size == 0) {
            x.size = uint64_t(n - pos);
        }
        if (x.size < x.header || x.size > uint64_t(n - pos)) break;
        out.push_back(x);
        pos += qint64(x.size);
    }
    return out;
}

// The path from moov down to the first audio track's first sample entry (moov's own box excluded: the buffer is its
// contents with its header).
struct AudioEntry {
    std::vector<Box> path;  // trak, mdia, minf, stbl, stsd, entry
};
bool firstAudioEntry(const std::vector<uint8_t>& moov, AudioEntry& out) {
    const uint8_t* b = moov.data();
    const Box root{0, moov.size(), fourcc("moov"), size_t(be32(b) == 1 ? 16 : 8)};
    for (const Box& trak : inside(b, root)) {
        if (trak.type != fourcc("trak")) continue;
        const auto t = inside(b, trak);
        const auto mdia = child(t, fourcc("mdia"));
        if (!mdia) continue;
        const auto m = inside(b, *mdia);
        const auto hdlr = child(m, fourcc("hdlr"));
        // hdlr: version and flags, pre_defined, then the handler type.
        if (!hdlr || hdlr->size < hdlr->header + 12 || be32(b + hdlr->start + hdlr->header + 8) != fourcc("soun")) continue;
        const auto minf = child(m, fourcc("minf"));
        if (!minf) continue;
        const auto stbl = child(inside(b, *minf), fourcc("stbl"));
        if (!stbl) continue;
        const auto stsd = child(inside(b, *stbl), fourcc("stsd"));
        if (!stsd || stsd->size < stsd->header + 8) continue;
        const auto entries = inside(b, *stsd, 8);  // after version, flags and the entry count
        if (entries.empty()) continue;
        out.path = {trak, *mdia, *minf, *stbl, *stsd, entries.front()};
        return true;
    }
    return false;
}

// Where an SA3D box sits in a sample entry (its children follow fields whose length depends on the entry's version,
// so the box is found by its type and a size that fits), or 0.
size_t findSa3d(const std::vector<uint8_t>& moov, const Box& entry) {
    const size_t end = entry.start + size_t(entry.size);
    for (size_t i = entry.start + entry.header + 28; i + 8 <= end; ++i)
        if (be32(moov.data() + i + 4) == fourcc("SA3D")) {
            const uint32_t size = be32(moov.data() + i);
            if (size >= 8 + 12 && i + size <= end) return i;
        }
    return 0;
}

bool readMoov(QFile& f, Box& moov, std::vector<uint8_t>& buf, std::vector<Box>* top = nullptr) {
    const std::vector<Box> boxes = topLevel(f);
    const auto m = child(boxes, fourcc("moov"));
    if (!m || m->size > (uint64_t(1) << 30)) return false;
    moov = *m;
    buf.resize(size_t(m->size));
    if (!f.seek(qint64(m->start)) || f.read(reinterpret_cast<char*>(buf.data()), qint64(buf.size())) != qint64(buf.size())) return false;
    if (top) *top = boxes;
    return true;
}

void addToSize(std::vector<uint8_t>& b, const Box& x, uint64_t delta) {
    if (x.header == 16) put64(b.data() + x.start + 8, x.size + delta);
    else put32(b.data() + x.start, uint32_t(x.size + delta));
}

}  // namespace

int readSpatialAudioBox(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    Box moov;
    std::vector<uint8_t> buf;
    AudioEntry e;
    if (!readMoov(f, moov, buf) || !firstAudioEntry(buf, e)) return 0;
    const size_t at = findSa3d(buf, e.path.back());
    // SA3D: version, ambisonic type, then the order.
    return at ? int(be32(buf.data() + at + 8 + 2)) : 0;
}

bool writeSpatialAudioBox(const std::string& path, int order, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (order < 1 || order > 7) return fail("Ambisonic orders are 1 to 7");
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail("Cannot open " + path);
    Box moov;
    std::vector<uint8_t> buf;
    std::vector<Box> top;
    AudioEntry e;
    if (!readMoov(f, moov, buf, &top)) return fail("Not an MP4 or MOV file: " + path);
    if (!firstAudioEntry(buf, e)) return fail("The file has no audio track");
    const Box& entry = e.path.back();
    if (findSa3d(buf, entry)) return true;  // already says so
    // The box: version 0, periphonic, the order, ACN order, SN3D, the channels each in its own place.
    const uint32_t channels = uint32_t((order + 1) * (order + 1));
    std::vector<uint8_t> sa3d(8 + 12 + 4 * size_t(channels), 0);
    put32(sa3d.data(), uint32_t(sa3d.size()));
    put32(sa3d.data() + 4, fourcc("SA3D"));
    put32(sa3d.data() + 10, uint32_t(order));
    put32(sa3d.data() + 16, channels);
    for (uint32_t c = 0; c < channels; ++c) put32(sa3d.data() + 20 + 4 * c, c);
    const uint64_t delta = sa3d.size();
    // A movie header before the media data moves them: every chunk offset beyond it grows by the box.
    bool dataAfter = false;
    for (const Box& x : top) dataAfter |= x.type == fourcc("mdat") && x.start > moov.start;
    if (dataAfter) {
        const uint8_t* b = buf.data();
        const Box root{0, buf.size(), fourcc("moov"), moov.header};
        for (const Box& trak : inside(b, root)) {
            if (trak.type != fourcc("trak")) continue;
            const auto mdia = child(inside(b, trak), fourcc("mdia"));
            const auto minf = mdia ? child(inside(b, *mdia), fourcc("minf")) : std::nullopt;
            const auto stbl = minf ? child(inside(b, *minf), fourcc("stbl")) : std::nullopt;
            if (!stbl) continue;
            for (const Box& x : inside(b, *stbl)) {
                if (x.type != fourcc("stco") && x.type != fourcc("co64")) continue;
                const size_t head = x.start + x.header;
                if (x.size < x.header + 8) continue;
                const uint32_t n = be32(buf.data() + head + 4);
                const size_t width = x.type == fourcc("co64") ? 8 : 4;
                if (head + 8 + size_t(n) * width > x.start + size_t(x.size)) continue;
                for (uint32_t k = 0; k < n; ++k) {
                    uint8_t* v = buf.data() + head + 8 + size_t(k) * width;
                    if (width == 8) {
                        if (be64(v) >= moov.start) put64(v, be64(v) + delta);
                    } else if (be32(v) >= moov.start) {
                        if (uint64_t(be32(v)) + delta > 0xffffffffULL) return fail("The file is too large to add spatial audio metadata to");
                        put32(v, uint32_t(be32(v) + delta));
                    }
                }
            }
        }
    }
    // Every box holding the entry grows by it, and the box goes at the entry's end.
    addToSize(buf, Box{0, buf.size(), fourcc("moov"), moov.header}, delta);
    for (const Box& x : e.path) addToSize(buf, x, delta);
    buf.insert(buf.begin() + std::ptrdiff_t(entry.start + size_t(entry.size)), sa3d.begin(), sa3d.end());
    // The file again: what came before the movie header, the new header, and what came after.
    QSaveFile out(QString::fromStdString(path));
    if (!out.open(QIODevice::WriteOnly)) return fail("Cannot write " + path);
    auto copy = [&](qint64 from, qint64 length) {
        if (!f.seek(from)) return false;
        std::vector<char> chunk(1 << 20);
        while (length > 0) {
            const qint64 n = std::min<qint64>(length, qint64(chunk.size()));
            if (f.read(chunk.data(), n) != n || out.write(chunk.data(), n) != n) return false;
            length -= n;
        }
        return true;
    };
    const qint64 after = qint64(moov.start + size_t(moov.size));
    if (!copy(0, qint64(moov.start)) || out.write(reinterpret_cast<const char*>(buf.data()), qint64(buf.size())) != qint64(buf.size()) ||
        !copy(after, f.size() - after))
        return fail("Cannot write " + path);
    f.close();
    if (!out.commit()) return fail("Cannot write " + path);
    return true;
}

}  // namespace montage
