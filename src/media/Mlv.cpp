#include "Mlv.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "Image.h"

namespace montage {

namespace {

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
int32_t i32(const uint8_t* p) { return int32_t(u32(p)); }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32); }

std::string text(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    s.resize(std::strlen(s.c_str()));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool endsWithNoCase(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != tail[i]) return false;
    return true;
}

void mul3(const double a[9], const double b[9], double out[9]) {
    double t[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    std::copy(t, t + 9, out);
}

bool invert3(const double m[9], double out[9]) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12) return false;
    const double k = 1 / det;
    out[0] = (e * i - f * h) * k, out[1] = (c * h - b * i) * k, out[2] = (b * f - c * e) * k;
    out[3] = (f * g - d * i) * k, out[4] = (a * i - c * g) * k, out[5] = (c * d - a * f) * k;
    out[6] = (d * h - e * g) * k, out[7] = (b * g - a * h) * k, out[8] = (a * e - b * d) * k;
    return true;
}

// The development's colour from the camera's matrix (XYZ to camera, DNG's ColorMatrix1) and the light as shot (its
// white as the camera sees it, green 1): balanced camera RGB to XYZ D65 with the light adapted to D65, white at Y 1.
void colourFrom(const double xyzToCam[9], const double camWhite[3], MlvInfo& info) {
    ProResRawColor& c = info.color;
    c.wbRed = camWhite[1] / std::max(1e-9, camWhite[0]);
    c.wbBlue = camWhite[1] / std::max(1e-9, camWhite[2]);
    double camToXyz[9];
    if (!invert3(xyzToCam, camToXyz)) return;
    double light[3];
    for (int r = 0; r < 3; ++r) light[r] = camToXyz[r * 3] * camWhite[0] + camToXyz[r * 3 + 1] * camWhite[1] + camToXyz[r * 3 + 2] * camWhite[2];
    const double sum = light[0] + light[1] + light[2];
    if (sum <= 1e-9 || light[1] <= 1e-9) return;
    const double x = light[0] / sum, y = light[1] / sum;
    // (McCamy's approximation of the light's colour temperature, for choosing another)
    const double n = (x - 0.3320) / (0.1858 - y);
    const double cct = 449 * n * n * n + 3525 * n * n + 6823.3 * n + 5520.33;
    c.cct = cct > 1667 && cct < 25000 ? unsigned(std::lround(cct)) : 0;
    double adapt[9], m[9];
    bradfordAdaptation(x, y, 0.3127, 0.3290, adapt);
    const double diag[9] = {camWhite[0], 0, 0, 0, camWhite[1], 0, 0, 0, camWhite[2]};
    mul3(camToXyz, diag, m);
    mul3(adapt, m, m);
    // (white, balanced camera (1, 1, 1), at Y 1)
    const double y1 = m[3] + m[4] + m[5];
    if (y1 <= 1e-9) return;
    for (int i = 0; i < 9; ++i) c.camToXyz[i] = m[i] / y1;
    c.asShotX = x, c.asShotY = y;
}

std::string chunkPath(const std::string& path, int i) {
    // FOO.MLV, then FOO.M00, FOO.M01... (as the camera names them; in lower case if those are not there)
    const size_t dot = path.find_last_of('.');
    char ext[8];
    std::snprintf(ext, sizeof ext, "M%02d", i - 1);
    const std::string upper = path.substr(0, dot + 1) + ext;
    if (QFile::exists(QString::fromStdString(upper))) return upper;
    ext[0] = 'm';
    const std::string lower = path.substr(0, dot + 1) + ext;
    return QFile::exists(QString::fromStdString(lower)) ? lower : upper;
}

// ---------------------------------------------------------------------------
// Lossless JPEG

struct Huffman {
    bool defined = false;
    // A 9-bit lookup (length << 8 | value; 0: longer), and the canonical decoding tables for longer codes.
    uint16_t fast[512] = {};
    int maxcode[18] = {};
    int valptr[17] = {};
    int mincode[17] = {};
    uint8_t values[256] = {};
};

bool buildHuffman(const uint8_t counts[16], const uint8_t* values, int nvalues, Huffman& h) {
    h = Huffman{};
    std::copy(values, values + nvalues, h.values);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; ++len) {
        h.valptr[len] = k;
        h.mincode[len] = code;
        for (int i = 0; i < counts[len - 1]; ++i, ++k, ++code) {
            if (code >= (1 << len)) return false;  // (more codes than this length has)
            if (len <= 9)
                for (int fill = 0; fill < (1 << (9 - len)); ++fill) h.fast[(code << (9 - len)) | fill] = uint16_t((len << 8) | values[k]);
        }
        h.maxcode[len] = counts[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h.maxcode[17] = 0x7FFFFFFF;
    h.defined = true;
    return true;
}

class BitReader {
public:
    BitReader(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}
    void fill() {
        while (n_ <= 56) {
            uint8_t b = 0;
            if (p_ < end_ && !marker_) {
                b = *p_;
                if (b == 0xFF) {
                    if (p_ + 1 < end_ && p_[1] == 0x00) {
                        p_ += 2;
                    } else {
                        marker_ = true;  // (a marker: zeros from here)
                        b = 0;
                    }
                } else {
                    ++p_;
                }
            }
            acc_ |= uint64_t(b) << (56 - n_);
            n_ += 8;
        }
    }
    int peek(int bits) {
        if (n_ < bits) fill();
        return int(acc_ >> (64 - bits));
    }
    void skip(int bits) {
        acc_ <<= bits;
        n_ -= bits;
    }
    int get(int bits) {
        if (bits == 0) return 0;
        const int v = peek(bits);
        skip(bits);
        return v;
    }
    // At a restart marker: the bits left in the byte dropped, the marker passed.
    bool restart() {
        acc_ = 0;
        n_ = 0;
        marker_ = false;
        while (p_ + 1 < end_ && !(p_[0] == 0xFF && p_[1] >= 0xD0 && p_[1] <= 0xD7)) ++p_;
        if (p_ + 1 >= end_) return false;
        p_ += 2;
        return true;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t acc_ = 0;
    int n_ = 0;
    bool marker_ = false;
};

int decodeDiff(BitReader& br, const Huffman& h) {
    int ssss;
    const uint16_t f = h.fast[br.peek(9)];
    if (f) {
        br.skip(f >> 8);
        ssss = f & 0xFF;
    } else {
        int code = br.get(9), len = 9;
        while (len < 16 && code > h.maxcode[len]) code = (code << 1) | br.get(1), ++len;
        if (code > h.maxcode[len]) return 0;  // (a bad code: no change)
        ssss = h.values[h.valptr[len] + code - h.mincode[len]];
    }
    if (ssss == 0) return 0;
    if (ssss >= 16) return 32768;
    int diff = br.get(ssss);
    if ((diff & (1 << (ssss - 1))) == 0) diff -= (1 << ssss) - 1;
    return diff;
}

}  // namespace

bool decodeLosslessJpeg(const uint8_t* data, size_t size, std::vector<uint16_t>& out, int& width, int& height, int& components,
                        int& bits, std::string* error, size_t samples) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return fail("Not a lossless JPEG frame");
    Huffman tables[4];
    int compTable[4] = {0, 0, 0, 0}, compIds[4] = {0, 0, 0, 0};
    int predictor = 1, pt = 0, restartInterval = 0;
    width = height = components = bits = 0;
    const uint8_t* p = data + 2;
    const uint8_t* end = data + size;
    while (p + 4 <= end) {
        if (p[0] != 0xFF) return fail("Damaged lossless JPEG frame");
        const uint8_t marker = p[1];
        if (marker == 0xFF) {
            ++p;
            continue;
        }
        p += 2;
        if (marker == 0xD9) break;
        const int len = (p[0] << 8) | p[1];
        if (len < 2 || p + len > end) return fail("Damaged lossless JPEG frame");
        const uint8_t* seg = p + 2;
        const uint8_t* segEnd = p + len;
        if (marker == 0xC4) {
            while (seg + 17 <= segEnd) {
                const int index = seg[0] & 0x0F;
                const uint8_t* counts = seg + 1;
                int n = 0;
                for (int i = 0; i < 16; ++i) n += counts[i];
                if (index > 3 || n > 256 || seg + 17 + n > segEnd) return fail("Damaged Huffman table");
                if (!buildHuffman(counts, seg + 17, n, tables[index])) return fail("Damaged Huffman table");
                seg += 17 + n;
            }
        } else if (marker == 0xC3) {
            if (len < 8) return fail("Damaged frame header");
            bits = seg[0];
            height = (seg[1] << 8) | seg[2];
            width = (seg[3] << 8) | seg[4];
            components = seg[5];
            if (components < 1 || components > 4 || bits < 2 || bits > 16 || width <= 0 || height <= 0 || len < 8 + components * 3)
                return fail("Unsupported lossless JPEG frame");
            for (int c = 0; c < components; ++c) {
                compIds[c] = seg[6 + c * 3];
                if (seg[7 + c * 3] != 0x11) return fail("Subsampled lossless JPEG is not supported");
            }
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return fail("Not a lossless JPEG frame (another process)");
        } else if (marker == 0xDD) {
            if (len < 4) return fail("Damaged restart interval");
            restartInterval = (seg[0] << 8) | seg[1];
        } else if (marker == 0xDA) {
            if (!width) return fail("Scan before the frame header");
            if (len < 3) return fail("Unsupported scan");
            const int ns = seg[0];
            if (ns != components || len < 6 + ns * 2) return fail("Unsupported scan");
            for (int i = 0; i < ns; ++i) {
                const int id = seg[1 + i * 2];
                int c = 0;
                while (c < components && compIds[c] != id) ++c;
                if (c == components) c = i;
                compTable[c] = (seg[2 + i * 2] >> 4) & 3;
                if (!tables[compTable[c]].defined) return fail("Missing Huffman table");
            }
            predictor = seg[1 + ns * 2];
            pt = seg[3 + ns * 2] & 0x0F;
            if (predictor < 1 || predictor > 7) predictor = 1;
            p = segEnd;
            // The samples.
            const size_t rowLen = size_t(width) * size_t(components);
            if (samples && rowLen * size_t(height) < samples) return fail("The lossless JPEG frame is smaller than the picture");
            if (samples && rowLen * size_t(height) > samples + samples / 8) return fail("The lossless JPEG frame is larger than the picture");
            out.resize(rowLen * size_t(height));  // (every sample written below)
            BitReader br(p, end);
            const int initial = 1 << std::max(0, bits - pt - 1);
            const int perRestart = restartInterval;
            if (perRestart > 0 && perRestart % width != 0) return fail("Restart intervals within a row are not supported");
            int firstRow = 0;  // (the first row since the scan began or the last restart)
            for (int y = 0; y < height; ++y) {
                if (perRestart > 0 && y > 0 && (size_t(y) * size_t(width)) % size_t(perRestart) == 0) {
                    if (!br.restart()) return fail("Missing restart marker");
                    firstRow = y;
                }
                uint16_t* row = out.data() + size_t(y) * rowLen;
                const uint16_t* above = y > firstRow ? row - rowLen : nullptr;
                for (int x = 0; x < width; ++x)
                    for (int c = 0; c < components; ++c) {
                        const size_t i = size_t(x) * size_t(components) + size_t(c);
                        const int diff = decodeDiff(br, tables[compTable[c]]);
                        int pred;
                        if (!above) {
                            pred = x == 0 ? initial : row[i - components];
                        } else if (x == 0) {
                            pred = above[i];
                        } else {
                            const int ra = row[i - components], rb = above[i], rc = above[i - components];
                            switch (predictor) {
                                case 1: pred = ra; break;
                                case 2: pred = rb; break;
                                case 3: pred = rc; break;
                                case 4: pred = ra + rb - rc; break;
                                case 5: pred = ra + ((rb - rc) >> 1); break;
                                case 6: pred = rb + ((ra - rc) >> 1); break;
                                default: pred = (ra + rb) >> 1; break;
                            }
                        }
                        row[i] = uint16_t((pred + diff) & 0xFFFF);
                    }
            }
            if (pt > 0)
                for (uint16_t& v : out) v = uint16_t(v << pt);
            return true;
        } else {
            // (APPn, comments, quantisation tables...: skipped)
        }
        p = segEnd;
    }
    return fail("No scan in the lossless JPEG frame");
}

// ---------------------------------------------------------------------------
// MLV

bool isMlvPath(const std::string& path) { return endsWithNoCase(path, ".mlv"); }

struct MlvIndex {
    struct Block {
        int chunk = 0;
        int64_t offset = 0;  // the data's, in its chunk
        int64_t size = 0;
    };
    MlvInfo info;
    std::vector<std::string> chunks;
    std::vector<Block> frames;  // by frame number less the first's (size 0: missing)
    std::vector<Block> sound;   // in order
};

namespace {

constexpr int kMaxSide = 16384;

// The white of WBAL's presets (mlv.h's WB_ modes), as mlv_dump takes them; 0 for daylight (D65).
int presetKelvin(int mode, int kelvin) {
    switch (mode) {
        case 1: return 5200;   // daylight
        case 2: return 6000;   // cloudy
        case 3: return 3200;   // tungsten
        case 4: return 4000;   // fluorescent
        case 5: return 5500;   // flash
        case 8: return 7000;   // shade
        case 9: return kelvin;  // the temperature set
        default: return 0;     // auto (what it chose is not recorded)
    }
}

std::shared_ptr<const MlvIndex> indexMlv(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::shared_ptr<const MlvIndex>();
    };
    auto index = std::make_shared<MlvIndex>();
    MlvInfo& info = index->info;
    bool rawi = false;
    double xyzToCam[9] = {};
    bool matrix = false;
    int wbMode = -1, wbKelvin = 0;
    uint32_t gain[3] = {0, 0, 0};
    uint64_t guid = 0;
    struct Numbered {
        uint32_t n;
        MlvIndex::Block block;
    };
    std::vector<Numbered> frames, sound;
    for (int chunk = 0; chunk <= 100; ++chunk) {
        const std::string file = chunk == 0 ? path : chunkPath(path, chunk);
        QFile f(QString::fromStdString(file));
        if (!f.open(QIODevice::ReadOnly)) {
            if (chunk == 0) return fail("Cannot open " + path);
            break;  // (no more chunks)
        }
        uint8_t head[52];
        if (f.read(reinterpret_cast<char*>(head), 52) != 52 || std::memcmp(head, "MLVI", 4) != 0) {
            if (chunk == 0) return fail(path + " is not a Magic Lantern Video file");
            break;
        }
        const uint32_t headerSize = u32(head + 4);
        if (chunk == 0) {
            guid = u64(head + 8);
            const uint16_t videoClass = u16(head + 32);
            info.fpsNum = int(u32(head + 44));
            info.fpsDen = int(u32(head + 48));
            info.lossless = (videoClass & 0x20) != 0;
            if ((videoClass & 0x0F) != 0x01) return fail(path + " holds no RAW video (its frames are JPEG, H.264 or YUV)");
            if (videoClass & 0xC0) return fail(path + " is compressed with LZMA or delta frames (MLV App's own), which this does not read");
        } else if (u64(head + 8) != guid) {
            break;  // (another recording's chunk of the same name)
        }
        const int c = int(index->chunks.size());
        const int64_t total = f.size();
        int64_t at = std::max<int64_t>(16, headerSize);
        uint8_t b[256];
        while (at + 16 <= total) {
            f.seek(at);
            if (f.read(reinterpret_cast<char*>(b), 16) != 16) break;
            const uint32_t blockSize = u32(b + 4);
            if (blockSize < 16 || at + blockSize > total) break;  // (a recording cut short: what came before is kept)
            const size_t want = std::min<size_t>(sizeof b, blockSize);
            if (want > 16 && f.read(reinterpret_cast<char*>(b + 16), qint64(want - 16)) != qint64(want - 16)) break;
            auto is = [&](const char* t) { return std::memcmp(b, t, 4) == 0; };
            if (is("VIDF") && want >= 32) {
                const uint32_t space = u32(b + 28);
                if (32 + int64_t(space) < int64_t(blockSize)) frames.push_back({u32(b + 16), {c, at + 32 + space, int64_t(blockSize) - 32 - space}});
            } else if (is("AUDF") && want >= 24) {
                const uint32_t space = u32(b + 20);
                if (24 + int64_t(space) < int64_t(blockSize)) sound.push_back({u32(b + 16), {c, at + 24 + space, int64_t(blockSize) - 24 - space}});
            } else if (is("RAWI") && want >= 180) {
                rawi = true;
                info.width = u16(b + 16);
                info.height = u16(b + 18);
                const uint8_t* r = b + 20;  // raw_info
                info.bitsPerPixel = i32(r + 24);
                info.black = i32(r + 28);
                info.white = i32(r + 32);
                info.cfa = i32(r + 76);
                matrix = false;
                for (int i = 0; i < 9; ++i) {
                    const int num = i32(r + 84 + i * 8), den = i32(r + 88 + i * 8);
                    xyzToCam[i] = den ? double(num) / den : 0;
                    matrix = matrix || xyzToCam[i] != 0;
                }
            } else if (is("RAWC") && want >= 32) {
                const int x = b[24] + b[25], y = b[26] + b[27];  // binning and skipping, across and down
                if (x > 0 && y > 0) info.pixelAspect = double(x) / y;
            } else if (is("WAVI") && want >= 32) {
                info.channels = u16(b + 18);
                info.sampleRate = int(u32(b + 20));
                info.bitsPerSample = u16(b + 30);
            } else if (is("IDNT") && want >= 84) {
                info.camera = text(b + 16, 32);
            } else if (is("LENS") && want >= 96) {
                info.focalLength = u16(b + 16);
                info.aperture = u16(b + 20) / 100.0;
                info.lens = text(b + 32, 32);
            } else if (is("EXPO") && want >= 40) {
                info.iso = int(u32(b + 20));
                info.shutter = double(u64(b + 32)) / 1e6;
            } else if (is("WBAL") && want >= 44) {
                wbMode = int(u32(b + 16));
                wbKelvin = int(u32(b + 20));
                gain[0] = u32(b + 24), gain[1] = u32(b + 28), gain[2] = u32(b + 32);
            } else if (is("RTCI") && want >= 44 && info.created.empty()) {
                char t[64];
                std::snprintf(t, sizeof t, "%04d-%02d-%02dT%02d:%02d:%02d", 1900 + u16(b + 26), u16(b + 24) + 1, u16(b + 22), u16(b + 20),
                              u16(b + 18), u16(b + 16));
                info.created = t;
            }
            at += blockSize;
        }
        index->chunks.push_back(file);
        if (u16(head + 26) <= 1 && chunk == 0) {
            // (fileCount: a recording in one file, unless the chunks are there anyway)
            if (!QFile::exists(QString::fromStdString(chunkPath(path, 1)))) break;
        }
    }
    if (!rawi || info.width < 2 || info.height < 2 || info.width > kMaxSide || info.height > kMaxSide)
        return fail(path + " has no usable RAW frame description");
    if (frames.empty()) return fail(path + " has no frames");
    // Frames by number, from the first recorded (a number far past the frames there are is damage, left out).
    auto byNumber = [](const Numbered& a, const Numbered& b) { return a.n < b.n; };
    std::stable_sort(frames.begin(), frames.end(), byNumber);
    const uint32_t first = frames.front().n;
    const uint64_t limit = uint64_t(frames.size()) * 2 + 1000;
    for (const Numbered& fr : frames) {
        const uint64_t k = uint64_t(fr.n) - first;
        if (k >= limit) break;
        if (index->frames.size() <= k) index->frames.resize(size_t(k) + 1);
        if (index->frames[size_t(k)].size == 0) index->frames[size_t(k)] = fr.block;
    }
    std::stable_sort(sound.begin(), sound.end(), byNumber);
    if (info.bitsPerPixel < 8 || info.bitsPerPixel > 16) info.bitsPerPixel = 14;
    if (info.white <= info.black) info.white = (1 << info.bitsPerPixel) - 1;
    if (info.fpsNum <= 0 || info.fpsDen <= 0) info.fpsNum = 25, info.fpsDen = 1;
    info.frames = int(index->frames.size());
    info.chunks = int(index->chunks.size());
    if (!(info.pixelAspect > 0.2 && info.pixelAspect < 5)) info.pixelAspect = 1;
    if (info.sampleRate > 0 && info.channels > 0 && info.channels <= 64 && (info.bitsPerSample == 16 || info.bitsPerSample == 24)) {
        const int64_t bytesPerFrame = int64_t(info.channels) * (info.bitsPerSample / 8);
        for (const Numbered& a : sound) {
            index->sound.push_back(a.block);
            info.soundFrames += a.block.size / bytesPerFrame;
        }
    }
    info.hasAudio = info.soundFrames > 0;
    info.kelvin = presetKelvin(wbMode, wbKelvin);
    // Levels as fractions of 16 bits (the samples are scaled to 16 bits).
    const double scale = double(1 << (16 - info.bitsPerPixel)) / 65535.0;
    info.color.black = info.black * scale;
    info.color.white = info.white * scale;
    info.color.gain = 1;
    // White balance as shot: Canon's custom gains, else its preset's or set colour temperature, else daylight (D65).
    if (matrix) {
        double camWhite[3] = {1, 1, 1};
        auto fromXyz = [&](double x, double y) {
            const double xyz[3] = {x / y, 1, (1 - x - y) / y};
            for (int r = 0; r < 3; ++r) camWhite[r] = xyzToCam[r * 3] * xyz[0] + xyzToCam[r * 3 + 1] * xyz[1] + xyzToCam[r * 3 + 2] * xyz[2];
        };
        if (wbMode == 6 && gain[0] && gain[1] && gain[2]) {
            camWhite[0] = double(gain[1]) / gain[0], camWhite[1] = 1, camWhite[2] = double(gain[1]) / gain[2];
        } else if (info.kelvin >= 2000 && info.kelvin <= 25000) {
            double x, y;
            planckianChromaticity(info.kelvin, x, y);
            fromXyz(x, y);
        } else {
            fromXyz(0.3127, 0.3290);
        }
        if (camWhite[0] > 0 && camWhite[1] > 0 && camWhite[2] > 0) {
            const double green = camWhite[1];
            for (double& v : camWhite) v /= green;
            colourFrom(xyzToCam, camWhite, info);
        }
    }
    return index;
}

// The indexes of files read, while each is unchanged (its and its chunks' sizes and times), so the decoders,
// thumbnails and analyses opening the same recording read its blocks once.
std::shared_ptr<const MlvIndex> sharedIndex(const std::string& path, std::string* error) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<std::string, std::shared_ptr<const MlvIndex>>> cache;
    std::string stamp;
    for (int chunk = 0; chunk <= 100; ++chunk) {
        const QFileInfo fi(QString::fromStdString(chunk == 0 ? path : chunkPath(path, chunk)));
        if (!fi.exists()) break;
        stamp += std::to_string(fi.size()) + ":" + std::to_string(fi.lastModified().toMSecsSinceEpoch()) + ";";
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(path);
        if (it != cache.end() && it->second.first == stamp) return it->second.second;
    }
    auto index = indexMlv(path, error);
    if (!index) return index;
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 64) cache.clear();
    cache[path] = {stamp, index};
    return index;
}

}  // namespace

MlvReader::MlvReader() = default;
MlvReader::~MlvReader() = default;

bool MlvReader::open(const std::string& path, std::string* error) {
    index_.reset();
    file_.reset();
    fileChunk_ = -1;
    index_ = sharedIndex(path, error);
    return bool(index_);
}

const MlvInfo& MlvReader::info() const {
    static const MlvInfo none;
    return index_ ? index_->info : none;
}

bool MlvReader::readBlock(int chunk, int64_t offset, int64_t size, std::string* error) {
    if (chunk != fileChunk_) {
        file_ = std::make_unique<QFile>(QString::fromStdString(index_->chunks[size_t(chunk)]));
        fileChunk_ = chunk;
        if (!file_->open(QIODevice::ReadOnly)) {
            fileChunk_ = -1;
            if (error) *error = "Cannot open " + index_->chunks[size_t(chunk)];
            return false;
        }
    }
    buffer_.resize(size_t(size));
    if (!file_->seek(offset) || file_->read(reinterpret_cast<char*>(buffer_.data()), qint64(size)) != qint64(size)) {
        if (error) *error = "Cannot read " + index_->chunks[size_t(chunk)];
        return false;
    }
    return true;
}

bool MlvReader::loadMosaic(int n, std::string* error) {
    if (!index_) return false;
    const std::vector<MlvIndex::Block>& frames = index_->frames;
    n = std::clamp(n, 0, int(frames.size()) - 1);
    while (n > 0 && frames[size_t(n)].size <= 0) --n;  // (a frame missing: the one before; the first is there)
    const MlvIndex::Block& fr = frames[size_t(n)];
    if (!readBlock(fr.chunk, fr.offset, fr.size, error)) return false;
    const MlvInfo& info = index_->info;
    const int w = info.width, h = info.height;
    const size_t count = size_t(w) * size_t(h);
    const int shift = 16 - info.bitsPerPixel;
    if (info.lossless) {
        int jw = 0, jh = 0, jc = 0, jbits = 0;
        if (!decodeLosslessJpeg(buffer_.data(), buffer_.size(), mosaic_, jw, jh, jc, jbits, error, count)) return false;
        mosaic_.resize(count);  // (any padding the encoder added after the picture dropped)
        // (the samples in raster order, whatever the components; scaled to 16 bits)
        if (shift > 0)
            parallelRows(h, [&](int y0, int y1) {
                for (size_t i = size_t(y0) * size_t(w), e = size_t(y1) * size_t(w); i < e; ++i)
                    mosaic_[i] = uint16_t(std::min<uint32_t>(65535, uint32_t(mosaic_[i]) << shift));
            });
        return true;
    }
    // Uncompressed: samples of bitsPerPixel bits, most significant first, in little-endian 16-bit words.
    const int bpp = info.bitsPerPixel;
    const size_t needBytes = (count * size_t(bpp) + 7) / 8;
    if (buffer_.size() < needBytes) {
        if (error) *error = "The frame is cut short";
        return false;
    }
    mosaic_.resize(count);
    const uint8_t* src = buffer_.data();
    const size_t words = buffer_.size() / 2;
    const uint32_t mask = (1u << bpp) - 1;
    parallelRows(h, [&](int y0, int y1) {
        // (each row from its first sample's bit)
        const uint64_t bit = uint64_t(y0) * uint64_t(w) * uint64_t(bpp);
        size_t w16 = size_t(bit / 16);
        int have = 0;
        uint64_t acc = 0;
        int skip = int(bit % 16);
        uint16_t* out = mosaic_.data() + size_t(y0) * size_t(w);
        const size_t n = size_t(y1 - y0) * size_t(w);
        for (size_t i = 0; i < n; ++i) {
            while (have < bpp + skip) {
                const uint32_t word = w16 < words ? u16(src + w16 * 2) : 0;
                ++w16;
                acc = (acc << 16) | word;
                have += 16;
            }
            have -= skip;
            skip = 0;
            have -= bpp;
            out[i] = uint16_t(((acc >> have) & mask) << shift);
        }
    });
    return true;
}

bool MlvReader::readMosaic(int n, std::vector<uint16_t>& out, std::string* error) {
    if (!loadMosaic(n, error)) return false;
    out = mosaic_;
    return true;
}

namespace {
// The pattern's red site (as DNG's CFAPattern packs it, one byte a site: 0 red, 1 green, 2 blue); red, green / green,
// blue if it is not a Bayer pattern.
void redSite(const MlvInfo& info, int& sx, int& sy) {
    int count[3] = {0, 0, 0}, red = 0;
    for (int i = 0; i < 4; ++i) {
        const int v = (info.cfa >> (i * 8)) & 0xFF;
        if (v > 2) return void(sx = sy = 0);
        ++count[v];
        if (v == 0) red = i;
    }
    if (count[0] != 1 || count[1] != 2 || count[2] != 1) red = 0;
    sx = red & 1, sy = red >> 1;
}

// The mosaic developed: from the red site, in whole 2x2 cells.
void developedMosaic(const MlvInfo& info, int& sx, int& sy, int& w, int& h) {
    redSite(info, sx, sy);
    w = (info.width - sx) & ~1;
    h = (info.height - sy) & ~1;
}
}  // namespace

void mlvPictureSize(const MlvInfo& info, bool half, int& width, int& height) {
    int sx, sy, w, h;
    developedMosaic(info, sx, sy, w, h);
    const int crop[4] = {0, 0, 0, 0};
    proResRawSize(w, h, crop, half, width, height);
}

bool MlvReader::develop(int n, const RawSettings& settings, uint16_t* rgb, ptrdiff_t stride, std::string* error) {
    if (!loadMosaic(n, error)) return false;
    const MlvInfo& info = index_->info;
    int sx, sy, w, h;
    developedMosaic(info, sx, sy, w, h);
    const int crop[4] = {0, 0, 0, 0};
    developProResRaw(mosaic_.data() + size_t(sy) * size_t(info.width) + size_t(sx), w, h, info.width, info.color, settings, crop, rgb,
                     stride);
    return true;
}

bool MlvReader::develop(int n, const RawSettings& settings, DevelopedRaw& out, std::string* error) {
    if (!index_) return false;
    mlvPictureSize(index_->info, settings.half, out.width, out.height);
    out.rgb.resize(size_t(out.width) * size_t(out.height) * 3);
    if (!develop(n, settings, out.rgb.data(), ptrdiff_t(out.width) * 3, error)) return false;
    return out.width > 0 && out.height > 0;
}

bool MlvReader::readSound(int64_t from, const std::function<bool(const int32_t*, size_t)>& sink, std::string* error) {
    if (!index_ || !index_->info.hasAudio) {
        if (error) *error = "No audio stream";
        return false;
    }
    const MlvInfo& info = index_->info;
    const int bytes = info.bitsPerSample / 8;
    const int64_t perFrame = int64_t(info.channels) * bytes;
    int64_t at = 0;  // the sample frame each block starts at
    for (const MlvIndex::Block& a : index_->sound) {
        const int64_t frames = a.size / perFrame;
        if (at + frames <= from) {
            at += frames;
            continue;
        }
        if (!readBlock(a.chunk, a.offset, frames * perFrame, error)) return false;
        const int64_t skip = std::max<int64_t>(0, from - at);
        const size_t n = size_t((frames - skip) * info.channels);
        sound_.resize(n);
        const uint8_t* p = buffer_.data() + skip * perFrame;
        if (bytes == 2)
            for (size_t i = 0; i < n; ++i, p += 2) sound_[i] = int32_t(uint32_t(u16(p)) << 16);
        else
            for (size_t i = 0; i < n; ++i, p += 3) sound_[i] = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24));
        if (!sink(sound_.data(), size_t(frames - skip))) return true;
        at += frames;
    }
    return true;
}

bool readMlvInfo(const std::string& path, MlvInfo& out, std::string* error) {
    MlvReader r;
    if (!r.open(path, error)) return false;
    out = r.info();
    return true;
}

}  // namespace montage
