#include "media/Psd.h"

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/Zip.h"
#include "media/Decoder.h"

namespace montage {

namespace {

// Big-endian reading over the file's bytes; reads past the end leave `bad` set and return zeros.
struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0, pos = 0;
    bool bad = false;
    bool need(uint64_t k) {
        if (bad || k > n || pos > n - size_t(k)) {
            bad = true;
            return false;
        }
        return true;
    }
    uint64_t be(int bytes) {
        if (!need(size_t(bytes))) return 0;
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | p[pos++];
        return v;
    }
    uint8_t u8() { return uint8_t(be(1)); }
    uint16_t u16() { return uint16_t(be(2)); }
    int16_t s16() { return int16_t(u16()); }
    uint32_t u32() { return uint32_t(be(4)); }
    int32_t s32() { return int32_t(u32()); }
    uint64_t len(bool psb) { return be(psb ? 8 : 4); }
    std::string str(size_t k) {
        if (!need(k)) return {};
        std::string s(reinterpret_cast<const char*>(p + pos), k);
        pos += k;
        return s;
    }
    void skip(uint64_t k) {
        if (need(k)) pos += size_t(k);
    }
};

struct Channel {
    int id = 0;
    size_t offset = 0, size = 0;  // its compression word and data
};

struct Record {
    PsdLayer layer;
    std::vector<Channel> channels;
    int maskLeft = 0, maskTop = 0, maskRight = 0, maskBottom = 0;
    int maskDefault = 255;
    bool maskOn = false;
};

struct Parsed {
    PsdInfo info;
    std::vector<Record> records;
    size_t merged = 0;  // where the merged picture's data starts
};

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

// Additional layer information whose length is 8 bytes in a PSB.
bool longKey(const std::string& key) {
    static const std::set<std::string> keys = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    return keys.count(key) > 0;
}

bool parse(const std::vector<uint8_t>& bytes, Parsed& out, std::string* error) {
    Reader r{bytes.data(), bytes.size()};
    if (r.str(4) != "8BPS") return fail(error, "Not a Photoshop file");
    const int version = r.u16();
    if (version != 1 && version != 2) return fail(error, "Unknown Photoshop file version");
    PsdInfo& info = out.info;
    info.psb = version == 2;
    r.skip(6);
    info.channels = r.u16();
    info.height = int(r.u32());
    info.width = int(r.u32());
    info.depth = r.u16();
    info.mode = r.u16();
    if (r.bad || info.width <= 0 || info.height <= 0) return fail(error, "The Photoshop file is cut short");
    if (info.mode != 1 && info.mode != 3 && info.mode != 4) return fail(error, "Only RGB, greyscale and CMYK Photoshop files can be read");
    if (info.depth != 8 && info.depth != 16) return fail(error, "Only 8- and 16-bit Photoshop files can be read in layers");
    r.skip(r.u32());  // colour mode data
    r.skip(r.u32());  // image resources
    const uint64_t sectionLength = r.len(info.psb);
    const size_t sectionEnd = r.pos + size_t(sectionLength);
    if (sectionLength > 0) {
        const uint64_t layersLength = r.len(info.psb);
        const size_t layersEnd = r.pos + size_t(layersLength);
        if (layersLength > 0) {
            const int count = std::abs(int(r.s16()));
            for (int i = 0; i < count && !r.bad; ++i) {
                Record rec;
                PsdLayer& l = rec.layer;
                l.index = i;
                l.top = r.s32(), l.left = r.s32(), l.bottom = r.s32(), l.right = r.s32();
                const int nch = r.u16();
                for (int c = 0; c < nch; ++c) {
                    Channel ch;
                    ch.id = r.s16();
                    ch.size = size_t(r.len(info.psb));
                    rec.channels.push_back(ch);
                }
                if (r.str(4) != "8BIM") return fail(error, "The Photoshop file's layers are damaged");
                l.blend = r.str(4);
                l.opacity = r.u8() / 255.0;
                l.clipped = r.u8() != 0;
                const uint8_t flags = r.u8();
                l.visible = !(flags & 0x02);
                r.skip(1);
                const uint32_t extra = r.u32();
                const size_t extraEnd = r.pos + extra;
                // The layer mask.
                const uint32_t maskLength = r.u32();
                const size_t maskEnd = r.pos + maskLength;
                if (maskLength >= 18) {
                    rec.maskTop = r.s32(), rec.maskLeft = r.s32(), rec.maskBottom = r.s32(), rec.maskRight = r.s32();
                    rec.maskDefault = r.u8();
                    const uint8_t maskFlags = r.u8();
                    rec.maskOn = !(maskFlags & 0x02) && rec.maskRight > rec.maskLeft && rec.maskBottom > rec.maskTop;
                }
                r.pos = std::min(maskEnd, r.n);
                r.skip(r.u32());  // blending ranges
                const int nameLength = r.u8();
                l.name = r.str(size_t(nameLength));
                r.skip(size_t((4 - (1 + nameLength) % 4) % 4));
                // Additional information: the Unicode name, group records, fill opacity, adjustments.
                double fill = 1;
                while (!r.bad && r.pos + 12 <= extraEnd) {
                    const std::string sig = r.str(4);
                    if (sig != "8BIM" && sig != "8B64") break;
                    const std::string key = r.str(4);
                    const uint64_t length = info.psb && longKey(key) ? r.be(8) : r.u32();
                    const size_t start = r.pos, end = start + size_t(length);
                    if (key == "luni" && length >= 4) {
                        const uint32_t chars = r.u32();
                        QString name;
                        for (uint32_t k = 0; k < chars && r.pos + 2 <= end; ++k) name += QChar(r.u16());
                        while (name.endsWith(QChar(0))) name.chop(1);
                        l.name = name.toStdString();
                    } else if ((key == "lsct" || key == "lsdk") && length >= 4) {
                        const uint32_t type = r.u32();
                        l.isGroup = type == 1 || type == 2;
                        l.isGroupEnd = type == 3;
                        if (length >= 12 && r.str(4) == "8BIM") l.blend = r.str(4);  // a group's own blend ("pass")
                    } else if (key == "iOpa" && length >= 1) {
                        fill = r.u8() / 255.0;
                    } else {
                        static const std::set<std::string> adjustments = {"levl", "curv", "brit", "blnc", "hue2", "hue ", "selc", "mixr",
                                                                           "grdm", "expA", "vibA", "post", "thrs", "nvrt", "phfl", "clrL",
                                                                           "SoCo", "GdFl", "PtFl", "blwh"};
                        if (adjustments.count(key)) l.adjustment = true;
                    }
                    r.pos = std::min(end, r.n);
                }
                l.opacity *= fill;
                r.pos = std::min(extraEnd, r.n);
                out.records.push_back(std::move(rec));
            }
            // Each layer's channel data, in order.
            for (Record& rec : out.records)
                for (Channel& ch : rec.channels) {
                    ch.offset = r.pos;
                    r.skip(ch.size);
                }
        }
        r.pos = std::min(layersEnd, r.n);
    }
    r.pos = std::min(sectionEnd, r.n);
    out.merged = r.pos;
    if (r.bad) return fail(error, "The Photoshop file is cut short");
    // Groups: a group's own record comes above (after) its layers, the record closing it below (before) them.
    std::vector<int> open;
    for (int i = int(out.records.size()) - 1; i >= 0; --i) {
        PsdLayer& l = out.records[size_t(i)].layer;
        if (l.isGroupEnd) {
            if (!open.empty()) open.pop_back();
            continue;
        }
        l.group = open.empty() ? -1 : open.back();
        if (l.isGroup) open.push_back(i);
    }
    for (const Record& rec : out.records) info.layers.push_back(rec.layer);
    return true;
}

bool loadFile(const std::string& path, std::vector<uint8_t>& bytes, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Cannot open " + path);
    const QByteArray all = f.readAll();
    bytes.assign(all.begin(), all.end());
    return true;
}

// One channel's samples (w x h, big-endian 8 or 16 bits as 16-bit values) from its compression word onwards.
bool decodeChannel(const std::vector<uint8_t>& bytes, size_t offset, size_t size, int w, int h, int depth, bool psb,
                   std::vector<uint16_t>& out, std::string* error) {
    const size_t bps = size_t(depth / 8), rowBytes = size_t(w) * bps, total = rowBytes * size_t(h);
    out.assign(size_t(w) * size_t(h), 0);
    if (w <= 0 || h <= 0) return true;
    Reader r{bytes.data(), std::min(bytes.size(), offset + size)};
    r.pos = offset;
    const int compression = r.u16();
    std::vector<uint8_t> raw;
    if (compression == 0) {
        if (!r.need(total)) return fail(error, "A layer of the Photoshop file is cut short");
        raw.assign(r.p + r.pos, r.p + r.pos + total);
    } else if (compression == 1) {
        std::vector<size_t> counts(static_cast<size_t>(h));
        for (int y = 0; y < h; ++y) counts[size_t(y)] = psb ? r.u32() : r.u16();
        raw.reserve(total);
        for (int y = 0; y < h && !r.bad; ++y) {
            const size_t rowEnd = r.pos + counts[size_t(y)], before = raw.size();
            while (r.pos < rowEnd && !r.bad && raw.size() - before < rowBytes) {
                const int8_t n = int8_t(r.u8());
                if (n >= 0) {
                    for (int k = 0; k <= n && r.pos < rowEnd; ++k) raw.push_back(r.u8());
                } else if (n != -128) {
                    const uint8_t v = r.u8();
                    raw.insert(raw.end(), size_t(1 - n), v);
                }
            }
            raw.resize(before + rowBytes, 0);
            r.pos = rowEnd;
        }
        if (r.bad) return fail(error, "A layer of the Photoshop file is damaged");
    } else if (compression == 2 || compression == 3) {
        // zlib: a two-byte header before the DEFLATE data.
        if (!r.need(2) || size < 4) return fail(error, "A layer of the Photoshop file is damaged");
        std::string inflated;
        if (!inflateRaw(r.p + r.pos + 2, size - 4, inflated, total) || inflated.size() < total)
            return fail(error, "A compressed layer of the Photoshop file could not be read");
        raw.assign(inflated.begin(), inflated.begin() + std::ptrdiff_t(total));
        if (compression == 3)  // each sample stored as the difference from the one before it on the row
            for (int y = 0; y < h; ++y) {
                uint8_t* row = raw.data() + size_t(y) * rowBytes;
                if (bps == 1)
                    for (int x = 1; x < w; ++x) row[x] = uint8_t(row[x] + row[x - 1]);
                else
                    for (int x = 1; x < w; ++x) {
                        const uint16_t v = uint16_t(((row[2 * x] << 8) | row[2 * x + 1]) + ((row[2 * x - 2] << 8) | row[2 * x - 1]));
                        row[2 * x] = uint8_t(v >> 8), row[2 * x + 1] = uint8_t(v);
                    }
            }
    } else {
        return fail(error, "A layer of the Photoshop file uses an unknown compression");
    }
    for (size_t i = 0; i < out.size(); ++i) out[i] = bps == 1 ? uint16_t(raw[i] * 257) : uint16_t((raw[2 * i] << 8) | raw[2 * i + 1]);
    return true;
}

// Colour channels to 16-bit RGB at one sample.
void toRgb(const PsdInfo& info, const std::vector<const std::vector<uint16_t>*>& colour, size_t i, uint16_t* rgb) {
    auto at = [&](size_t c) -> uint32_t { return c < colour.size() && colour[c] ? (*colour[c])[i] : 0; };
    if (info.mode == 1) {
        rgb[0] = rgb[1] = rgb[2] = uint16_t(at(0));
    } else if (info.mode == 4) {  // stored inverted: 65535 is no ink
        const uint32_t k = at(3);
        for (int c = 0; c < 3; ++c) rgb[c] = uint16_t(at(size_t(c)) * k / 65535);
    } else {
        for (int c = 0; c < 3; ++c) rgb[c] = uint16_t(at(size_t(c)));
    }
}

}  // namespace

bool readPsdInfo(const std::string& path, PsdInfo& info, std::string* error) {
    std::vector<uint8_t> bytes;
    Parsed parsed;
    if (!loadFile(path, bytes, error) || !parse(bytes, parsed, error)) return false;
    info = parsed.info;
    return true;
}

bool readPsdPixels(const std::string& path, int layer, PsdInfo& info, std::vector<uint16_t>& rgba, std::string* error) {
    std::vector<uint8_t> bytes;
    Parsed parsed;
    if (!loadFile(path, bytes, error) || !parse(bytes, parsed, error)) return false;
    info = parsed.info;
    const int W = info.width, H = info.height;
    const int colours = info.mode == 1 ? 1 : info.mode == 4 ? 4 : 3;
    rgba.assign(size_t(W) * size_t(H) * 4, 0);
    if (layer < 0) {
        // The merged picture: every channel's rows one after another.
        Reader r{bytes.data(), bytes.size()};
        r.pos = parsed.merged;
        const int compression = r.u16();
        const size_t planeBytes = size_t(W) * size_t(H) * size_t(info.depth / 8);
        std::vector<std::vector<uint16_t>> planes(size_t(info.channels));
        if (compression == 1) {
            // Row byte counts for every row of every channel first, then the rows: decode each plane as one channel.
            const size_t countBytes = size_t(info.psb ? 4 : 2);
            std::vector<size_t> rows(size_t(info.channels) * size_t(H));
            for (size_t& c : rows) c = info.psb ? r.u32() : r.u16();
            size_t data = r.pos;
            for (int c = 0; c < info.channels && !r.bad; ++c) {
                size_t length = 0;
                for (int y = 0; y < H; ++y) length += rows[size_t(c) * size_t(H) + size_t(y)];
                // Rebuild a stand-alone channel (compression word, its counts, its rows) to decode.
                std::vector<uint8_t> one = {0, 1};
                for (int y = 0; y < H; ++y) {
                    const size_t n = rows[size_t(c) * size_t(H) + size_t(y)];
                    if (countBytes == 4) one.push_back(uint8_t(n >> 24)), one.push_back(uint8_t(n >> 16));
                    one.push_back(uint8_t(n >> 8)), one.push_back(uint8_t(n));
                }
                if (data + length > bytes.size()) return fail(error, "The Photoshop file is cut short");
                one.insert(one.end(), bytes.begin() + std::ptrdiff_t(data), bytes.begin() + std::ptrdiff_t(data + length));
                if (!decodeChannel(one, 0, one.size(), W, H, info.depth, info.psb, planes[size_t(c)], error)) return false;
                data += length;
            }
        } else if (compression == 0) {
            for (int c = 0; c < info.channels; ++c) {
                std::vector<uint8_t> one = {0, 0};
                const size_t at = r.pos + size_t(c) * planeBytes;
                if (at + planeBytes > bytes.size()) return fail(error, "The Photoshop file is cut short");
                one.insert(one.end(), bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + planeBytes));
                if (!decodeChannel(one, 0, one.size(), W, H, info.depth, info.psb, planes[size_t(c)], error)) return false;
            }
        } else {
            return fail(error, "The Photoshop file's merged picture uses an unknown compression");
        }
        std::vector<const std::vector<uint16_t>*> colour;
        for (int c = 0; c < colours && c < info.channels; ++c) colour.push_back(&planes[size_t(c)]);
        const std::vector<uint16_t>* alpha = info.channels > colours ? &planes[size_t(colours)] : nullptr;
        for (size_t i = 0; i < size_t(W) * size_t(H); ++i) {
            toRgb(info, colour, i, &rgba[i * 4]);
            rgba[i * 4 + 3] = alpha ? (*alpha)[i] : 65535;
        }
        return true;
    }
    if (layer >= int(parsed.records.size())) return fail(error, "The Photoshop file has no layer " + std::to_string(layer));
    const Record& rec = parsed.records[size_t(layer)];
    const PsdLayer& l = rec.layer;
    if (!l.hasPixels()) return true;  // nothing to draw
    const int w = l.right - l.left, h = l.bottom - l.top;
    std::map<int, std::vector<uint16_t>> ch;
    for (const Channel& c : rec.channels) {
        if (c.id < -2) continue;  // a vector mask's pixels
        const bool mask = c.id == -2;
        const int cw = mask ? rec.maskRight - rec.maskLeft : w, chh = mask ? rec.maskBottom - rec.maskTop : h;
        if (!decodeChannel(bytes, c.offset, c.size, cw, chh, info.depth, info.psb, ch[c.id], error)) return false;
    }
    std::vector<const std::vector<uint16_t>*> colour;
    for (int c = 0; c < colours; ++c) colour.push_back(ch.count(c) ? &ch[c] : nullptr);
    const std::vector<uint16_t>* alpha = ch.count(-1) ? &ch[-1] : nullptr;
    const std::vector<uint16_t>* mask = rec.maskOn && ch.count(-2) ? &ch[-2] : nullptr;
    const int mw = rec.maskRight - rec.maskLeft;
    for (int y = std::max(0, l.top); y < std::min(H, l.bottom); ++y)
        for (int x = std::max(0, l.left); x < std::min(W, l.right); ++x) {
            const size_t i = size_t(y - l.top) * size_t(w) + size_t(x - l.left), o = (size_t(y) * size_t(W) + size_t(x)) * 4;
            toRgb(info, colour, i, &rgba[o]);
            uint32_t a = alpha ? (*alpha)[i] : 65535;
            if (mask) {
                const bool inside = x >= rec.maskLeft && x < rec.maskRight && y >= rec.maskTop && y < rec.maskBottom;
                const uint32_t m = inside ? (*mask)[size_t(y - rec.maskTop) * size_t(mw) + size_t(x - rec.maskLeft)] : uint32_t(rec.maskDefault * 257);
                a = a * m / 65535;
            }
            rgba[o + 3] = uint16_t(a);
        }
    return true;
}

bool isPsdFile(const std::string& path) {
    const QString s = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    return s == "psd" || s == "psb";
}

bool parsePsdLayerPath(const std::string& path, std::string& file, int& layer) {
    const size_t at = path.rfind("#layer=");
    if (at == std::string::npos || !isPsdFile(path.substr(0, at))) return false;
    file = path.substr(0, at);
    layer = std::atoi(path.c_str() + at + 7);
    return layer >= 0;
}

std::string psdLayerPath(const std::string& file, int layer) { return file + "#layer=" + std::to_string(layer); }

std::string psdBlendMode(const std::string& key) {
    static const std::map<std::string, std::string> modes = {
        {"norm", "normal"},     {"diss", "normal"},      {"pass", "normal"},     {"mul ", "multiply"},   {"lbrn", "color_burn"},
        {"idiv", "color_burn"}, {"dark", "darken"},      {"dkCl", "darken"},     {"scrn", "screen"},     {"div ", "color_dodge"},
        {"lddg", "add"},        {"lite", "lighten"},     {"lgCl", "lighten"},    {"over", "overlay"},    {"sLit", "soft_light"},
        {"hLit", "hard_light"}, {"vLit", "hard_light"},  {"lLit", "hard_light"}, {"pLit", "hard_light"}, {"hMix", "hard_light"},
        {"diff", "difference"}, {"smud", "difference"},  {"fsub", "subtract"}};
    const auto it = modes.find(key);
    return it == modes.end() ? std::string("normal") : it->second;
}

std::vector<Id> importPsd(Project& p, const std::string& path, PsdImport mode, double seconds, std::string* error) {
    std::vector<Id> made;
    const QFileInfo fi(QString::fromStdString(path));
    const std::string base = fi.completeBaseName().toStdString();
    if (mode == PsdImport::Merged) {
        MediaItem m;
        m.id = p.newId();
        if (!probeMedia(path, m, error)) return {};
        p.media.push_back(m);
        return {m.id};
    }
    PsdInfo info;
    if (!readPsdInfo(path, info, error)) return {};
    // Each layer with pixels a still of its own, in a bin of the file's name.
    const std::string bin = base + " Layers";
    if (std::find(p.bins.begin(), p.bins.end(), bin) == p.bins.end()) p.bins.push_back(bin);
    std::map<int, Id> media;
    for (const PsdLayer& l : info.layers) {
        if (!l.hasPixels() || l.adjustment) continue;
        MediaItem m;
        m.id = p.newId();
        m.name = base + " - " + (l.name.empty() ? "Layer " + std::to_string(l.index + 1) : l.name);
        m.bin = bin;
        if (!probeMedia(psdLayerPath(path, l.index), m, error)) return {};
        m.name = base + " - " + (l.name.empty() ? "Layer " + std::to_string(l.index + 1) : l.name);
        p.media.push_back(m);
        media[l.index] = m.id;
        made.push_back(m.id);
    }
    if (made.empty()) {
        if (error) *error = "The Photoshop file has no layers with pixels";
        return {};
    }
    if (mode == PsdImport::Layers) return made;
    // A sequence at the canvas size, a track per layer from the bottom.
    Rational fps{25, 1};
    if (const Sequence* active = p.active()) fps = active->fps;
    Sequence s = makeSequence(p, base, info.width, info.height, fps, 0, 1);
    const FrameTime length = std::max<FrameTime>(1, FrameTime(std::llround(seconds * fps.toDouble())));
    auto groupOf = [&](int index) -> const PsdLayer* { return index >= 0 && index < int(info.layers.size()) ? &info.layers[size_t(index)] : nullptr; };
    std::map<int, int> trackOf;  // layer index -> video track
    for (const PsdLayer& l : info.layers) {
        if (!media.count(l.index)) continue;
        const int track = int(s.videoTracks.size());
        edit::addTrack(p, s, TrackKind::Video);
        Track& t = s.videoTracks.back();
        t.name = l.name;
        // Its groups: their names as the track folder, their opacity and visibility on top of its own.
        double opacity = l.opacity;
        bool visible = l.visible;
        std::string folder;
        for (const PsdLayer* g = groupOf(l.group); g; g = groupOf(g->group)) {
            opacity *= g->opacity;
            visible = visible && g->visible;
            folder = folder.empty() ? g->name : g->name + " / " + folder;
        }
        t.folder = folder;
        const edit::Result r = edit::placeMedia(p, s, media[l.index], 0, 0, length, {TrackKind::Video, track}, {TrackKind::Audio, 0}, false);
        if (!r.ok || t.clips.empty()) {
            if (error) *error = r.error.empty() ? "A layer could not be placed" : r.error;
            return {};
        }
        Clip& c = s.videoTracks[size_t(track)].clips.front();
        c.name = l.name;
        c.blendMode = psdBlendMode(l.blend);
        if (opacity < 1) c.motion.params["opacity"] = Param(std::round(opacity * 1000) / 10);
        c.enabled = visible;
        trackOf[l.index] = track;
        // A clipping mask: seen only through the layer it is clipped to (the nearest unclipped one below).
        if (l.clipped)
            for (int below = l.index - 1; below >= 0; --below) {
                const PsdLayer& b = info.layers[size_t(below)];
                if (b.clipped || !trackOf.count(below)) continue;
                Effect matte = makeEffect(p, "track_matte");
                matte.params["track"] = Param(double(trackOf[below] + 1));
                matte.params["composite"] = Param(0.0);
                matte.params["hide"] = Param(0.0);
                c.effects.push_back(matte);
                break;
            }
    }
    MediaItem item;
    item.id = p.newId();
    item.kind = MediaKind::Sequence;
    item.name = s.name;
    item.sequenceId = s.id;
    item.hasVideo = true;
    item.width = s.width;
    item.height = s.height;
    item.fps = s.fps;
    item.bin = bin;
    p.sequences.push_back(std::move(s));
    p.media.push_back(item);
    made.push_back(item.id);
    return made;
}

}  // namespace montage
