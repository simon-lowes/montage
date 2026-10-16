#include "MonitorCalibration.h"

#include <QColor>
#include <QXmlStreamReader>
#include <QtCore/qfloat16.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "render/Clf.h"
#include "render/ColorSpace.h"
#include "render/Processing.h"

namespace montage {

namespace {

std::filesystem::path fsPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Read files, by path, with the file's time and size when read.
struct Cached {
    std::filesystem::file_time_type time;
    uintmax_t size = 0;
    std::shared_ptr<const Lut3D> lut;
    std::string error;  // (a file that could not be read is not read again until it changes)
};
std::mutex gCacheMutex;
std::map<std::string, Cached> gCache;

std::shared_ptr<const Lut3D> bakeClf(const ClfTransform& clf) {
    auto lut = std::make_shared<Lut3D>();
    const int n = 65;
    lut->size = n;
    lut->data.resize(size_t(n) * n * n * 3);
    parallelRows(n, [&](int b0, int b1) {
        for (int b = b0; b < b1; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    float v[3] = {float(r) / float(n - 1), float(g) / float(n - 1), float(b) / float(n - 1)};
                    clf.apply(v);
                    float* d = &lut->data[((size_t(b) * n + size_t(g)) * n + size_t(r)) * 3];
                    d[0] = v[0], d[1] = v[1], d[2] = v[2];
                }
    });
    return lut;
}

// The lattice position of each 8-bit level along one axis of `lut` (as Lut3D::apply places it).
struct Axis {
    int i0[256];
    float f[256];
};
void axisFor(const Lut3D& lut, int c, Axis& a) {
    const float span = lut.domainMax[c] - lut.domainMin[c];
    for (int v = 0; v < 256; ++v) {
        const float in = float(v) / 255.0f;
        const float u = std::clamp(span > 0 ? (in - lut.domainMin[c]) / span : in, 0.0f, 1.0f) * float(lut.size - 1);
        a.i0[v] = std::min(lut.size - 2, int(u));
        a.f[v] = u - float(a.i0[v]);
    }
}

uint8_t to8(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

}  // namespace

std::shared_ptr<const Lut3D> parse3dl(const std::string& text, std::string* error) {
    auto fail = [&](const std::string& why) -> std::shared_ptr<const Lut3D> {
        if (error) *error = why;
        return nullptr;
    };
    std::istringstream in(text);
    std::string line;
    int meshBits = 0, outBits = 0;
    std::vector<double> shaper;
    std::vector<double> values;
    bool floats = false;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string first;
        if (!(ls >> first)) continue;
        const std::string key = lower(first);
        if (key == "3dmesh") continue;
        if (key == "mesh") {
            ls >> meshBits >> outBits;  // (2^meshBits + 1 points, `outBits` output)
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(first[0])) && first[0] != '-' && first[0] != '.') continue;  // (other keywords)
        std::vector<double> row;
        std::istringstream all(line);
        std::string word;
        while (all >> word) {
            if (word.find_first_of(".eE") != std::string::npos) floats = true;
            try {
                row.push_back(std::stod(word));
            } catch (...) {
                return fail("Malformed .3dl LUT: \"" + word + "\" is not a number");
            }
        }
        if (shaper.empty()) shaper = row;  // the input lattice (always the first line of numbers)
        else if (row.size() == 3) values.insert(values.end(), row.begin(), row.end());
        else return fail("Malformed .3dl LUT: a row with " + std::to_string(row.size()) + " values");
    }
    const int n = int(shaper.size());
    if (n < 2) return fail("Malformed .3dl LUT: no input lattice line");
    if (values.size() != size_t(n) * n * n * 3)
        return fail("Malformed .3dl LUT: " + std::to_string(values.size() / 3) + " entries for a " + std::to_string(n) + "-point lattice");
    for (int i = 1; i < n; ++i)
        if (shaper[size_t(i)] <= shaper[size_t(i) - 1]) return fail("Malformed .3dl LUT: the input lattice does not rise");
    // Integer depths: from the header, else the smallest usual one that holds every value (the lattice's last point
    // for the input).
    auto depthMax = [](int bits, double largest) {
        if (bits > 0) return std::pow(2.0, bits) - 1;
        for (double m : {1023.0, 4095.0, 65535.0})
            if (largest <= m) return m;
        return largest;
    };
    if (meshBits > 0 && meshBits < 10 && n != (1 << meshBits) + 1)
        return fail("Malformed .3dl LUT: its header says " + std::to_string((1 << meshBits) + 1) + " points, its lattice has " + std::to_string(n));
    const double inMax = floats ? 1.0 : depthMax(0, shaper.back());
    const double outMax = floats ? 1.0 : depthMax(outBits, *std::max_element(values.begin(), values.end()));
    auto lut = std::make_shared<Lut3D>();
    lut->size = n;
    for (int c = 0; c < 3; ++c) {
        lut->domainMin[c] = float(shaper.front() / inMax);
        lut->domainMax[c] = float(shaper.back() / inMax);
    }
    lut->data.resize(values.size());
    // Blue varies fastest in the file; red does in a Lut3D.
    size_t k = 0;
    for (int r = 0; r < n; ++r)
        for (int g = 0; g < n; ++g)
            for (int b = 0; b < n; ++b, k += 3) {
                float* d = &lut->data[((size_t(b) * n + size_t(g)) * n + size_t(r)) * 3];
                for (int c = 0; c < 3; ++c) d[c] = float(values[k + size_t(c)] / outMax);
            }
    return lut;
}

std::shared_ptr<const Lut3D> loadCalibrationLut(const std::string& path, std::string* error) {
    std::error_code ec;
    const std::filesystem::path p = fsPath(path);
    const auto time = std::filesystem::last_write_time(p, ec);
    const uintmax_t size = ec ? 0 : std::filesystem::file_size(p, ec);
    if (ec) {
        if (error) *error = "Cannot open LUT " + path;
        return nullptr;
    }
    {
        std::lock_guard lock(gCacheMutex);
        auto it = gCache.find(path);
        if (it != gCache.end() && it->second.time == time && it->second.size == size) {
            if (!it->second.lut && error) *error = it->second.error;
            return it->second.lut;
        }
    }
    std::string why;
    const std::u8string e8 = p.extension().u8string();
    const std::string ext = lower(std::string(e8.begin(), e8.end()));
    std::shared_ptr<const Lut3D> lut;
    if (ext == ".cube") {
        forgetCubeLut(path);  // (rewritten by the calibration software since)
        lut = loadCubeLut(path, &why);
    } else if (ext == ".3dl") {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        if (in) ss << in.rdbuf();
        lut = in ? parse3dl(ss.str(), &why) : nullptr;
        if (!in) why = "Cannot open LUT " + path;
    } else if (ext == ".clf" || ext == ".ctf") {
        if (auto clf = loadClf(path, &why)) lut = bakeClf(*clf);
    } else {
        why = "A calibration LUT is a .cube, .3dl or .clf file";
    }
    if (!lut && error) *error = why;
    std::lock_guard lock(gCacheMutex);
    if (gCache.size() > 32) gCache.clear();
    gCache[path] = {time, size, lut, lut ? std::string() : why};
    return lut;
}

QImage calibratedImage(const QImage& src, const Lut3D& lut) {
    QImage img = src.format() == QImage::Format_RGBA8888 || src.format() == QImage::Format_RGBX8888
                     ? src.copy()
                     : src.convertToFormat(QImage::Format_RGBA8888);
    if (img.isNull() || lut.size < 2) return img;
    const int w = img.width();
    uchar* bits = img.bits();
    const qsizetype bpl = img.bytesPerLine();
    if (lut.is1D) {
        uint8_t table[3][256];
        for (int v = 0; v < 256; ++v) {
            float rgb[3] = {float(v) / 255.0f, float(v) / 255.0f, float(v) / 255.0f};
            lut.apply(rgb[0], rgb[1], rgb[2]);
            for (int c = 0; c < 3; ++c) table[c][v] = to8(rgb[c]);
        }
        parallelRows(img.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                uchar* d = bits + y * bpl;
                for (int x = 0; x < w; ++x, d += 4)
                    for (int c = 0; c < 3; ++c) d[c] = table[c][d[c]];
            }
        });
        return img;
    }
    Axis ax[3];
    for (int c = 0; c < 3; ++c) axisFor(lut, c, ax[c]);
    const size_t n = size_t(lut.size);
    const float* data = lut.data.data();
    parallelRows(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uchar* d = bits + y * bpl;
            for (int x = 0; x < w; ++x, d += 4) {
                const int x0 = ax[0].i0[d[0]], yy0 = ax[1].i0[d[1]], z0 = ax[2].i0[d[2]];
                const float fx = ax[0].f[d[0]], fy = ax[1].f[d[1]], fz = ax[2].f[d[2]];
                const float* p000 = data + ((size_t(z0) * n + size_t(yy0)) * n + size_t(x0)) * 3;
                const size_t dx = 3, dy = n * 3, dz = n * n * 3;
                for (int c = 0; c < 3; ++c) {
                    const float c00 = p000[c] * (1 - fx) + p000[dx + c] * fx;
                    const float c10 = p000[dy + c] * (1 - fx) + p000[dy + dx + c] * fx;
                    const float c01 = p000[dz + c] * (1 - fx) + p000[dz + dx + c] * fx;
                    const float c11 = p000[dz + dy + c] * (1 - fx) + p000[dz + dy + dx + c] * fx;
                    const float c0 = c00 * (1 - fy) + c10 * fy, c1 = c01 * (1 - fy) + c11 * fy;
                    d[c] = to8(c0 * (1 - fz) + c1 * fz);
                }
            }
        }
    });
    return img;
}

void calibrateImage(Image& img, const Lut3D& lut) {
    if (lut.size < 2) return;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                // (premultiplied: the colour shown over black)
                lut.apply(p[0], p[1], p[2]);
                p[3] = 1;
            }
        }
    });
}

void calibrateHdrPicture(std::vector<uint16_t>& rgba, int width, int height, const Lut3D& lut, double sdrWhiteNits) {
    if (lut.size < 2 || rgba.size() < size_t(width) * size_t(height) * 4) return;
    double to2020[9], to709[9];
    primariesMatrix(Primaries::Bt709, Primaries::Bt2020, to2020);
    primariesMatrix(Primaries::Bt2020, Primaries::Bt709, to709);
    // PQ code value <-> linear light relative to SDR white, through tables (the curve is costly per pixel): linear to
    // code over the square root of the light (fine near black), code to linear over the code.
    constexpr int kTable = 4096;
    const double maxLinear = toLinear(Transfer::Pq, 1.0);
    static std::vector<float> encode, decode;
    static std::once_flag once;
    std::call_once(once, [&] {
        encode.resize(kTable + 1);
        decode.resize(kTable + 1);
        for (int i = 0; i <= kTable; ++i) {
            const double s = double(i) / kTable;
            encode[size_t(i)] = float(fromLinear(Transfer::Pq, s * s * maxLinear));
            decode[size_t(i)] = float(toLinear(Transfer::Pq, s));
        }
    });
    auto lookup = [](const std::vector<float>& t, float u) {
        const float x = std::clamp(u, 0.0f, 1.0f) * float(kTable);
        const int i = std::min(kTable - 1, int(x));
        const float f = x - float(i);
        return t[size_t(i)] * (1 - f) + t[size_t(i) + 1] * f;
    };
    const float invMax = float(1.0 / maxLinear);
    // (the picture's 1.0 is the display's SDR white; PQ's transfer here counts 203 nits as 1.0)
    const float toPq = float(std::max(1.0, sdrWhiteNits) / 203.0), fromPq = 1.0f / toPq;
    parallelRows(height, [&](int y0, int y1) {
        std::vector<float> row(size_t(width) * 4);
        for (int y = y0; y < y1; ++y) {
            qfloat16* h = reinterpret_cast<qfloat16*>(&rgba[size_t(y) * size_t(width) * 4]);
            qFloatFromFloat16(row.data(), h, qsizetype(row.size()));
            for (int x = 0; x < width; ++x) {
                float* p = &row[size_t(x) * 4];
                float c[3];
                for (int i = 0; i < 3; ++i)
                    c[i] = float(to2020[i * 3] * p[0] + to2020[i * 3 + 1] * p[1] + to2020[i * 3 + 2] * p[2]);
                for (float& v : c) v = lookup(encode, std::sqrt(std::max(0.0f, v) * toPq * invMax));
                lut.apply(c[0], c[1], c[2]);
                for (float& v : c) v = lookup(decode, v) * fromPq;
                for (int i = 0; i < 3; ++i) p[i] = float(to709[i * 3] * c[0] + to709[i * 3 + 1] * c[1] + to709[i * 3 + 2] * c[2]);
            }
            qFloatToFloat16(h, row.data(), qsizetype(row.size()));
        }
    });
}

// ---------------------------------------------------------------------------

double CalibrationPatch::value(int code) const {
    const double top = std::pow(2.0, std::clamp(bits, 1, 16)) - 1;
    return std::clamp(double(code) / top, 0.0, 1.0);
}

bool parseCalibrationPatch(const std::string& xml, CalibrationPatch& out, std::string* error) {
    QXmlStreamReader r(QByteArray::fromStdString(xml));
    CalibrationPatch p;
    p.shapes.clear();
    int bits = 0;
    int largest = 0;
    bool root = false, haveColour = false;
    CalibrationPatch::Shape single;  // (the colour and geometry given directly under the root)
    CalibrationPatch::Shape* shape = nullptr;
    auto readRgb = [&](int rgb[3]) {
        const auto a = r.attributes();
        rgb[0] = a.value(QLatin1String("red")).toInt();
        rgb[1] = a.value(QLatin1String("green")).toInt();
        rgb[2] = a.value(QLatin1String("blue")).toInt();
        for (int c = 0; c < 3; ++c) largest = std::max(largest, rgb[c]);
        if (a.hasAttribute(QLatin1String("bits"))) bits = a.value(QLatin1String("bits")).toInt();
    };
    auto readGeometry = [&](CalibrationPatch::Shape& s) {
        const auto a = r.attributes();
        s.x = a.value(QLatin1String("x")).toDouble();
        s.y = a.value(QLatin1String("y")).toDouble();
        s.width = a.hasAttribute(QLatin1String("cx")) ? a.value(QLatin1String("cx")).toDouble() : 1.0;
        s.height = a.hasAttribute(QLatin1String("cy")) ? a.value(QLatin1String("cy")).toDouble() : 1.0;
    };
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        const QStringView name = r.name();
        if (!root) {
            if (name != QLatin1String("calibration")) {
                if (error) *error = "Not a calibration message (<" + name.toString().toStdString() + ">)";
                return false;
            }
            root = true;
        } else if (name == QLatin1String("rectangle")) {
            p.shapes.emplace_back();
            shape = &p.shapes.back();
        } else if (name == QLatin1String("color")) {
            readRgb(shape ? shape->rgb : single.rgb);
            if (!shape) haveColour = true;
        } else if (name == QLatin1String("background")) {
            readRgb(p.background);
        } else if (name == QLatin1String("geometry")) {
            readGeometry(shape ? *shape : single);
        }
    }
    if (r.hasError()) {
        if (error) *error = "Malformed calibration message: " + r.errorString().toStdString();
        return false;
    }
    if (!root || (!haveColour && p.shapes.empty())) {
        if (error) *error = "The calibration message names no colour";
        return false;
    }
    if (haveColour) p.shapes.insert(p.shapes.begin(), single);
    // With no depth given, the smallest that holds the values.
    p.bitsGiven = bits > 0;
    p.bits = bits > 0 ? std::clamp(bits, 1, 16) : largest > 1023 ? 12 : largest > 255 ? 10 : 8;
    for (auto& s : p.shapes) {
        s.x = std::clamp(s.x, 0.0, 1.0), s.y = std::clamp(s.y, 0.0, 1.0);
        s.width = std::clamp(s.width, 0.0, 1.0 - s.x), s.height = std::clamp(s.height, 0.0, 1.0 - s.y);
    }
    out = std::move(p);
    return true;
}

std::vector<CalibrationPatch> PatchStream::feed(const char* data, size_t size) {
    std::vector<CalibrationPatch> out;
    if (broken_) return out;
    buffer_.append(data, size);
    size_t at = 0;
    while (buffer_.size() - at >= 4) {
        const auto* b = reinterpret_cast<const unsigned char*>(buffer_.data() + at);
        const uint32_t len = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
        if (len > (1u << 20)) {  // (not this protocol, or out of step: nothing more can be read from this stream)
            error_ = "A message of " + std::to_string(len) + " bytes";
            broken_ = true;
            buffer_.clear();
            return out;
        }
        if (buffer_.size() - at - 4 < len) break;
        CalibrationPatch p;
        std::string why;
        if (parseCalibrationPatch(buffer_.substr(at + 4, len), p, &why)) {
            // With no depth given, the deepest any message so far needed (a dark 10-bit patch may fit in 8 bits).
            if (!p.bitsGiven) p.bits = guessedBits_ = std::max(guessedBits_, p.bits);
            out.push_back(std::move(p));
        } else {
            error_ = why;
        }
        at += 4 + len;
    }
    buffer_.erase(0, at);
    return out;
}

namespace {

// Each shape's pixel rectangle in a W x H picture.
void shapeRect(const CalibrationPatch::Shape& s, int W, int H, int& x0, int& y0, int& x1, int& y1) {
    x0 = int(std::lround(s.x * W)), y0 = int(std::lround(s.y * H));
    x1 = std::min(W, int(std::lround((s.x + s.width) * W))), y1 = std::min(H, int(std::lround((s.y + s.height) * H)));
}

}  // namespace

Image patchFrame(const CalibrationPatch& p, int width, int height, const Lut3D* lut) {
    Image img(width, height, Image::Uninitialized{});
    auto colour = [&](const int rgb[3], float out[3]) {
        for (int c = 0; c < 3; ++c) out[c] = float(p.value(rgb[c]));
        if (lut) lut->apply(out[0], out[1], out[2]);
    };
    float bg[3];
    colour(p.background, bg);
    img.fill(bg[0], bg[1], bg[2], 1);
    for (const auto& s : p.shapes) {
        float c[3];
        colour(s.rgb, c);
        int x0, y0, x1, y1;
        shapeRect(s, width, height, x0, y0, x1, y1);
        for (int y = y0; y < y1; ++y) {
            float* d = img.at(x0, y);
            for (int x = x0; x < x1; ++x, d += 4) d[0] = c[0], d[1] = c[1], d[2] = c[2], d[3] = 1;
        }
    }
    return img;
}

QImage patchImage(const CalibrationPatch& p, int width, int height, const Lut3D* lut) {
    QImage img(width, height, QImage::Format_RGBA8888);
    auto colour = [&](const int rgb[3]) {
        float c[3];
        for (int i = 0; i < 3; ++i) c[i] = float(p.value(rgb[i]));
        if (lut) lut->apply(c[0], c[1], c[2]);
        return QColor(to8(c[0]), to8(c[1]), to8(c[2]));
    };
    img.fill(colour(p.background));
    for (const auto& s : p.shapes) {
        int x0, y0, x1, y1;
        shapeRect(s, width, height, x0, y0, x1, y1);
        const QColor c = colour(s.rgb);
        for (int y = y0; y < y1; ++y) {
            uchar* d = img.scanLine(y) + x0 * 4;
            for (int x = x0; x < x1; ++x, d += 4) d[0] = uchar(c.red()), d[1] = uchar(c.green()), d[2] = uchar(c.blue()), d[3] = 255;
        }
    }
    return img;
}

}  // namespace montage
