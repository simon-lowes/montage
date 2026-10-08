#include "Cfb.h"

#include <QFile>
#include <QString>
#include <algorithm>
#include <cstring>
#include <functional>

namespace montage {

namespace {

constexpr uint32_t kSector = 4096;  // version 4
constexpr uint32_t kMini = 64;
constexpr uint32_t kCutoff = 4096;  // smaller streams live in the mini stream
constexpr uint32_t kFree = 0xFFFFFFFF, kEnd = 0xFFFFFFFE, kFatSect = 0xFFFFFFFD, kDifSect = 0xFFFFFFFC, kNone = 0xFFFFFFFF;

void put16(std::string& b, size_t at, uint16_t v) {
    b[at] = char(v & 0xff);
    b[at + 1] = char(v >> 8);
}
void put32(std::string& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = char((v >> (8 * i)) & 0xff);
}
void put64(std::string& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + size_t(i)] = char((v >> (8 * i)) & 0xff);
}
uint16_t get16(const std::string& b, size_t at) {
    return uint16_t(uint8_t(b[at]) | uint16_t(uint8_t(b[at + 1])) << 8);
}
uint32_t get32(const std::string& b, size_t at) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | uint8_t(b[at + size_t(i)]);
    return v;
}
uint64_t get64(const std::string& b, size_t at) { return uint64_t(get32(b, at)) | uint64_t(get32(b, at + 4)) << 32; }

std::u16string utf16(const std::string& s) { return QString::fromStdString(s).toStdU16String(); }

// [MS-CFB] 2.6.4: shorter names first, then code unit by code unit, upper-cased.
bool nameLess(const std::u16string& a, const std::u16string& b) {
    if (a.size() != b.size()) return a.size() < b.size();
    for (size_t i = 0; i < a.size(); ++i) {
        char16_t x = a[i], y = b[i];
        if (x < 128) x = char16_t(std::toupper(int(x)));
        if (y < 128) y = char16_t(std::toupper(int(y)));
        if (x != y) return x < y;
    }
    return false;
}

struct Dir {
    std::u16string name;
    uint8_t type = 0;  // 1 storage, 2 stream, 5 root
    uint8_t color = 1;  // 0 red, 1 black
    uint32_t left = kNone, right = kNone, child = kNone;
    std::array<uint8_t, 16> clsid{};
    uint32_t start = kEnd;
    uint64_t size = 0;
    const std::string* data = nullptr;
};

// A red-black tree of siblings, balanced: the middle one at the top. Only the deepest level can be
// short, and it is red, so every path has the same number of black nodes.
uint32_t buildTree(std::vector<Dir>& dirs, const std::vector<uint32_t>& sorted, int lo, int hi, int depth,
                   std::vector<std::pair<uint32_t, int>>& depths) {
    if (lo > hi) return kNone;
    const int mid = (lo + hi) / 2;
    const uint32_t node = sorted[size_t(mid)];
    depths.push_back({node, depth});
    dirs[node].left = buildTree(dirs, sorted, lo, mid - 1, depth + 1, depths);
    dirs[node].right = buildTree(dirs, sorted, mid + 1, hi, depth + 1, depths);
    return node;
}

void collect(const CfbEntry& e, uint32_t self, std::vector<Dir>& dirs) {
    std::vector<uint32_t> idx;
    for (const CfbEntry& c : e.children) {
        Dir d;
        d.name = utf16(c.name).substr(0, 31);
        d.type = c.storage ? 1 : 2;
        d.clsid = c.storage ? c.clsid : std::array<uint8_t, 16>{};
        if (!c.storage) {
            d.size = c.data.size();
            d.data = &c.data;
        }
        idx.push_back(uint32_t(dirs.size()));
        dirs.push_back(d);
    }
    if (!idx.empty()) {
        std::vector<uint32_t> sorted = idx;
        std::sort(sorted.begin(), sorted.end(), [&](uint32_t a, uint32_t b) { return nameLess(dirs[a].name, dirs[b].name); });
        std::vector<std::pair<uint32_t, int>> depths;
        dirs[self].child = buildTree(dirs, sorted, 0, int(sorted.size()) - 1, 0, depths);
        int deepest = 0;
        for (const auto& [n, d] : depths) deepest = std::max(deepest, d);
        for (const auto& [n, d] : depths) dirs[n].color = (d == deepest && deepest > 0) ? 0 : 1;
    }
    for (size_t k = 0; k < e.children.size(); ++k)
        if (e.children[k].storage) collect(e.children[k], idx[k], dirs);
}

}  // namespace

CfbEntry* CfbEntry::find(const std::string& child) {
    for (CfbEntry& c : children)
        if (c.name == child) return &c;
    return nullptr;
}

const CfbEntry* CfbEntry::find(const std::string& child) const {
    for (const CfbEntry& c : children)
        if (c.name == child) return &c;
    return nullptr;
}

const CfbEntry* CfbEntry::at(const std::string& path) const {
    const CfbEntry* e = this;
    size_t from = 0;
    while (e && from <= path.size()) {
        const size_t slash = path.find('/', from);
        const std::string part = path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
        if (!part.empty()) e = e->find(part);
        if (slash == std::string::npos) break;
        from = slash + 1;
    }
    return e;
}

bool writeCompoundFile(const std::string& path, const CfbEntry& root, std::string* error) {
    std::vector<Dir> dirs(1);
    dirs[0].name = u"Root Entry";
    dirs[0].type = 5;
    dirs[0].clsid = root.clsid;
    collect(root, 0, dirs);

    // Small streams go in the mini stream, in 64-byte mini sectors.
    std::string mini;
    std::vector<uint32_t> miniFat;
    std::vector<uint32_t> big;  // directory entries of streams with their own sectors
    for (uint32_t i = 1; i < dirs.size(); ++i) {
        Dir& d = dirs[i];
        if (d.type != 2) {
            d.start = 0;
            continue;
        }
        if (d.size == 0) {
            d.start = kEnd;
        } else if (d.size < kCutoff) {
            const uint32_t first = uint32_t(mini.size() / kMini), count = uint32_t((d.size + kMini - 1) / kMini);
            d.start = first;
            mini += *d.data;
            mini.resize(size_t(first + count) * kMini, '\0');
            for (uint32_t k = 0; k < count; ++k) miniFat.push_back(k + 1 < count ? first + k + 1 : kEnd);
        } else {
            big.push_back(i);
        }
    }
    auto sectorsFor = [](uint64_t bytes) { return uint32_t((bytes + kSector - 1) / kSector); };
    const uint32_t nDir = std::max<uint32_t>(1, sectorsFor(uint64_t(dirs.size()) * 128));
    const uint32_t nMiniFat = sectorsFor(uint64_t(miniFat.size()) * 4);
    const uint32_t nMini = sectorsFor(mini.size());
    uint64_t nData = uint64_t(nDir) + nMiniFat + nMini;
    for (uint32_t i : big) nData += sectorsFor(dirs[i].size);
    uint32_t nFat = 1, nDifat = 0;
    for (;;) {
        nDifat = nFat > 109 ? (nFat - 109 + 1022) / 1023 : 0;
        if (uint64_t(nFat) * (kSector / 4) >= nData + nFat + nDifat) break;
        ++nFat;
    }
    // Sectors in order: directory, mini FAT, mini stream, the big streams, the FAT, the DIFAT.
    std::vector<uint32_t> fat(size_t(nFat) * (kSector / 4), kFree);
    uint32_t next = 0;
    auto chain = [&](uint32_t count) {
        const uint32_t first = count ? next : kEnd;
        for (uint32_t k = 0; k < count; ++k) fat[next + k] = k + 1 < count ? next + k + 1 : kEnd;
        next += count;
        return first;
    };
    const uint32_t dirStart = chain(nDir);
    const uint32_t miniFatStart = chain(nMiniFat);
    const uint32_t miniStart = chain(nMini);
    dirs[0].start = mini.empty() ? kEnd : miniStart;
    dirs[0].size = mini.size();
    for (uint32_t i : big) dirs[i].start = chain(sectorsFor(dirs[i].size));
    const uint32_t fatStart = next;
    for (uint32_t k = 0; k < nFat; ++k) fat[next++] = kFatSect;
    const uint32_t difatStart = nDifat ? next : kEnd;
    for (uint32_t k = 0; k < nDifat; ++k) fat[next++] = kDifSect;

    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    auto writeAll = [&](const std::string& bytes) { return f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()); };
    auto padded = [](std::string s) {
        s.resize((s.size() + kSector - 1) / kSector * kSector, '\0');
        return s;
    };
    // Header, in a sector of its own.
    std::string h(kSector, '\0');
    static const unsigned char sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::memcpy(&h[0], sig, 8);
    put16(h, 24, 0x003E);
    put16(h, 26, 0x0004);
    put16(h, 28, 0xFFFE);
    put16(h, 30, 12);
    put16(h, 32, 6);
    put32(h, 40, nDir);
    put32(h, 44, nFat);
    put32(h, 48, dirStart);
    put32(h, 56, kCutoff);
    put32(h, 60, nMiniFat ? miniFatStart : kEnd);
    put32(h, 64, nMiniFat);
    put32(h, 68, difatStart);
    put32(h, 72, nDifat);
    for (uint32_t k = 0; k < 109; ++k) put32(h, 76 + size_t(k) * 4, k < nFat ? fatStart + k : kFree);
    bool ok = writeAll(h);
    // Directory.
    std::string d(size_t(nDir) * kSector, '\0');
    for (size_t i = 0; i < size_t(nDir) * kSector / 128; ++i) {
        const size_t at = i * 128;
        if (i >= dirs.size()) {
            put32(d, at + 68, kNone);
            put32(d, at + 72, kNone);
            put32(d, at + 76, kNone);
            continue;
        }
        const Dir& e = dirs[i];
        for (size_t c = 0; c < e.name.size(); ++c) put16(d, at + c * 2, uint16_t(e.name[c]));
        put16(d, at + 64, uint16_t((e.name.size() + 1) * 2));
        d[at + 66] = char(e.type);
        d[at + 67] = char(e.color);
        put32(d, at + 68, e.left);
        put32(d, at + 72, e.right);
        put32(d, at + 76, e.child);
        std::memcpy(&d[at + 80], e.clsid.data(), 16);
        put32(d, at + 116, e.start);
        put64(d, at + 120, e.size);
    }
    ok = ok && writeAll(d);
    // Mini FAT and mini stream.
    if (nMiniFat) {
        std::string mf(size_t(nMiniFat) * kSector, '\0');
        for (size_t k = 0; k < size_t(nMiniFat) * kSector / 4; ++k) put32(mf, k * 4, k < miniFat.size() ? miniFat[k] : kFree);
        ok = ok && writeAll(mf);
    }
    if (nMini) ok = ok && writeAll(padded(mini));
    for (uint32_t i : big) ok = ok && writeAll(padded(*dirs[i].data));
    // FAT and DIFAT.
    std::string fb(fat.size() * 4, '\0');
    for (size_t k = 0; k < fat.size(); ++k) put32(fb, k * 4, fat[k]);
    ok = ok && writeAll(fb);
    for (uint32_t s = 0; s < nDifat; ++s) {
        std::string ds(kSector, '\0');
        for (uint32_t k = 0; k < 1023; ++k) {
            const uint32_t n = 109 + s * 1023 + k;
            put32(ds, size_t(k) * 4, n < nFat ? fatStart + n : kFree);
        }
        put32(ds, 1023 * 4, s + 1 < nDifat ? difatStart + s + 1 : kEnd);
        ok = ok && writeAll(ds);
    }
    if (!ok && error) *error = "Could not write all of " + path;
    return ok;
}

bool readCompoundFile(const std::string& path, CfbEntry& root, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot open " + path;
        return false;
    }
    const QByteArray all = f.readAll();
    const std::string b(all.constData(), size_t(all.size()));
    auto fail = [&](const char* why) {
        if (error) *error = std::string(why) + ": " + path;
        return false;
    };
    if (b.size() < 512 || uint8_t(b[0]) != 0xD0 || uint8_t(b[1]) != 0xCF) return fail("Not a compound file");
    const uint32_t sector = 1u << get16(b, 30), miniSector = 1u << get16(b, 32);
    const uint32_t cutoff = get32(b, 56);
    auto offset = [&](uint32_t s) { return (uint64_t(s) + 1) * sector; };
    // The FAT, from the DIFAT.
    std::vector<uint32_t> fatSectors;
    for (uint32_t k = 0; k < 109 && k < get32(b, 44); ++k) fatSectors.push_back(get32(b, 76 + size_t(k) * 4));
    for (uint32_t s = get32(b, 68), n = 0; s < kDifSect && n < get32(b, 72); ++n) {
        for (uint32_t k = 0; k + 1 < sector / 4 && fatSectors.size() < get32(b, 44); ++k)
            fatSectors.push_back(get32(b, size_t(offset(s)) + size_t(k) * 4));
        s = get32(b, size_t(offset(s)) + sector - 4);
    }
    std::vector<uint32_t> fat;
    for (uint32_t s : fatSectors) {
        if (offset(s) + sector > b.size()) return fail("Truncated FAT");
        for (uint32_t k = 0; k < sector / 4; ++k) fat.push_back(get32(b, size_t(offset(s)) + size_t(k) * 4));
    }
    auto readChain = [&](uint32_t start, uint64_t size, bool sized) {
        std::string out;
        for (uint32_t s = start, guard = 0; s < kDifSect && s < fat.size() && guard < fat.size(); s = fat[s], ++guard) {
            if (offset(s) + sector > b.size()) break;
            out.append(b, size_t(offset(s)), sector);
        }
        if (sized) out.resize(size_t(size), '\0');
        return out;
    };
    const std::string dir = readChain(get32(b, 48), 0, false);
    const size_t count = dir.size() / 128;
    if (count == 0) return fail("No directory");
    const std::string miniStream = readChain(get32(dir, 116), get64(dir, 120), true);  // the root entry holds it
    const std::string miniFatBytes = readChain(get32(b, 60), 0, false);
    std::vector<uint32_t> miniFat;
    for (size_t k = 0; k + 4 <= miniFatBytes.size(); k += 4) miniFat.push_back(get32(miniFatBytes, k));
    auto entryName = [&](size_t i) {
        const size_t at = i * 128;
        const size_t chars = std::min<size_t>(31, get16(dir, at + 64) / 2);  // the length counts the terminator
        std::u16string n;
        for (size_t c = 0; c < chars; ++c) {
            const char16_t ch = char16_t(get16(dir, at + c * 2));
            if (!ch) break;
            n += ch;
        }
        return QString::fromStdU16String(n).toStdString();
    };
    std::function<void(uint32_t, CfbEntry&, int)> fill;
    std::function<void(uint32_t, CfbEntry&, int)> siblings = [&](uint32_t i, CfbEntry& parent, int depth) {
        if (i >= count || depth > 64) return;
        const size_t at = size_t(i) * 128;
        siblings(get32(dir, at + 68), parent, depth + 1);
        CfbEntry e;
        e.name = entryName(i);
        e.storage = dir[at + 66] == 1;
        std::memcpy(e.clsid.data(), &dir[at + 80], 16);
        if (dir[at + 66] == 2) {
            const uint64_t size = get64(dir, at + 120) & 0xFFFFFFFFull;  // version 3 files leave the top half undefined
            const uint32_t start = get32(dir, at + 116);
            if (size < cutoff) {
                for (uint32_t s = start, guard = 0; s < kDifSect && s < miniFat.size() && guard < miniFat.size(); s = miniFat[s], ++guard)
                    if (size_t(s) * miniSector + miniSector <= miniStream.size()) e.data.append(miniStream, size_t(s) * miniSector, miniSector);
                e.data.resize(size_t(size), '\0');
            } else {
                e.data = readChain(start, size, true);
            }
        } else if (dir[at + 66] == 1) {
            fill(i, e, depth);
        }
        parent.children.push_back(std::move(e));
        siblings(get32(dir, at + 72), parent, depth + 1);
    };
    fill = [&](uint32_t i, CfbEntry& e, int depth) { siblings(get32(dir, size_t(i) * 128 + 76), e, depth + 1); };
    root = CfbEntry{};
    root.name = "Root Entry";
    root.storage = true;
    std::memcpy(root.clsid.data(), &dir[80], 16);
    fill(0, root, 0);
    return true;
}

}  // namespace montage
