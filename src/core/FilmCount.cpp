#include "FilmCount.h"

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QtEndian>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace montage {

const std::vector<FilmGauge>& filmGauges() {
    static const std::vector<FilmGauge> g = {
        {"35mm-4perf", "35 mm 4-perf (16 frames a foot)", 4, 64, 64},
        {"35mm-3perf", "35 mm 3-perf (64 frames in 3 feet)", 3, 64, 64},
        {"35mm-2perf", "35 mm 2-perf (32 frames a foot)", 2, 64, 64},
        {"16mm", "16 mm (40 frames a foot)", 1, 40, 20},
        {"65mm-5perf", "65 mm 5-perf (64 frames in 5 feet)", 5, 64, 120},
    };
    return g;
}

const FilmGauge& filmGauge(const std::string& id) {
    for (const FilmGauge& g : filmGauges())
        if (id == g.id) return g;
    return filmGauges().front();
}

namespace {

// Floor division and its remainder (C++ rounds toward zero).
int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// "frames" or "frames.perf" past a mark: whole frames of the gauge, and the perforation the frame starts on where the
// spacing between marks is not a whole number of frames (or the frames do not start on the mark, as a scan's key
// number can say).
std::string pastMark(int64_t perfs, int spacing, const FilmGauge& g) {
    char buf[32];
    const int64_t frames = perfs / g.perfsPerFrame;
    if (spacing % g.perfsPerFrame == 0 && perfs % g.perfsPerFrame == 0) std::snprintf(buf, sizeof buf, "%02lld", static_cast<long long>(frames));
    else std::snprintf(buf, sizeof buf, "%02lld.%lld", static_cast<long long>(frames), static_cast<long long>(perfs % g.perfsPerFrame));
    return buf;
}

// "05" or "05.1" as perforations past a mark; false if it is not a frame of the gauge within `spacing`.
bool parsePast(const std::string& s, int spacing, const FilmGauge& g, int64_t& perfs) {
    const size_t dot = s.find('.');
    const std::string f = s.substr(0, dot), p = dot == std::string::npos ? std::string() : s.substr(dot + 1);
    if (f.empty() || f.size() > 6 || f.find_first_not_of("0123456789") != std::string::npos) return false;
    if (dot != std::string::npos && (p.empty() || p.size() > 2 || p.find_first_not_of("0123456789") != std::string::npos)) return false;
    const int64_t frames = std::stoll(f), perf = p.empty() ? 0 : std::stoll(p);
    if (perf >= g.perfsPerFrame) return false;
    perfs = frames * g.perfsPerFrame + perf;
    return perfs < spacing;
}

std::string trimmed(const std::string& s) { return QString::fromStdString(s).trimmed().toStdString(); }

}  // namespace

std::string formatFeetFrames(int64_t frame, const FilmGauge& g) {
    const bool negative = frame < 0;
    const int64_t perfs = (negative ? -frame : frame) * g.perfsPerFrame;
    return (negative ? "-" : "") + std::to_string(perfs / g.perfsPerFoot) + "+" + pastMark(perfs % g.perfsPerFoot, g.perfsPerFoot, g);
}

bool parseFeetFrames(const std::string& text, const FilmGauge& g, int64_t& frame) {
    std::string s = trimmed(text);
    const bool negative = !s.empty() && s[0] == '-';
    if (negative) s.erase(0, 1);
    const size_t plus = s.find('+');
    const std::string feet = s.substr(0, plus);
    if (feet.empty() || feet.size() > 9 || feet.find_first_not_of("0123456789") != std::string::npos) return false;
    int64_t past = 0;
    if (plus != std::string::npos && !parsePast(s.substr(plus + 1), g.perfsPerFoot, g, past)) return false;
    const int64_t perfs = std::stoll(feet) * g.perfsPerFoot + past;
    if (perfs % g.perfsPerFrame != 0) return false;  // (between frames)
    frame = (negative ? -1 : 1) * perfs / g.perfsPerFrame;
    return true;
}

int filmNumberSpacing(const FilmNumber& n, const FilmGauge& g) { return n.ink ? g.perfsPerFoot : g.perfsPerKey; }

std::string formatFilmNumber(const FilmNumber& n, const FilmGauge& g) {
    if (!n.valid()) return {};
    char count[32];
    std::snprintf(count, sizeof count, "%04lld", static_cast<long long>(n.count));
    return n.prefix + " " + count + "+" + pastMark(n.perfs, filmNumberSpacing(n, g), g);
}

bool parseFilmNumber(const std::string& text, const FilmGauge& g, FilmNumber& out, bool ink) {
    const std::string s = trimmed(text);
    const size_t plus = s.rfind('+');
    if (plus == std::string::npos) return false;
    // The count: the digits just before the "+", after a space (or the last four digits of a run written together).
    size_t start = plus;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(s[start - 1]))) --start;
    if (start == plus) return false;
    if (start > 0 && s[start - 1] != ' ') start = plus - std::min<size_t>(4, plus - start);
    FilmNumber n;
    n.ink = ink;
    n.prefix = trimmed(s.substr(0, start));
    if (n.prefix.empty() || plus - start > 12) return false;
    n.count = std::stoll(s.substr(start, plus - start));
    int64_t perfs = 0;
    if (!parsePast(s.substr(plus + 1), filmNumberSpacing(n, g), g, perfs)) return false;
    n.perfs = int(perfs);
    out = n;
    return true;
}

FilmNumber advanceFilmNumber(FilmNumber n, int64_t frames, const FilmGauge& g) {
    const int spacing = filmNumberSpacing(n, g);
    const int64_t perfs = int64_t(n.perfs) + frames * g.perfsPerFrame;
    n.count += floorDiv(perfs, spacing);
    n.perfs = int(floorMod(perfs, spacing));
    return n;
}

std::optional<int64_t> filmNumberFrames(const FilmNumber& a, const FilmNumber& b, const FilmGauge& g) {
    if (!a.valid() || a.prefix != b.prefix || a.ink != b.ink) return std::nullopt;
    const int spacing = filmNumberSpacing(a, g);
    const int64_t perfs = (b.count - a.count) * spacing + (b.perfs - a.perfs);
    return floorDiv(perfs, g.perfsPerFrame);
}

const FilmGauge& mediaFilmGauge(const MediaItem& m, const FilmGauge& fallback) {
    const auto it = m.metadata.find("film_gauge");
    if (it != m.metadata.end())
        for (const FilmGauge& g : filmGauges())
            if (it->second == g.id) return g;
    return fallback;
}

std::string mediaFilmNumberText(const MediaItem& m, bool ink, int64_t frame, const FilmGauge& fallback) {
    const FilmGauge& g = mediaFilmGauge(m, fallback);
    const auto n = mediaFilmNumber(m, ink, frame, g);
    return n ? formatFilmNumber(*n, g) : std::string();
}

std::optional<FilmNumber> mediaFilmNumber(const MediaItem& m, bool ink, int64_t frame, const FilmGauge& g) {
    const auto it = m.metadata.find(ink ? "ink_number" : "key_number");
    FilmNumber first;
    if (it == m.metadata.end() || !parseFilmNumber(it->second, g, first, ink)) return std::nullopt;
    return advanceFilmNumber(first, frame, g);
}

int64_t clipMediaFrame(const Clip& c, FrameTime t, const Sequence& s, const MediaItem& m) {
    const double perSeq = m.fps.valid() && s.fps.valid() ? m.fps.toDouble() / s.fps.toDouble() : 1.0;
    return int64_t(std::floor(c.sourceFrameAt(t) * perSeq + 1e-6));
}

bool readFilmScanHeader(const std::string& path, FilmScanHeader& out) {
    out = FilmScanHeader{};
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray d = f.read(2048);
    if (d.size() < 4) return false;
    const auto* p = reinterpret_cast<const uchar*>(d.constData());
    // A text field: printable ASCII up to the first NUL (unset fields are NULs or 0xFF).
    auto text = [&](int at, int len) {
        std::string s;
        for (int i = at; i < at + len && i < d.size(); ++i) {
            const uchar c = p[i];
            if (c == 0 || c == 0xff) break;
            if (c < 32 || c > 126) return std::string();
            s += char(c);
        }
        return trimmed(s);
    };
    auto digits = [](const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
    const uint32_t magic = qFromBigEndian<quint32>(p);
    if (magic == 0x53445058 || magic == 0x58504453) {  // "SDPX" big-endian, "XPDS" little-endian
        if (d.size() < 1920) return false;
        const bool be = magic == 0x53445058;
        auto u32 = [&](int at) { return be ? qFromBigEndian<quint32>(p + at) : qFromLittleEndian<quint32>(p + at); };
        auto r32 = [&](int at) {
            const quint32 v = u32(at);
            float x;
            std::memcpy(&x, &v, 4);
            return v == 0xffffffffu || !std::isfinite(x) ? 0.0 : double(x);
        };
        const std::string mfr = text(1664, 2), type = text(1666, 2), offset = text(1668, 2), prefix = text(1670, 6),
                          count = text(1676, 4);
        if (!mfr.empty() && !type.empty() && digits(prefix) && prefix.size() == 6 && digits(count)) {
            out.hasKeyKode = true;
            out.key.prefix = mfr + type + " " + prefix.substr(0, 2) + " " + prefix.substr(2);
            out.key.count = std::stoll(count);
            out.key.perfs = digits(offset) ? std::stoi(offset) : 0;
        }
        out.format = text(1680, 32);
        if (const quint32 pos = u32(1712); pos != 0xffffffffu) out.framePosition = pos;
        out.frameRate = r32(1724);
        out.frameId = text(1732, 32);
        out.slate = text(1764, 100);
        return true;
    }
    if (magic == 0x802a5fd7 || magic == 0xd75f2a80) {  // Cineon
        if (d.size() < 1308) return false;
        const bool be = magic == 0x802a5fd7;
        auto u32 = [&](int at) { return be ? qFromBigEndian<quint32>(p + at) : qFromLittleEndian<quint32>(p + at); };
        const uchar mfr = p[1024], type = p[1025], offset = p[1026];
        const quint32 prefix = u32(1028), count = u32(1032);
        if (mfr != 0xff && type != 0xff && prefix != 0xffffffffu && count != 0xffffffffu && prefix < 1000000 && count < 10000) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%02u%02u %02u %04u", unsigned(mfr), unsigned(type), unsigned(prefix / 10000), unsigned(prefix % 10000));
            out.hasKeyKode = true;
            out.key.prefix = buf;
            out.key.count = count;
            out.key.perfs = offset != 0xff ? offset : 0;
        }
        out.format = text(1036, 32);
        if (const quint32 pos = u32(1068); pos != 0xffffffffu) out.framePosition = pos;
        {
            const quint32 v = u32(1072);
            float x;
            std::memcpy(&x, &v, 4);
            out.frameRate = v == 0xffffffffu || !std::isfinite(x) ? 0.0 : double(x);
        }
        out.frameId = text(1076, 32);
        out.slate = text(1108, 200);
        return true;
    }
    return false;
}

const FilmGauge& filmScanGauge(const FilmScanHeader& h, const FilmScanHeader* next) {
    if (next && h.hasKeyKode && next->hasKeyKode && h.key.prefix == next->key.prefix)
        for (const FilmGauge& g : filmGauges())
            if ((next->key.count - h.key.count) * g.perfsPerKey + (next->key.perfs - h.key.perfs) == g.perfsPerFrame) return g;
    static const QRegularExpression sixteen(QStringLiteral("\\b(16 ?mm|super ?16|s16)\\b"), QRegularExpression::CaseInsensitiveOption);
    if (QString::fromStdString(h.format).contains(sixteen)) return filmGauge("16mm");
    return filmGauge(h.key.perfs % 4 == 0 ? "35mm-4perf" : "35mm-3perf");
}

std::string filmScanKeyNumber(const FilmScanHeader& h, const FilmGauge& g) {
    if (!h.hasKeyKode) return {};
    // (the header's perforation offset, carried past the next mark if it is that far)
    FilmNumber n = h.key;
    const int spacing = filmNumberSpacing(n, g);
    n.count += n.perfs / spacing;
    n.perfs %= spacing;
    return formatFilmNumber(n, g);
}

}  // namespace montage
