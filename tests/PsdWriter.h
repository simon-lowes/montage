// A small Photoshop file writer for the tests: RGB at 8 or 16 bits, PSD or PSB, layers with names (also as Unicode),
// bounds, blend modes, opacity, visibility, clipping, groups, layer masks, and channels raw, PackBits or ZIP (with or
// without prediction), plus the merged picture.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TestPsdLayer {
    std::string name;
    int left = 0, top = 0, right = 0, bottom = 0;
    std::string blend = "norm";
    int opacity = 255;
    bool hidden = false, clipped = false;
    int section = 0;  // 1 a group's own record (open folder), 3 the record closing a group
    int compression = 0;  // 0 raw, 1 PackBits, 2 ZIP, 3 ZIP with prediction
    std::function<std::array<uint16_t, 4>(int x, int y)> pixel;  // RGBA (16-bit scale) at canvas x, y
    bool mask = false;
    int maskLeft = 0, maskTop = 0, maskRight = 0, maskBottom = 0;
    std::function<uint16_t(int x, int y)> maskValue;  // at canvas x, y; outside the mask's rectangle it is 0
};

namespace testpsd {

inline void be(QByteArray& b, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) b.append(char((v >> (8 * i)) & 0xff));
}

inline QByteArray packBits(const QByteArray& row) {
    QByteArray out;
    int i = 0;
    const int n = int(row.size());
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) ++run;
        if (run >= 3) {
            out.append(char(1 - run));
            out.append(row[i]);
            i += run;
            continue;
        }
        int lit = 0;
        while (i + lit < n && lit < 128) {
            if (i + lit + 2 < n && row[i + lit] == row[i + lit + 1] && row[i + lit] == row[i + lit + 2]) break;
            ++lit;
        }
        out.append(char(lit - 1));
        out.append(row.mid(i, lit));
        i += lit;
    }
    return out;
}

// One channel's data (compression word first) from its samples, row by row.
inline QByteArray channel(const std::vector<uint16_t>& v, int w, int h, int depth, bool psb, int compression) {
    std::vector<QByteArray> byRow(size_t(std::max(0, h)));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint16_t s = v[size_t(y) * size_t(w) + size_t(x)];
            if (depth == 8) byRow[size_t(y)].append(char(s >> 8));
            else be(byRow[size_t(y)], s, 2);
        }
    QByteArray out;
    be(out, uint64_t(compression), 2);
    if (compression == 0) {
        for (const QByteArray& r : byRow) out.append(r);
    } else if (compression == 1) {
        std::vector<QByteArray> packed;
        for (const QByteArray& r : byRow) packed.push_back(packBits(r));
        for (const QByteArray& p : packed) be(out, uint64_t(p.size()), psb ? 4 : 2);
        for (const QByteArray& p : packed) out.append(p);
    } else {
        QByteArray all;
        for (QByteArray r : byRow) {
            if (compression == 3) {  // differences along the row, from its end back
                if (depth == 8)
                    for (int x = w - 1; x > 0; --x) r[x] = char(uint8_t(r[x]) - uint8_t(r[x - 1]));
                else
                    for (int x = w - 1; x > 0; --x) {
                        const uint16_t a = uint16_t((uint8_t(r[2 * x]) << 8) | uint8_t(r[2 * x + 1]));
                        const uint16_t b = uint16_t((uint8_t(r[2 * x - 2]) << 8) | uint8_t(r[2 * x - 1]));
                        const uint16_t d = uint16_t(a - b);
                        r[2 * x] = char(d >> 8), r[2 * x + 1] = char(d & 0xff);
                    }
            }
            all.append(r);
        }
        out.append(qCompress(all).mid(4));  // a zlib stream (qCompress puts the length first)
    }
    return out;
}

inline bool write(const QString& path, int width, int height, int depth, bool psb, const std::vector<TestPsdLayer>& layers,
                  const std::function<std::array<uint16_t, 4>(int x, int y)>& merged) {
    QByteArray f("8BPS");
    be(f, psb ? 2 : 1, 2);
    f.append(QByteArray(6, 0));
    be(f, 4, 2);  // R, G, B and the merged transparency
    be(f, uint64_t(height), 4);
    be(f, uint64_t(width), 4);
    be(f, uint64_t(depth), 2);
    be(f, 3, 2);  // RGB
    be(f, 0, 4);  // colour mode data
    be(f, 0, 4);  // image resources
    // Layer records, then their channels.
    QByteArray info, data;
    be(info, uint64_t(layers.size()), 2);
    for (const TestPsdLayer& l : layers) {
        const int w = std::max(0, l.right - l.left), h = std::max(0, l.bottom - l.top);
        std::vector<int> ids = {-1, 0, 1, 2};
        if (l.mask) ids.push_back(-2);
        std::vector<QByteArray> chans;
        for (int id : ids) {
            const bool m = id == -2;
            const int cw = m ? l.maskRight - l.maskLeft : w, ch = m ? l.maskBottom - l.maskTop : h;
            std::vector<uint16_t> v(size_t(cw) * size_t(std::max(0, ch)));
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    uint16_t s = 0;
                    if (m) s = l.maskValue(l.maskLeft + x, l.maskTop + y);
                    else if (l.pixel) s = l.pixel(l.left + x, l.top + y)[size_t(id < 0 ? 3 : id)];
                    v[size_t(y) * size_t(cw) + size_t(x)] = s;
                }
            chans.push_back(channel(v, cw, ch, depth, psb, l.compression));
        }
        be(info, uint64_t(uint32_t(l.top)), 4);
        be(info, uint64_t(uint32_t(l.left)), 4);
        be(info, uint64_t(uint32_t(l.bottom)), 4);
        be(info, uint64_t(uint32_t(l.right)), 4);
        be(info, ids.size(), 2);
        for (size_t c = 0; c < ids.size(); ++c) {
            be(info, uint64_t(uint16_t(int16_t(ids[c]))), 2);
            be(info, uint64_t(chans[c].size()), psb ? 8 : 4);
        }
        info.append("8BIM");
        info.append(QByteArray::fromStdString(l.blend));
        info.append(char(l.opacity));
        info.append(char(l.clipped ? 1 : 0));
        info.append(char(l.hidden ? 0x0a : 0x08));
        info.append(char(0));
        QByteArray extra;
        if (l.mask) {
            be(extra, 20, 4);
            be(extra, uint64_t(uint32_t(l.maskTop)), 4);
            be(extra, uint64_t(uint32_t(l.maskLeft)), 4);
            be(extra, uint64_t(uint32_t(l.maskBottom)), 4);
            be(extra, uint64_t(uint32_t(l.maskRight)), 4);
            extra.append(char(0));  // outside its rectangle: hidden
            extra.append(char(0));
            extra.append(QByteArray(2, 0));
        } else {
            be(extra, 0, 4);
        }
        be(extra, 0, 4);  // blending ranges
        const QByteArray latin = QString::fromStdString(l.name).toLatin1();
        extra.append(char(latin.size()));
        extra.append(latin);
        extra.append(QByteArray((4 - (1 + int(latin.size())) % 4) % 4, 0));  // the name padded to four bytes
        // The Unicode name.
        const QString uname = QString::fromStdString(l.name);
        QByteArray luni;
        be(luni, uint64_t(uname.size()), 4);
        for (QChar c : uname) be(luni, c.unicode(), 2);
        if (luni.size() % 2) luni.append(char(0));
        extra.append("8BIMluni");
        be(extra, uint64_t(luni.size()), 4);
        extra.append(luni);
        if (l.section) {
            extra.append("8BIMlsct");
            be(extra, 4, 4);
            be(extra, uint64_t(l.section), 4);
        }
        be(info, uint64_t(extra.size()), 4);
        info.append(extra);
        for (const QByteArray& c : chans) data.append(c);
    }
    info.append(data);
    if (info.size() % 2) info.append(char(0));
    QByteArray section;
    be(section, uint64_t(info.size()), psb ? 8 : 4);
    section.append(info);
    be(section, 0, 4);  // global layer mask
    be(f, uint64_t(section.size()), psb ? 8 : 4);
    f.append(section);
    // The merged picture, uncompressed: each channel's rows in turn.
    be(f, 0, 2);
    for (int c = 0; c < 4; ++c)
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const uint16_t s = merged(x, y)[size_t(c)];
                if (depth == 8) f.append(char(s >> 8));
                else be(f, s, 2);
            }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return false;
    return out.write(f) == f.size();
}

}  // namespace testpsd
