#include "media/MotionData.h"

#include "media/Decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
}

namespace montage {

namespace {

// ---- Rotations ---------------------------------------------------------------------------------------------------
using Q = std::array<double, 4>;  // w, x, y, z
using V = std::array<double, 3>;
using M = std::array<double, 9>;  // row-major

Q qmul(const Q& a, const Q& b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3], a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1], a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}
Q qconj(const Q& a) { return {a[0], -a[1], -a[2], -a[3]}; }
Q qnorm(Q a) {
    const double n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    if (n < 1e-300) return {1, 0, 0, 0};
    for (double& v : a) v /= n;
    return a;
}
double qdot(const Q& a, const Q& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }
// The rotation by vector r (its length the angle).
Q qexp(const V& r) {
    const double a = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (a < 1e-12) return qnorm({1, r[0] / 2, r[1] / 2, r[2] / 2});
    const double s = std::sin(a / 2) / a;
    return {std::cos(a / 2), r[0] * s, r[1] * s, r[2] * s};
}
V qlog(Q q) {
    if (q[0] < 0)
        for (double& v : q) v = -v;
    const double s = std::sqrt(q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (s < 1e-12) return {2 * q[1], 2 * q[2], 2 * q[3]};
    const double a = 2 * std::atan2(s, q[0]);
    return {q[1] / s * a, q[2] / s * a, q[3] / s * a};
}
Q qslerp(const Q& a, Q b, double t) {
    if (qdot(a, b) < 0)
        for (double& v : b) v = -v;
    const V r = qlog(qmul(qconj(a), b));
    return qnorm(qmul(a, qexp({r[0] * t, r[1] * t, r[2] * t})));
}
M qmat(const Q& q) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
            2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
            2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
}
V mv(const M& m, const V& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}
V qrot(const Q& q, const V& v) { return mv(qmat(q), v); }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double vlen(const V& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// ---- Readings ----------------------------------------------------------------------------------------------------
constexpr double kStandardGravity = 9.80665;

// Puts the readings in time order and gives each gyro reading the acceleration at its time (from accelerometer
// readings sent apart from the gyro's).
void finish(MotionData& d) {
    // (readings that are not numbers, from a damaged file, are left out)
    auto finite = [](const ImuSample& s) {
        return std::isfinite(s.t) && std::isfinite(s.accel[0]) && std::isfinite(s.accel[1]) && std::isfinite(s.accel[2]) &&
               (s.accelOnly || (std::isfinite(s.gyro[0]) && std::isfinite(s.gyro[1]) && std::isfinite(s.gyro[2])));
    };
    d.samples.erase(std::remove_if(d.samples.begin(), d.samples.end(), [&](const ImuSample& s) { return !finite(s); }), d.samples.end());
    std::stable_sort(d.samples.begin(), d.samples.end(), [](const ImuSample& a, const ImuSample& b) { return a.t < b.t; });
    std::vector<ImuSample> accel, gyro;
    for (const ImuSample& s : d.samples) (s.accelOnly ? accel : gyro).push_back(s);
    if (!accel.empty()) {
        d.hasAccel = true;
        size_t j = 0;
        for (ImuSample& g : gyro) {
            while (j + 1 < accel.size() && accel[j + 1].t <= g.t) ++j;
            const ImuSample& a = accel[j];
            const ImuSample& b = accel[std::min(j + 1, accel.size() - 1)];
            const double f = b.t > a.t ? std::clamp((g.t - a.t) / (b.t - a.t), 0.0, 1.0) : 0.0;
            for (int k = 0; k < 3; ++k) g.accel[size_t(k)] = float(a.accel[size_t(k)] + (b.accel[size_t(k)] - a.accel[size_t(k)]) * f);
        }
    } else {
        for (const ImuSample& g : gyro)
            if (g.accel[0] != 0 || g.accel[1] != 0 || g.accel[2] != 0) d.hasAccel = true;
    }
    d.samples = std::move(gyro);
}

// GPMF: four-character key, type, size of one sample, repeat count (big-endian), the data padded to four bytes.
struct Klv {
    uint32_t key;
    char type;
    int size, repeat;
    const uint8_t* data;
};
uint32_t fourcc(const char* s) { return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]); }

// The value at index i of a GPMF array of numbers.
double gpmfNumber(const Klv& k, size_t i) {
    const uint8_t* p = k.data;
    switch (k.type) {
        case 'b': return int8_t(p[i]);
        case 'B': return p[i];
        case 's': return int16_t(AV_RB16(p + 2 * i));
        case 'S': return AV_RB16(p + 2 * i);
        case 'l': return int32_t(AV_RB32(p + 4 * i));
        case 'L': return AV_RB32(p + 4 * i);
        case 'f': {
            const uint32_t u = AV_RB32(p + 4 * i);
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
        case 'd': {
            const uint64_t u = AV_RB64(p + 8 * i);
            double f;
            std::memcpy(&f, &u, 8);
            return f;
        }
        case 'j': return double(int64_t(AV_RB64(p + 8 * i)));
        case 'J': return double(AV_RB64(p + 8 * i));
        case 'q': return int32_t(AV_RB32(p + 4 * i)) / 65536.0;
        default: return 0;
    }
}
int gpmfTypeSize(char t) {
    switch (t) {
        case 'b': case 'B': case 'c': return 1;
        case 's': case 'S': return 2;
        case 'l': case 'L': case 'f': case 'q': case 'F': return 4;
        case 'd': case 'j': case 'J': case 'Q': return 8;
        default: return 0;
    }
}

// Calls f for each entry at this level; false when they run past the end.
template <typename F>
bool walkKlv(const uint8_t* p, size_t size, F&& f) {
    size_t at = 0;
    while (at + 8 <= size) {
        Klv k{AV_RB32(p + at), char(p[at + 4]), p[at + 5], int(AV_RB16(p + at + 6)), p + at + 8};
        const size_t len = size_t(k.size) * size_t(k.repeat), padded = (len + 3) & ~size_t(3);
        if (k.key == 0) break;  // padding at the end
        if (at + 8 + len > size) return false;
        f(k, len);
        at += 8 + padded;
    }
    return true;
}

void readGpmfStream(const uint8_t* p, size_t size, double t0, double t1, MotionData& out) {
    std::vector<double> scale;
    walkKlv(p, size, [&](const Klv& k, size_t) {
        const int ts = gpmfTypeSize(k.type);
        if (k.key == fourcc("SCAL") && ts > 0) {
            scale.clear();
            const size_t n = size_t(k.size) * size_t(k.repeat) / size_t(ts);
            for (size_t i = 0; i < n; ++i) scale.push_back(gpmfNumber(k, i));
        } else if (k.key == fourcc("IORI") && ts > 0 && k.size == 4 * ts) {
            // The turn the camera gave its picture to steady it (w, x, y, z): more than a degree, and the picture no
            // longer moves as the gyro says.
            for (int i = 0; i < k.repeat && !out.steadiedInCamera; ++i) {
                double q[4];
                for (int a = 0; a < 4; ++a) {
                    const double sc = scale.empty() ? 1.0 : scale[std::min(size_t(a), scale.size() - 1)];
                    q[a] = gpmfNumber(k, size_t(i) * 4 + size_t(a)) / (sc != 0 ? sc : 1.0);
                }
                const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (n > 0 && 2 * std::acos(std::min(1.0, std::fabs(q[0]) / n)) > M_PI / 180) out.steadiedInCamera = true;
            }
        } else if ((k.key == fourcc("GYRO") || k.key == fourcc("ACCL")) && ts > 0 && k.size == 3 * ts && k.repeat > 0) {
            const bool gyro = k.key == fourcc("GYRO");
            const double span = t1 - t0;
            for (int i = 0; i < k.repeat; ++i) {
                ImuSample s;
                s.t = t0 + span * i / k.repeat;
                std::array<float, 3> v{};
                for (int a = 0; a < 3; ++a) {
                    const double sc = scale.empty() ? 1.0 : scale[std::min(size_t(a), scale.size() - 1)];
                    v[size_t(a)] = float(gpmfNumber(k, size_t(i) * 3 + size_t(a)) / (sc != 0 ? sc : 1.0));
                }
                if (gyro) {
                    s.gyro = v;
                } else {
                    s.accelOnly = true;
                    s.accel = v;
                }
                out.samples.push_back(s);
            }
        }
    });
}

void readGpmf(const uint8_t* p, size_t size, double t0, double t1, MotionData& out, int depth) {
    if (depth > 4) return;
    walkKlv(p, size, [&](const Klv& k, size_t len) {
        if (k.type != 0) return;
        if (k.key == fourcc("STRM")) readGpmfStream(k.data, len, t0, t1, out);
        else readGpmf(k.data, len, t0, t1, out, depth + 1);  // DEVC
    });
}

double fileOrigin(const AVFormatContext* fmt) {
    return fmt->start_time == AV_NOPTS_VALUE ? 0.0 : double(fmt->start_time) / AV_TIME_BASE;
}

std::filesystem::path sidecar(const std::string& videoPath) {
    std::filesystem::path p = std::filesystem::u8path(videoPath);
    for (const char* ext : {".gcsv", ".GCSV"}) {
        std::filesystem::path s = p;
        s.replace_extension(ext);
        std::error_code ec;
        if (std::filesystem::is_regular_file(s, ec)) return s;
    }
    return {};
}

int motionStream(AVFormatContext* fmt, bool& gpmf) {
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const uint32_t tag = fmt->streams[i]->codecpar->codec_tag;
        if (tag == MKTAG('g', 'p', 'm', 'd') || tag == MKTAG('c', 'a', 'm', 'm')) {
            gpmf = tag == MKTAG('g', 'p', 'm', 'd');
            return int(i);
        }
    }
    return -1;
}

}  // namespace

double MotionData::rate() const {
    if (samples.size() < 2) return 0;
    const double span = samples.back().t - samples.front().t;
    return span > 0 ? double(samples.size() - 1) / span : 0;
}

bool parseGpmf(const uint8_t* data, size_t size, double t0, double t1, MotionData& out) {
    const size_t before = out.samples.size();
    readGpmf(data, size, t0, t1, out, 0);
    return out.samples.size() > before;
}

bool parseCamm(const uint8_t* data, size_t size, double t, MotionData& out) {
    if (size < 4) return false;
    auto f32 = [&](size_t at) {
        const uint32_t u = AV_RL32(data + at);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    };
    const int type = AV_RL16(data + 2);
    ImuSample s;
    s.t = t;
    switch (type) {
        case 1:  // exposure: pixel exposure time, rolling shutter skew time (nanoseconds)
            if (size < 12) return false;
            out.readout = int32_t(AV_RL32(data + 8)) / 1e9;
            return true;
        case 2:  // gyro, radians a second
            if (size < 16) return false;
            s.gyro = {f32(4), f32(8), f32(12)};
            out.samples.push_back(s);
            return true;
        case 3:  // acceleration, metres a second squared
            if (size < 16) return false;
            s.accelOnly = true;
            s.accel = {f32(4), f32(8), f32(12)};
            out.samples.push_back(s);
            return true;
        default: return true;  // (angle axis, position, GPS, magnetic field: not needed)
    }
}

bool parseGcsv(const std::string& text, MotionData& out, std::string* error) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("GYROFLOW IMU LOG", 0) != 0) {
        if (error) *error = "Not a .gcsv motion log";
        return false;
    }
    double tscale = 0.001, gscale = 1, ascale = 1;
    bool readoutDown = true;
    std::vector<std::string> columns;
    auto num = [](const std::string& s, double fallback) {
        try {
            return std::stod(s);
        } catch (...) {
            return fallback;
        }
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t comma = line.find(',');
        const std::string key = line.substr(0, comma), value = comma == std::string::npos ? "" : line.substr(comma + 1);
        if (key == "t") {
            std::stringstream cs(line);
            std::string c;
            while (std::getline(cs, c, ',')) columns.push_back(c);
            break;
        }
        if (key == "tscale") tscale = num(value, tscale);
        else if (key == "gscale") gscale = num(value, gscale);
        else if (key == "ascale") ascale = num(value, ascale);
        else if (key == "frame_readout_time") out.readout = num(value, -1000) / 1000;  // milliseconds
        else if (key == "frame_readout_direction") readoutDown = num(value, 0) == 0;    // 0: top to bottom
    }
    if (!readoutDown || !(out.readout >= 0)) out.readout = -1;  // (the rows read the other way: not used)
    auto col = [&](const char* name) {
        const auto it = std::find(columns.begin(), columns.end(), name);
        return it == columns.end() ? -1 : int(it - columns.begin());
    };
    const int ct = col("t"), gx = col("gx"), gy = col("gy"), gz = col("gz"), ax = col("ax"), ay = col("ay"), az = col("az");
    if (ct < 0 || gx < 0 || gy < 0 || gz < 0) {
        if (error) *error = "The log has no gyro columns";
        return false;
    }
    const size_t before = out.samples.size();
    std::vector<double> v;
    while (std::getline(in, line)) {
        v.clear();
        const char* p = line.c_str();
        while (*p) {
            char* end = nullptr;
            const double x = std::strtod(p, &end);
            v.push_back(end == p ? std::numeric_limits<double>::quiet_NaN() : x);
            p = end == p ? p + 1 : end;
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
        if (int(v.size()) <= std::max({ct, gx, gy, gz}) || std::isnan(v[size_t(ct)])) continue;
        ImuSample s;
        s.t = v[size_t(ct)] * tscale;
        s.gyro = {float(v[size_t(gx)] * gscale), float(v[size_t(gy)] * gscale), float(v[size_t(gz)] * gscale)};
        if (ax >= 0 && ay >= 0 && az >= 0 && int(v.size()) > std::max({ax, ay, az}))
            s.accel = {float(v[size_t(ax)] * ascale * kStandardGravity), float(v[size_t(ay)] * ascale * kStandardGravity),
                       float(v[size_t(az)] * ascale * kStandardGravity)};
        out.samples.push_back(s);
    }
    if (out.samples.size() == before) {
        if (error) *error = "The log has no readings";
        return false;
    }
    return true;
}

bool hasMotionData(const std::string& videoPath) {
    if (!sidecar(videoPath).empty()) return true;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, videoPath.c_str(), nullptr, nullptr) < 0) return false;
    bool gpmf = false;
    const bool found = motionStream(fmt, gpmf) >= 0;
    avformat_close_input(&fmt);
    return found;
}

bool readMotionData(const std::string& videoPath, MotionData& out, std::string* error) {
    out = MotionData{};
    AVFormatContext* fmt = nullptr;
    int stream = -1;
    bool gpmf = false;
    if (avformat_open_input(&fmt, videoPath.c_str(), nullptr, nullptr) >= 0) {
        stream = motionStream(fmt, gpmf);
        if (stream >= 0) {
            for (unsigned i = 0; i < fmt->nb_streams; ++i) fmt->streams[i]->discard = int(i) == stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
            const AVStream* st = fmt->streams[stream];
            const double tb = av_q2d(st->time_base), origin = fileOrigin(fmt);
            out.source = gpmf ? "GoPro GPMF" : "CAMM";
            AVPacket* pkt = av_packet_alloc();
            while (av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == stream && pkt->pts != AV_NOPTS_VALUE) {
                    const double t0 = double(pkt->pts) * tb - origin;
                    if (gpmf) parseGpmf(pkt->data, size_t(pkt->size), t0, t0 + double(std::max<int64_t>(pkt->duration, 1)) * tb, out);
                    else parseCamm(pkt->data, size_t(pkt->size), t0, out);
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        avformat_close_input(&fmt);
    }
    if (out.samples.empty()) {
        const std::filesystem::path s = sidecar(videoPath);
        if (!s.empty()) {
            std::ifstream f(s, std::ios::binary);
            std::stringstream text;
            text << f.rdbuf();
            out = MotionData{};
            out.source = ".gcsv log";
            if (!parseGcsv(text.str(), out, error)) return false;
        }
    }
    finish(out);
    if (out.samples.size() < 16) {
        if (error) *error = stream >= 0 ? "The clip's motion data track has no gyro readings" : "The clip has no camera motion data";
        return false;
    }
    return true;
}

// ---- Lining up -----------------------------------------------------------------------------------------------------
namespace {

// The gyro's running integral on an even grid, for integrals over any stretch.
struct Integral {
    double t0 = 0, dt = 0.001;
    std::array<std::vector<double>, 3> c;
    // Over [from, to] only (within the readings).
    Integral(const MotionData& d, double from, double to) {
        t0 = std::max(from, d.samples.front().t);
        const double t1 = std::max(t0, std::min(to, d.samples.back().t));
        const size_t n = size_t(std::ceil((t1 - t0) / dt)) + 1;
        for (auto& v : c) v.assign(n, 0.0);
        std::array<double, 3> sum{};
        size_t j = size_t(std::lower_bound(d.samples.begin(), d.samples.end(), t0, [](const ImuSample& s, double t) { return s.t < t; }) -
                          d.samples.begin());
        j = j > 0 ? j - 1 : 0;
        for (size_t i = 1; i < n; ++i) {
            const double a = t0 + double(i - 1) * dt, b = a + dt, m = (a + b) / 2;
            while (j + 1 < d.samples.size() && d.samples[j + 1].t <= m) ++j;
            const ImuSample& s0 = d.samples[j];
            const ImuSample& s1 = d.samples[std::min(j + 1, d.samples.size() - 1)];
            const double f = s1.t > s0.t ? std::clamp((m - s0.t) / (s1.t - s0.t), 0.0, 1.0) : 0.0;
            for (int k = 0; k < 3; ++k) {
                sum[size_t(k)] += (s0.gyro[size_t(k)] + (s1.gyro[size_t(k)] - s0.gyro[size_t(k)]) * f) * dt;
                c[size_t(k)][i] = sum[size_t(k)];
            }
        }
    }
    double at(int k, double t) const {
        const auto& v = c[size_t(k)];
        const double x = (t - t0) / dt;
        if (x <= 0) return v.front();
        const size_t i = size_t(x);
        if (i + 1 >= v.size()) return v.back();
        return v[i] + (v[i + 1] - v[i]) * (x - double(i));
    }
    double end() const { return t0 + dt * double(c[0].size() - 1); }
};

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const size_t n = a.size();
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) ma += a[i], mb += b[i];
    ma /= double(n), mb /= double(n);
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i) {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0.0;
}

struct Match {
    double score = -1;
    std::array<int, 3> axis{0, 1, 2};
    std::array<int, 3> sign{1, 1, 1};
    std::array<double, 3> corr{};
};

}  // namespace

bool syncMotionData(const MotionData& d, const CameraMotion& tracked, GyroSync& out, std::string* error, double searchSeconds,
                    const std::atomic<bool>* cancel) {
    out = GyroSync{};
    if (d.samples.size() < 16 || tracked.steps.size() < 16 || tracked.fps <= 0) {
        if (error) *error = "Too little to line the motion data up with";
        return false;
    }
    if (d.steadiedInCamera) {
        if (error) *error = "The camera stabilised the picture itself, so it no longer moves as its motion data says";
        return false;
    }
    const size_t n = tracked.steps.size() - 1;
    const Integral in(d, tracked.start - searchSeconds - 1, tracked.start + double(n + 1) / tracked.fps + searchSeconds + 1);
    // The picture's movement each frame: across (from the camera turning about y), down (about x) and its roll (z).
    std::array<std::vector<double>, 3> target;
    for (auto& v : target) v.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Similarity& s = tracked.steps[i + 1];
        target[0][i] = s.tx, target[1][i] = s.ty, target[2][i] = s.angle;
    }
    std::array<std::vector<double>, 3> g;
    for (auto& v : g) v.resize(n);
    auto match = [&](double offset) {
        for (size_t i = 0; i < n; ++i) {
            const double a = tracked.start + double(i) / tracked.fps + offset, b = a + 1 / tracked.fps;
            for (int k = 0; k < 3; ++k) g[size_t(k)][i] = in.at(k, b) - in.at(k, a);
        }
        double c[3][3];
        for (int t = 0; t < 3; ++t)
            for (int k = 0; k < 3; ++k) c[t][k] = correlation(target[size_t(t)], g[size_t(k)]);
        // Across follows the turn about y, down the turn about x, roll the turn about z: the best of the six ways the
        // channels could be assigned, with across and down counting most (roll is often slight).
        Match best;
        int perm[3] = {0, 1, 2};
        do {
            const double s = 0.375 * (std::fabs(c[0][perm[0]]) + std::fabs(c[1][perm[1]])) + 0.25 * std::fabs(c[2][perm[2]]);
            if (s > best.score) {
                best.score = s;
                // Camera x from the downward movement, y from across, z from roll (signs as the projection gives:
                // across = -f wy, down = f wx, roll = -wz).
                best.axis = {perm[1], perm[0], perm[2]};
                best.corr = {c[1][perm[1]], c[0][perm[0]], c[2][perm[2]]};
                best.sign = {c[1][perm[1]] >= 0 ? 1 : -1, c[0][perm[0]] >= 0 ? -1 : 1, c[2][perm[2]] >= 0 ? -1 : 1};
            }
        } while (std::next_permutation(perm, perm + 3));
        return best;
    };
    // Coarse, then fine.
    Match best;
    double bestOffset = 0;
    const double lo = std::max(-searchSeconds, in.t0 - tracked.start);
    const double hi = std::min(searchSeconds, in.end() - (tracked.start + double(n + 1) / tracked.fps));
    if (hi < lo) {
        if (error) *error = "The motion data does not cover the clip";
        return false;
    }
    for (double o = lo; o <= hi + 1e-9; o += 0.002) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        const Match m = match(o);
        if (m.score > best.score) best = m, bestOffset = o;
    }
    const double coarse = bestOffset;
    for (double o = coarse - 0.003; o <= coarse + 0.003 + 1e-9; o += 0.0001) {
        if (o < lo || o > hi) continue;
        const Match m = match(o);
        if (m.score > best.score) best = m, bestOffset = o;
    }
    // A mirror image is no rotation: the least sure axis gives way.
    const int parity = [&] {
        int inv = 0;
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b) inv += best.axis[size_t(a)] > best.axis[size_t(b)];
        return inv % 2 ? -1 : 1;
    }();
    if (parity * best.sign[0] * best.sign[1] * best.sign[2] < 0) {
        int weakest = 0;
        for (int a = 1; a < 3; ++a)
            if (std::fabs(best.corr[size_t(a)]) < std::fabs(best.corr[size_t(weakest)])) weakest = a;
        best.sign[size_t(weakest)] = -best.sign[size_t(weakest)];
    }
    out.offset = bestOffset;
    out.axis = best.axis;
    out.sign = best.sign;
    out.score = best.score;
    // The focal length: the movement across and down over the turn that made it.
    match(bestOffset);
    double num = 0, den = 0;
    for (size_t i = 0; i < n; ++i) {
        const double wx = out.sign[0] * g[size_t(out.axis[0])][i], wy = out.sign[1] * g[size_t(out.axis[1])][i];
        num += target[0][i] * -wy + target[1][i] * wx;
        den += wx * wx + wy * wy;
    }
    out.focal = den > 0 ? num / den : 0;
    if (out.score < 0.4) {
        if (error) *error = "The motion data does not match the picture's movement: the camera may have stabilised the picture itself, "
                          "barely moved, or the data is another clip's";
        return false;
    }
    if (!(out.focal > 0.25 && out.focal < 6)) {  // (wider than 127°: not a flat picture)
        if (error) *error = "Could not measure the lens from the motion data";
        return false;
    }
    return true;
}

// ---- The camera's path -----------------------------------------------------------------------------------------------
GyroPath gyroPath(const MotionData& d, const GyroSync& sync, double fps, double from, double to, int perFrame) {
    GyroPath p;
    p.source = d.source;
    p.fps = fps;
    p.start = from;
    p.perFrame = std::max(1, perFrame);
    p.focal = sync.focal;
    p.readout = sync.readout >= 0 ? sync.readout : d.readout;
    if (d.samples.size() < 2 || fps <= 0 || to <= from) return p;
    // In the camera's axes: x right, y down, z forward.
    auto camera = [&](const std::array<float, 3>& v) {
        return V{sync.sign[0] * double(v[size_t(sync.axis[0])]), sync.sign[1] * double(v[size_t(sync.axis[1])]),
                 sync.sign[2] * double(v[size_t(sync.axis[2])])};
    };
    // The orientation at each reading (camera to world), the turn between readings at their mean speed.
    const size_t m = d.samples.size();
    std::vector<Q> q(m);
    q[0] = {1, 0, 0, 0};
    for (size_t i = 1; i < m; ++i) {
        const double dt = d.samples[i].t - d.samples[i - 1].t;
        const V a = camera(d.samples[i - 1].gyro), b = camera(d.samples[i].gyro);
        q[i] = qnorm(qmul(q[i - 1], qexp({(a[0] + b[0]) / 2 * dt, (a[1] + b[1]) / 2 * dt, (a[2] + b[2]) / 2 * dt})));
    }
    // The way up in the world at each reading (the accelerometer feels the push against gravity), smoothed over a
    // second either side: the shakes go, the gyro's slow drift is followed.
    std::vector<V> up;
    if (d.hasAccel) {
        std::vector<V> raw(m);
        for (size_t i = 0; i < m; ++i) {
            const V a = camera(d.samples[i].accel);
            const double l = vlen(a);
            raw[i] = l > 1e-6 ? qrot(q[i], V{a[0] / l, a[1] / l, a[2] / l}) : V{0, 0, 0};
        }
        // A running mean over a window of about two seconds (twice, nearly Gaussian).
        const double rate = d.rate();
        const size_t half = size_t(std::max(1.0, rate * 0.5));
        auto boxed = [&](const std::vector<V>& v) {
            std::vector<V> out(v.size());
            std::vector<V> pre(v.size() + 1, V{0, 0, 0});
            for (size_t i = 0; i < v.size(); ++i)
                for (int k = 0; k < 3; ++k) pre[i + 1][size_t(k)] = pre[i][size_t(k)] + v[i][size_t(k)];
            for (size_t i = 0; i < v.size(); ++i) {
                const size_t a = i >= half ? i - half : 0, b = std::min(v.size(), i + half + 1);
                for (int k = 0; k < 3; ++k) out[i][size_t(k)] = (pre[b][size_t(k)] - pre[a][size_t(k)]) / double(b - a);
            }
            return out;
        };
        up = boxed(boxed(raw));
    }
    // Sampled at the picture's times (readings at the picture time plus the offset).
    const size_t count = size_t(std::floor((to - from) * fps * p.perFrame + 1e-6)) + 1;
    size_t j = 0;
    Q first{1, 0, 0, 0};
    for (size_t s = 0; s < count; ++s) {
        const double t = from + double(s) / (fps * p.perFrame) + sync.offset;
        while (j + 1 < m && d.samples[j + 1].t <= t) ++j;
        const size_t k = std::min(j + 1, m - 1);
        const double f = d.samples[k].t > d.samples[j].t ? std::clamp((t - d.samples[j].t) / (d.samples[k].t - d.samples[j].t), 0.0, 1.0) : 0.0;
        const Q at = qslerp(q[j], q[k], f);
        if (s == 0) first = qconj(at);
        GyroPath::Sample out;
        out.q = qnorm(qmul(first, at));
        if (!up.empty()) {
            V u{up[j][0] + (up[k][0] - up[j][0]) * f, up[j][1] + (up[k][1] - up[j][1]) * f, up[j][2] + (up[k][2] - up[j][2]) * f};
            const V c = qrot(qconj(at), u);
            const double l = vlen(c);
            if (l > 1e-6) out.up = {c[0] / l, c[1] / l, c[2] / l};
        }
        p.samples.push_back(out);
    }
    return p;
}

// Text: a header, then a line a sample: the turn since the last sample (micro-radians, camera axes) and the way up
// (ten-thousandths).
std::string gyroPathToString(const GyroPath& p) {
    std::ostringstream o;
    o.precision(10);
    std::string source = p.source;
    std::replace(source.begin(), source.end(), '\n', ' ');
    o << "gyro 1\n" << source << "\n" << p.fps << ' ' << p.start << ' ' << p.perFrame << ' ' << p.focal << ' ' << p.readout << ' '
      << p.samples.size() << '\n';
    Q prev{1, 0, 0, 0};
    V carried{0, 0, 0};  // what rounding has not passed on yet
    for (const GyroPath::Sample& s : p.samples) {
        V r = qlog(qmul(qconj(prev), s.q));
        long ri[3];
        for (int k = 0; k < 3; ++k) {
            r[size_t(k)] += carried[size_t(k)];
            ri[k] = std::lround(r[size_t(k)] * 1e6);
            carried[size_t(k)] = r[size_t(k)] - double(ri[k]) / 1e6;
        }
        prev = qnorm(qmul(prev, qexp({double(ri[0]) / 1e6, double(ri[1]) / 1e6, double(ri[2]) / 1e6})));
        o << ri[0] << ' ' << ri[1] << ' ' << ri[2] << ' ' << std::lround(s.up[0] * 1e4) << ' ' << std::lround(s.up[1] * 1e4) << ' '
          << std::lround(s.up[2] * 1e4) << '\n';
    }
    return o.str();
}

bool gyroPathFromString(const std::string& s, GyroPath& p) {
    p = GyroPath{};
    std::istringstream in(s);
    std::string line;
    if (!std::getline(in, line) || line != "gyro 1") return false;
    if (!std::getline(in, p.source)) return false;
    size_t n = 0;
    if (!(in >> p.fps >> p.start >> p.perFrame >> p.focal >> p.readout >> n) || p.fps <= 0 || p.perFrame < 1 || n > 100000000) return false;
    Q q{1, 0, 0, 0};
    p.samples.reserve(std::min(n, s.size() / 12));  // (each takes a line of at least 12 characters)
    for (size_t i = 0; i < n; ++i) {
        long r[3], u[3];
        if (!(in >> r[0] >> r[1] >> r[2] >> u[0] >> u[1] >> u[2])) return false;
        q = qnorm(qmul(q, qexp({double(r[0]) / 1e6, double(r[1]) / 1e6, double(r[2]) / 1e6})));
        GyroPath::Sample sm;
        sm.q = q;
        sm.up = {double(u[0]) / 1e4, double(u[1]) / 1e4, double(u[2]) / 1e4};
        p.samples.push_back(sm);
    }
    return !p.samples.empty();
}

namespace {
// The orientation and way up at picture time t (seconds from the path's start).
void pathAt(const GyroPath& p, double t, Q& q, V& up) {
    const double x = std::clamp(t * p.fps * p.perFrame, 0.0, double(p.samples.size() - 1));
    const size_t i = std::min(size_t(x), p.samples.size() - 1), k = std::min(i + 1, p.samples.size() - 1);
    const double f = x - double(i);
    q = qslerp(p.samples[i].q, p.samples[k].q, f);
    const auto& a = p.samples[i].up;
    const auto& b = p.samples[k].up;
    up = {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f};
}
}  // namespace

bool refineGyroLens(const std::string& videoPath, const MotionData& d, GyroSync& sync, double fps, double from, double to,
                    std::string* error, const std::atomic<bool>* cancel) {
    if (fps <= 0 || to - from < 4 / fps || sync.focal <= 0) {
        if (error) *error = "Too short to measure the lens";
        return false;
    }
    // The turn at any time, finely sampled.
    const GyroPath path = gyroPath(d, sync, fps, from, to, 8);
    if (path.samples.size() < 16) return false;
    auto orientation = [&](double t) {
        Q q;
        V up;
        pathAt(path, t - from, q, up);
        return q;
    };
    // The pairs of frames that turned furthest, one from each twelfth of the stretch.
    const int frames = int(std::floor((to - from) * fps)) - 1;
    const int parts = std::min(12, frames);
    std::vector<int> picks;
    for (int s = 0; s < parts; ++s) {
        int best = -1;
        double most = 0;
        for (int i = frames * s / parts; i < frames * (s + 1) / parts; ++i) {
            const double a = vlen(qlog(qmul(qconj(orientation(from + i / fps)), orientation(from + (i + 1) / fps))));
            if (a > most) most = a, best = i;
        }
        if (best >= 0 && most > 0.002) picks.push_back(best);
    }
    VideoDecoder dec;
    if (picks.empty() || !dec.open(videoPath, error)) {
        if (error && error->empty()) *error = "The camera hardly turned";
        return false;
    }
    const int aw = std::min(640, dec.displayWidth());
    const int ah = std::max(2, int(std::lround(double(aw) * dec.displayHeight() / std::max(1, dec.displayWidth()))));
    struct Pair {
        int frame;
        std::vector<Point2> a, b;
    };
    std::vector<Pair> pairs;
    size_t points = 0;
    for (int i : picks) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        const Frame16Ptr fa = dec.frameAt(from + i / fps + 1e-4, aw, ah), fb = dec.frameAt(from + (i + 1) / fps + 1e-4, aw, ah);
        if (!fa || !fb) continue;
        const GrayImage ga = toGray(*fa), gb = toGray(*fb);
        Pair pr;
        pr.frame = i;
        const std::vector<Point2> pts = goodFeatures(ga, 300, aw / 40.0);
        std::vector<Point2> moved;
        std::vector<bool> ok;
        trackPoints(ga, gb, pts, moved, ok);
        for (size_t k = 0; k < pts.size(); ++k)
            if (ok[k]) pr.a.push_back(pts[k]), pr.b.push_back(moved[k]);
        points += pr.a.size();
        if (pr.a.size() >= 8) pairs.push_back(std::move(pr));
    }
    if (pairs.size() < 3 || points < 100) {
        if (error) *error = "Too few points to follow to measure the lens";
        return false;
    }
    // Each row's orientation, with the data's readout (when it gives none, as for a global shutter: the readout
    // hardly shows between neighbouring frames, so it cannot be measured here).
    const double knownReadout = sync.readout >= 0 ? sync.readout : d.readout;
    const std::vector<double> readouts{knownReadout > 0 ? std::min(1.0, knownReadout * fps) : 0.0};
    const double W = aw, H = ah;
    double bestCost = std::numeric_limits<double>::max(), bestFocal = sync.focal;
    for (double readout : readouts) {
        std::vector<std::vector<M>> rowsA(pairs.size()), rowsB(pairs.size());
        for (size_t k = 0; k < pairs.size(); ++k) {
            rowsA[k].resize(size_t(ah));
            rowsB[k].resize(size_t(ah));
            for (int r = 0; r < ah; ++r) {
                const double dt = ((r + 0.5) / H - 0.5) * readout / fps;
                rowsA[k][size_t(r)] = qmat(orientation(from + pairs[k].frame / fps + dt));
                rowsB[k][size_t(r)] = qmat(orientation(from + (pairs[k].frame + 1) / fps + dt));
            }
        }
        for (double scale = 0.6; scale <= 1.4 + 1e-9; scale += 0.005) {
            const double f = sync.focal * scale * W;
            double cost = 0;
            for (size_t k = 0; k < pairs.size(); ++k)
                for (size_t n = 0; n < pairs[k].a.size(); ++n) {
                    const Point2& pa = pairs[k].a[n];
                    const Point2& pb = pairs[k].b[n];
                    const M& ra = rowsA[k][size_t(std::clamp(int(pa.y), 0, ah - 1))];
                    const M& rb = rowsB[k][size_t(std::clamp(int(pb.y), 0, ah - 1))];
                    const V world = mv(ra, V{pa.x + 0.5 - W / 2, pa.y + 0.5 - H / 2, f});
                    const V cb{rb[0] * world[0] + rb[3] * world[1] + rb[6] * world[2], rb[1] * world[0] + rb[4] * world[1] + rb[7] * world[2],
                               rb[2] * world[0] + rb[5] * world[1] + rb[8] * world[2]};  // (the transpose: world to camera)
                    if (cb[2] <= 1e-9) {
                        cost += 9;
                        continue;
                    }
                    const double ex = f * cb[0] / cb[2] + W / 2 - 0.5 - pb.x, ey = f * cb[1] / cb[2] + H / 2 - 0.5 - pb.y;
                    cost += std::min(ex * ex + ey * ey, 9.0);
                }
            if (cost < bestCost) bestCost = cost, bestFocal = sync.focal * scale;
        }
    }
    sync.focal = bestFocal;
    return true;
}

bool gyroPathSummary(const std::string& s, std::string& source, double& focal) {
    std::istringstream in(s);
    std::string line;
    if (!std::getline(in, line) || line != "gyro 1" || !std::getline(in, source)) return false;
    double fps = 0, start = 0;
    int perFrame = 0;
    return bool(in >> fps >> start >> perFrame >> focal) && fps > 0 && focal > 0;
}

// ---- Stabilising -------------------------------------------------------------------------------------------------------

std::vector<GyroCorrection> gyroCorrections(const GyroPath& p, const GyroStabilizeSettings& s) {
    std::vector<GyroCorrection> out;
    if (p.samples.empty() || p.fps <= 0) return out;
    const size_t frames = (p.samples.size() - 1) / size_t(p.perFrame) + 1;
    std::vector<Q> r(frames);
    std::vector<V> upWorld(frames);
    std::vector<V> spin(frames);
    const double h = 0.25 / p.fps;  // (a quarter frame either side for the speed)
    for (size_t i = 0; i < frames; ++i) {
        const double t = double(i) / p.fps + s.offset;
        V up;
        pathAt(p, t, r[i], up);
        upWorld[i] = qrot(r[i], up);
        Q a, b;
        V ignored;
        pathAt(p, t - h, a, ignored);
        pathAt(p, t + h, b, ignored);
        const V w = qlog(qmul(qconj(a), b));  // in the camera's axes, over 2h
        spin[i] = {w[0] / (2 * h * p.fps), w[1] / (2 * h * p.fps), w[2] / (2 * h * p.fps)};
    }
    // The smoothed orientation: a near-Gaussian mean of the frames around (three running means, so long clips and
    // wide smoothing cost no more; the quaternions kept on one side, as their signs are free), 0 seconds: the first
    // frame's.
    const double sigma = s.smoothSeconds * p.fps / 2;
    using Row = std::array<double, 7>;  // the orientation (4) and the way up in the world (3)
    std::vector<Row> path(frames);
    for (size_t i = 0; i < frames; ++i) {
        Q q = r[i];
        if (i > 0 && qdot(q, Q{path[i - 1][0], path[i - 1][1], path[i - 1][2], path[i - 1][3]}) < 0)
            for (double& v : q) v = -v;
        path[i] = {q[0], q[1], q[2], q[3], upWorld[i][0], upWorld[i][1], upWorld[i][2]};
    }
    if (sigma > 0.01) {
        // Three boxes of this half-width make a Gaussian of the same spread; beyond the ends, the end frames repeated.
        const long half = std::max(1L, std::lround((std::sqrt(4 * sigma * sigma + 1) - 1) / 2));
        const long last = long(frames) - 1;
        std::vector<Row> pre(frames + 1), next(frames);
        for (int pass = 0; pass < 3; ++pass) {
            pre[0] = Row{};
            for (size_t i = 0; i < frames; ++i)
                for (size_t c = 0; c < 7; ++c) pre[i + 1][c] = pre[i][c] + path[i][c];
            for (long i = 0; i <= last; ++i) {
                const long a = i - half, b = i + half;
                for (size_t c = 0; c < 7; ++c) {
                    double sum = pre[size_t(std::min(b, last) + 1)][c] - pre[size_t(std::max(a, 0L))][c];
                    if (a < 0) sum += double(-a) * path[0][c];
                    if (b > last) sum += double(b - last) * path[size_t(last)][c];
                    next[size_t(i)][c] = sum / double(2 * half + 1);
                }
            }
            path.swap(next);
        }
    } else {
        for (size_t i = 0; i < frames; ++i)
            for (size_t c = 0; c < 4; ++c) path[i][c] = path[0][c];  // (held; the way up still each frame's)
    }
    out.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        Q smooth = qnorm(Q{path[i][0], path[i][1], path[i][2], path[i][3]});
        const V upw{path[i][4], path[i][5], path[i][6]};
        // The horizon: turned about the view's axis until up is straight up in the picture (less when looking
        // nearly straight up or down, where there is no horizon to level).
        if (s.horizon > 0 && vlen(upw) > 1e-6) {
            const V u = qrot(qconj(smooth), upw);
            const double across = std::hypot(u[0], u[1]), l = vlen(u);
            const double weight = std::clamp((across / l - 0.2) / 0.3, 0.0, 1.0);
            if (across > 1e-9) {
                const double phi = std::atan2(u[0], -u[1]) * s.horizon * weight;
                smooth = qnorm(qmul(smooth, qexp({0, 0, phi})));
            }
        }
        GyroCorrection& c = out[i];
        c.r = qmat(qmul(qconj(r[i]), smooth));
        c.spin = spin[i];
    }
    return out;
}

bool gyroSourcePoint(const GyroCorrection& c, double focal, double readout, double aspect, double x, double y, double& sx,
                     double& sy) {
    const V cam = mv(c.r, V{x / focal, y / focal, 1});
    if (cam[2] <= 1e-6) return false;
    sx = focal * cam[0] / cam[2];
    sy = focal * cam[1] / cam[2];
    if (readout > 0) {
        // That row was read `dt` frames from the middle one: the camera had turned on by its speed times dt.
        for (int it = 0; it < 2; ++it) {
            const double dt = sy / aspect * readout;
            const V a{c.spin[0] * dt, c.spin[1] * dt, c.spin[2] * dt};
            const Q turn = qexp(a);
            const V moved = qrot(qconj(turn), cam);
            if (moved[2] <= 1e-6) return false;
            sx = focal * moved[0] / moved[2];
            sy = focal * moved[1] / moved[2];
        }
    }
    return true;
}

std::array<double, 9> gyroTurnAt(const GyroCorrection& c, double dt) {
    return qmat(qconj(qexp({c.spin[0] * dt, c.spin[1] * dt, c.spin[2] * dt})));
}

double gyroZoom(const std::vector<GyroCorrection>& cs, double focal, double readout, double aspect) {
    double zoom = 1;
    const double hw = 0.5, hh = aspect / 2;
    const double pts[8][2] = {{-hw, -hh}, {0, -hh}, {hw, -hh}, {hw, 0}, {hw, hh}, {0, hh}, {-hw, hh}, {-hw, 0}};
    auto covered = [&](const GyroCorrection& c, double z) {
        for (const auto& pt : pts) {
            double sx, sy;
            if (!gyroSourcePoint(c, focal, readout, aspect, pt[0] / z, pt[1] / z, sx, sy)) return false;
            if (std::fabs(sx) > hw + 1e-9 || std::fabs(sy) > hh + 1e-9) return false;
        }
        return true;
    };
    constexpr double kMax = 4;
    for (const GyroCorrection& c : cs) {
        if (covered(c, zoom)) continue;
        double lo = zoom, hi = kMax;
        if (!covered(c, hi)) return kMax;
        for (int it = 0; it < 30; ++it) {
            const double mid = (lo + hi) / 2;
            (covered(c, mid) ? hi : lo) = mid;
        }
        zoom = hi;
    }
    return zoom;
}

}  // namespace montage
