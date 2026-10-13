#include "Mlv.h"

#include <QFile>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

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
            if (len <= 9)
                for (int fill = 0; fill < (1 << (9 - len)); ++fill) h.fast[(code << (9 - len)) | fill] = uint16_t((len << 8) | values[k]);
        }
        h.maxcode[len] = counts[len - 1] ? code - 1 : -1;
        if (code > (1 << len)) return false;
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
                        int& bits, std::string* error) {
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
            restartInterval = (seg[0] << 8) | seg[1];
        } else if (marker == 0xDA) {
            if (!width) return fail("Scan before the frame header");
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
            out.assign(rowLen * size_t(height), 0);
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

MlvReader::MlvReader() = default;
MlvReader::~MlvReader() = default;

bool MlvReader::open(const std::string& path, std::string* error) {
    files_.clear();
    frames_.clear();
    info_ = MlvInfo{};
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        files_.clear();
        frames_.clear();
        return false;
    };
    bool rawi = false;
    double xyzToCam[9] = {};
    bool matrix = false;
    int wbMode = -1, wbKelvin = 0;
    uint32_t gain[3] = {0, 0, 0};
    int audioFrames = 0;
    for (int chunk = 0; chunk < 100; ++chunk) {
        const std::string file = chunk == 0 ? path : chunkPath(path, chunk);
        auto f = std::make_unique<QFile>(QString::fromStdString(file));
        if (!f->open(QIODevice::ReadOnly)) {
            if (chunk == 0) return fail("Cannot open " + path);
            break;  // (no more chunks)
        }
        uint8_t head[52];
        if (f->read(reinterpret_cast<char*>(head), 52) != 52 || std::memcmp(head, "MLVI", 4) != 0) {
            if (chunk == 0) return fail(path + " is not a Magic Lantern Video file");
            break;
        }
        const uint32_t headerSize = u32(head + 4);
        if (chunk == 0) {
            const uint16_t videoClass = u16(head + 32);
            info_.fpsNum = int(u32(head + 44));
            info_.fpsDen = int(u32(head + 48));
            info_.lossless = (videoClass & 0x20) != 0;
            if ((videoClass & 0x0F) != 0x01) return fail(path + " holds no RAW video (its frames are JPEG, H.264 or YUV)");
            if (videoClass & 0xC0) return fail(path + " is compressed with LZMA or delta frames (MLV App's own), which this does not read");
        }
        const int64_t total = f->size();
        int64_t at = std::max<int64_t>(16, headerSize);
        uint8_t b[256];
        while (at + 16 <= total) {
            f->seek(at);
            if (f->read(reinterpret_cast<char*>(b), 16) != 16) break;
            const uint32_t blockSize = u32(b + 4);
            if (blockSize < 16 || at + blockSize > total) break;  // (a recording cut short: what came before is kept)
            const size_t want = std::min<size_t>(sizeof b, blockSize);
            if (want > 16 && f->read(reinterpret_cast<char*>(b + 16), qint64(want - 16)) != qint64(want - 16)) break;
            auto is = [&](const char* t) { return std::memcmp(b, t, 4) == 0; };
            if (is("VIDF") && want >= 32) {
                const uint32_t n = u32(b + 16), space = u32(b + 28);
                if (n < 10000000 && 32 + int64_t(space) <= int64_t(blockSize)) {
                    if (frames_.size() <= n) frames_.resize(size_t(n) + 1);
                    frames_[n] = Frame{int(files_.size()), at + 32 + space, int64_t(blockSize) - 32 - space};
                }
            } else if (is("RAWI") && want >= 180) {
                rawi = true;
                info_.width = u16(b + 16);
                info_.height = u16(b + 18);
                const uint8_t* r = b + 20;  // raw_info
                info_.bitsPerPixel = i32(r + 24);
                info_.black = i32(r + 28);
                info_.white = i32(r + 32);
                info_.cfa = i32(r + 76);
                matrix = false;
                for (int i = 0; i < 9; ++i) {
                    const int num = i32(r + 84 + i * 8), den = i32(r + 88 + i * 8);
                    xyzToCam[i] = den ? double(num) / den : 0;
                    matrix = matrix || xyzToCam[i] != 0;
                }
            } else if (is("WAVI") && want >= 32) {
                info_.channels = u16(b + 18);
                info_.sampleRate = int(u32(b + 20));
                info_.bitsPerSample = u16(b + 30);
            } else if (is("AUDF")) {
                ++audioFrames;
            } else if (is("IDNT") && want >= 84) {
                info_.camera = text(b + 16, 32);
            } else if (is("LENS") && want >= 96) {
                info_.focalLength = u16(b + 16);
                info_.aperture = u16(b + 20) / 100.0;
                info_.lens = text(b + 32, 32);
            } else if (is("EXPO") && want >= 40) {
                info_.iso = int(u32(b + 20));
                info_.shutter = double(u64(b + 32)) / 1e6;
            } else if (is("WBAL") && want >= 44) {
                wbMode = int(u32(b + 16));
                wbKelvin = int(u32(b + 20));
                gain[0] = u32(b + 24), gain[1] = u32(b + 28), gain[2] = u32(b + 32);
            } else if (is("RTCI") && want >= 44 && info_.created.empty()) {
                char t[64];
                std::snprintf(t, sizeof t, "%04d-%02d-%02dT%02d:%02d:%02d", 1900 + u16(b + 26), u16(b + 24) + 1, u16(b + 22), u16(b + 20),
                              u16(b + 18), u16(b + 16));
                info_.created = t;
            }
            at += blockSize;
        }
        files_.push_back(std::move(f));
        if (u16(head + 26) <= 1 && chunk == 0) {
            // (fileCount: a recording in one file, unless the chunks are there anyway)
            if (!QFile::exists(QString::fromStdString(chunkPath(path, 1)))) break;
        }
    }
    if (!rawi || info_.width <= 0 || info_.height <= 0) return fail(path + " has no RAW frame description");
    if (frames_.empty()) return fail(path + " has no frames");
    if (info_.bitsPerPixel < 8 || info_.bitsPerPixel > 16) info_.bitsPerPixel = 14;
    if (info_.white <= info_.black) info_.white = (1 << info_.bitsPerPixel) - 1;
    if (info_.fpsNum <= 0 || info_.fpsDen <= 0) info_.fpsNum = 25, info_.fpsDen = 1;
    info_.frames = int(frames_.size());
    info_.chunks = int(files_.size());
    info_.hasAudio = audioFrames > 0 && info_.sampleRate > 0 && info_.channels > 0;
    info_.kelvin = wbKelvin;
    // Levels as fractions of 16 bits (the samples are scaled to 16 bits).
    const double scale = double(1 << (16 - info_.bitsPerPixel)) / 65535.0;
    info_.color.black = info_.black * scale;
    info_.color.white = info_.white * scale;
    info_.color.gain = 1;
    // White balance as shot: Canon's custom gains, else its colour temperature, else daylight (D65).
    if (matrix) {
        double camWhite[3] = {1, 1, 1};
        auto fromXyz = [&](double x, double y) {
            const double xyz[3] = {x / y, 1, (1 - x - y) / y};
            for (int r = 0; r < 3; ++r) camWhite[r] = xyzToCam[r * 3] * xyz[0] + xyzToCam[r * 3 + 1] * xyz[1] + xyzToCam[r * 3 + 2] * xyz[2];
        };
        if (wbMode == 6 && gain[0] && gain[1] && gain[2]) {
            camWhite[0] = double(gain[1]) / gain[0], camWhite[1] = 1, camWhite[2] = double(gain[1]) / gain[2];
        } else if (wbKelvin >= 2000 && wbKelvin <= 25000) {
            double x, y;
            planckianChromaticity(wbKelvin, x, y);
            fromXyz(x, y);
        } else {
            fromXyz(0.3127, 0.3290);
        }
        if (camWhite[0] > 0 && camWhite[1] > 0 && camWhite[2] > 0) {
            const double green = camWhite[1];
            for (double& v : camWhite) v /= green;
            colourFrom(xyzToCam, camWhite, info_);
        }
    }
    return true;
}

bool MlvReader::readMosaic(int n, std::vector<uint16_t>& out, std::string* error) {
    if (frames_.empty()) return false;
    n = std::clamp(n, 0, int(frames_.size()) - 1);
    while (n > 0 && frames_[size_t(n)].size <= 0) --n;  // (a frame missing: the one before)
    const Frame& fr = frames_[size_t(n)];
    if (fr.size <= 0) {
        if (error) *error = "The frame is missing";
        return false;
    }
    QFile& f = *files_[size_t(fr.chunk)];
    buffer_.resize(size_t(fr.size));
    if (!f.seek(fr.offset) || f.read(reinterpret_cast<char*>(buffer_.data()), qint64(fr.size)) != qint64(fr.size)) {
        if (error) *error = "Cannot read frame " + std::to_string(n);
        return false;
    }
    const int w = info_.width, h = info_.height;
    const size_t count = size_t(w) * size_t(h);
    out.resize(count);
    if (info_.lossless) {
        std::vector<uint16_t> samples;
        int jw = 0, jh = 0, jc = 0, jbits = 0;
        if (!decodeLosslessJpeg(buffer_.data(), buffer_.size(), samples, jw, jh, jc, jbits, error)) return false;
        if (samples.size() < count) {
            if (error) *error = "The frame is smaller than the picture";
            return false;
        }
        // (the samples in raster order, whatever the components; scaled to 16 bits)
        const int shift = 16 - std::clamp(info_.bitsPerPixel, 8, 16);
        for (size_t i = 0; i < count; ++i) out[i] = uint16_t(std::min<uint32_t>(65535, uint32_t(samples[i]) << shift));
        return true;
    }
    // Uncompressed: samples of bitsPerPixel bits, most significant first, in little-endian 16-bit words.
    const int bpp = info_.bitsPerPixel;
    const size_t needBytes = (count * size_t(bpp) + 7) / 8;
    if (buffer_.size() < needBytes) {
        if (error) *error = "The frame is cut short";
        return false;
    }
    const uint8_t* src = buffer_.data();
    const size_t words = buffer_.size() / 2;
    const int shift = 16 - bpp;
    const uint32_t mask = (1u << bpp) - 1;
    uint64_t acc = 0;
    int have = 0;
    size_t w16 = 0;
    for (size_t i = 0; i < count; ++i) {
        while (have < bpp) {
            const uint32_t word = w16 < words ? u16(src + w16 * 2) : 0;
            ++w16;
            acc = (acc << 16) | word;
            have += 16;
        }
        have -= bpp;
        out[i] = uint16_t(((acc >> have) & mask) << shift);
    }
    return true;
}

namespace {
void redSite(const MlvInfo& info, int& sx, int& sy) {
    int red = 0;
    for (int i = 0; i < 4; ++i)
        if (((info.cfa >> (i * 8)) & 0xFF) == 0) red = i;
    sx = red & 1, sy = red >> 1;
}
}  // namespace

void mlvPictureSize(const MlvInfo& info, bool half, int& width, int& height) {
    int sx = 0, sy = 0;
    redSite(info, sx, sy);
    const int crop[4] = {0, 0, 0, 0};
    proResRawSize(info.width - sx, info.height - sy, crop, half, width, height);
}

bool MlvReader::develop(int n, const RawSettings& settings, DevelopedRaw& out, std::string* error) {
    std::vector<uint16_t> mosaic;
    if (!readMosaic(n, mosaic, error)) return false;
    // The pattern's red site: the mosaic started there, so it reads as red, green / green, blue.
    int sx = 0, sy = 0;
    redSite(info_, sx, sy);
    const int crop[4] = {0, 0, 0, 0};
    developProResRaw(mosaic.data() + size_t(sy) * size_t(info_.width) + size_t(sx), info_.width - sx, info_.height - sy, info_.width, info_.color, settings,
                     crop, out);
    return out.width > 0 && out.height > 0;
}

bool readMlvInfo(const std::string& path, MlvInfo& out, std::string* error) {
    MlvReader r;
    if (!r.open(path, error)) return false;
    out = r.info();
    return true;
}

}  // namespace montage
