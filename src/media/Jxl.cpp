// Montage — JPEG XL through libjxl (see Jxl.h).
#include "Jxl.h"

#include "media/Image.h"
#include "render/ColorSpace.h"

#include <QByteArray>
#include <QColorSpace>
#include <QFile>
#include <QSaveFile>
#include <QtCore/qfloat16.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <numeric>

#ifdef MONTAGE_WITH_JXL
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/version.h>
#ifdef MONTAGE_WITH_LCMS2
#include <lcms2.h>
#endif
#endif

namespace montage {

namespace {
// The frames' durations in ticks: as stated, or a tick each when no frame states one.
std::vector<uint32_t> ticksOf(const std::vector<uint32_t>& durations) {
    if (std::any_of(durations.begin(), durations.end(), [](uint32_t d) { return d != 0; })) return durations;
    return std::vector<uint32_t>(durations.size(), 1);
}
}  // namespace

double JxlInfo::seconds() const {
    if (!animated || tpsNum == 0) return 0;
    uint64_t ticks = 0;
    for (uint32_t d : ticksOf(durations)) ticks += d;
    return double(ticks) * double(tpsDen) / double(tpsNum);
}

double JxlInfo::frameStart(int k) const {
    if (!animated || tpsNum == 0 || k <= 0) return 0;
    const std::vector<uint32_t> d = ticksOf(durations);
    uint64_t ticks = 0;
    for (int i = 0; i < k && i < int(d.size()); ++i) ticks += d[size_t(i)];
    return double(ticks) * double(tpsDen) / double(tpsNum);
}

int JxlInfo::frameAt(double t) const {
    if (!animated || durations.empty() || tpsNum == 0) return 0;
    const double ticks = std::max(0.0, t) * double(tpsNum) / double(tpsDen);
    const std::vector<uint32_t> d = ticksOf(durations);
    double end = 0;
    for (size_t i = 0; i < d.size(); ++i) {
        end += d[i];
        if (ticks < end - 1e-6) return int(i);
    }
    return int(durations.size()) - 1;
}

bool isJxlPath(const std::string& file) {
    const size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || file.size() - dot != 4) return false;
    std::string ext = file.substr(dot);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".jxl";
}

#ifndef MONTAGE_WITH_JXL

namespace {
bool unbuilt(std::string* error) {
    if (error) *error = "This build of Montage has no JPEG XL (libjxl)";
    return false;
}
}  // namespace

bool jxlAvailable() { return false; }
bool jxlWritesPremultiplied() { return false; }
bool jxlConvertsProfiles() { return false; }
bool readJxlInfo(const std::string&, JxlInfo&, std::string* error) { return unbuilt(error); }
bool readJxlFrame(const std::string&, int, int&, int&, std::vector<float>&, bool*, std::string* error) { return unbuilt(error); }
struct JxlAnimationReader::State {
    JxlInfo info;
};
JxlAnimationReader::JxlAnimationReader() : s_(std::make_unique<State>()) {}
JxlAnimationReader::~JxlAnimationReader() = default;
bool JxlAnimationReader::open(const std::string&, std::string* error) { return unbuilt(error); }
const JxlInfo& JxlAnimationReader::info() const { return s_->info; }
bool JxlAnimationReader::frame(int, int&, int&, std::vector<float>&, std::string* error) { return unbuilt(error); }
bool jxlCanState(const ColorSpace&) { return false; }
bool encodeJxl(std::vector<uint8_t>&, int, int, const std::vector<const float*>&, const JxlWriteOptions&, std::string* error,
               const std::vector<uint32_t>&, uint32_t, uint32_t) {
    return unbuilt(error);
}
bool writeJxl(const std::string&, int, int, const std::vector<const float*>&, const JxlWriteOptions&, std::string* error,
              const std::vector<uint32_t>&, uint32_t, uint32_t) {
    return unbuilt(error);
}
bool writeJxl(const std::string&, int, int, const float*, const JxlWriteOptions&, std::string* error) { return unbuilt(error); }
bool jpegToJxl(const std::string&, const std::string&, std::string* error, int) { return unbuilt(error); }
bool jxlToJpeg(const std::string&, std::vector<uint8_t>&, std::string* error) { return unbuilt(error); }

#else

bool jxlAvailable() { return true; }

// (libjxl 0.7 aborts on premultiplied alpha given as its extra channel, and drops it given in the header alone)
bool jxlWritesPremultiplied() { return JPEGXL_NUMERIC_VERSION >= JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0); }

bool jxlConvertsProfiles() {
#ifdef MONTAGE_WITH_LCMS2
    return true;
#else
    return false;
#endif
}

namespace {

// (libjxl 0.9 dropped the pixel format these took)
JxlDecoderStatus encodedProfile(const JxlDecoder* dec, JxlColorProfileTarget target, JxlColorEncoding* out) {
#if JPEGXL_NUMERIC_VERSION >= JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
    return JxlDecoderGetColorAsEncodedProfile(dec, target, out);
#else
    return JxlDecoderGetColorAsEncodedProfile(dec, nullptr, target, out);
#endif
}
bool iccProfile(const JxlDecoder* dec, JxlColorProfileTarget target, QByteArray& out) {
    size_t size = 0;
#if JPEGXL_NUMERIC_VERSION >= JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
    if (JxlDecoderGetICCProfileSize(dec, target, &size) != JXL_DEC_SUCCESS || size == 0) return false;
    out.resize(qsizetype(size));
    return JxlDecoderGetColorAsICCProfile(dec, target, reinterpret_cast<uint8_t*>(out.data()), size) == JXL_DEC_SUCCESS;
#else
    if (JxlDecoderGetICCProfileSize(dec, nullptr, target, &size) != JXL_DEC_SUCCESS || size == 0) return false;
    out.resize(qsizetype(size));
    return JxlDecoderGetColorAsICCProfile(dec, nullptr, target, reinterpret_cast<uint8_t*>(out.data()), size) == JXL_DEC_SUCCESS;
#endif
}

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

bool readAll(const std::string& file, std::vector<uint8_t>& out, std::string* error) {
    QFile f(QString::fromStdString(file));
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Cannot read " + file);
    const QByteArray b = f.readAll();
    out.assign(reinterpret_cast<const uint8_t*>(b.constData()), reinterpret_cast<const uint8_t*>(b.constData()) + b.size());
    return true;
}

// libjxl's thread pools, made once and lent to one decoder or encoder at a time (a pool runs one job at a time), so
// a run of frames does not start and stop threads for each.
class RunnerPool {
public:
    void* take() {
        {
            std::lock_guard lock(m_);
            if (!idle_.empty()) {
                void* r = idle_.back();
                idle_.pop_back();
                return r;
            }
        }
        return JxlThreadParallelRunnerCreate(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    }
    void give(void* r) {
        {
            std::lock_guard lock(m_);
            if (idle_.size() < 4) {
                idle_.push_back(r);
                return;
            }
        }
        JxlThreadParallelRunnerDestroy(r);
    }

private:
    std::mutex m_;
    std::vector<void*> idle_;
};
RunnerPool& runnerPool() {
    static RunnerPool* pool = new RunnerPool;  // (kept to the end: its threads are not joined while the program exits)
    return *pool;
}

// Threads for decoding or encoding pixels (taken when first wanted: header-only passes need none).
struct Runner {
    void* r = nullptr;
    Runner() = default;
    bool make() {
        if (!r) r = runnerPool().take();
        return r != nullptr;
    }
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    ~Runner() {
        if (r) runnerPool().give(r);
    }
};

// The most pixels a picture may have (as the other readers), and the most memory decoding one may take: libjxl's
// planes, the samples it gives and the RGBA made from them.
constexpr uint64_t kMaxPixels = uint64_t(1) << 26;
constexpr uint64_t kMaxDecodeBytes = uint64_t(2) << 30;

struct DecoderDeleter {
    void operator()(JxlDecoder* d) const { JxlDecoderDestroy(d); }
};
struct EncoderDeleter {
    void operator()(JxlEncoder* e) const { JxlEncoderDestroy(e); }
};

// ---- Colour ---------------------------------------------------------------------------------------------------------

constexpr Primaries kPrimaries[] = {Primaries::Bt709,           Primaries::Bt2020,          Primaries::P3D65,
                                    Primaries::SGamut3Cine,     Primaries::AlexaWideGamut3, Primaries::AlexaWideGamut4,
                                    Primaries::VGamut,          Primaries::CinemaGamut,     Primaries::Ap1,
                                    Primaries::Ap0,             Primaries::SGamut3,         Primaries::DGamut,
                                    Primaries::RedWideGamut,    Primaries::BmdWideGamut,    Primaries::DavinciWideGamut,
                                    Primaries::AppleWideGamut,  Primaries::AdobeRgb};

bool d65(const double* xy) { return std::fabs(xy[0] - 0.3127) < 2e-4 && std::fabs(xy[1] - 0.3290) < 2e-4; }

bool encodingFor(const ColorSpace& s, JxlColorEncoding& e) {
    std::memset(&e, 0, sizeof e);
    e.color_space = JXL_COLOR_SPACE_RGB;
    e.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
    switch (s.transfer) {
        case Transfer::Srgb: e.transfer_function = JXL_TRANSFER_FUNCTION_SRGB; break;
        case Transfer::Bt1886: e.transfer_function = JXL_TRANSFER_FUNCTION_709; break;
        case Transfer::Linear: e.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR; break;
        case Transfer::Pq: e.transfer_function = JXL_TRANSFER_FUNCTION_PQ; break;
        case Transfer::Hlg: e.transfer_function = JXL_TRANSFER_FUNCTION_HLG; break;
        case Transfer::Gamma22:
            e.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
            e.gamma = 256.0 / 563.0;  // (the encoding's exponent, as libjxl takes it: 1 / Adobe RGB's 563/256)
            break;
        default: return false;
    }
    if (s.primaries == Primaries::Bt709 || s.primaries == Primaries::Bt2020 || s.primaries == Primaries::P3D65) {
        e.primaries = s.primaries == Primaries::Bt709    ? JXL_PRIMARIES_SRGB
                      : s.primaries == Primaries::Bt2020 ? JXL_PRIMARIES_2100
                                                         : JXL_PRIMARIES_P3;
        e.white_point = JXL_WHITE_POINT_D65;
        return true;
    }
    double xy[8];
    if (!primariesChromaticities(s.primaries, xy)) return false;
    // (libjxl takes primaries inside the spectrum locus's square only: not ACES AP0's or the camera gamuts' imaginary
    // blues below y = 0)
    for (int i = 0; i < 8; ++i)
        if (xy[i] <= 0 || xy[i] >= 1) return false;
    e.primaries = JXL_PRIMARIES_CUSTOM;
    e.primaries_red_xy[0] = xy[0], e.primaries_red_xy[1] = xy[1];
    e.primaries_green_xy[0] = xy[2], e.primaries_green_xy[1] = xy[3];
    e.primaries_blue_xy[0] = xy[4], e.primaries_blue_xy[1] = xy[5];
    if (d65(xy + 6)) {
        e.white_point = JXL_WHITE_POINT_D65;
    } else {
        e.white_point = JXL_WHITE_POINT_CUSTOM;
        e.white_point_xy[0] = xy[6], e.white_point_xy[1] = xy[7];
    }
    return true;
}

// The Montage space a colour encoding is (by its primaries, white and curve), or "".
std::string spaceFor(const JxlColorEncoding& e) {
    Transfer t;
    switch (e.transfer_function) {
        case JXL_TRANSFER_FUNCTION_SRGB: t = Transfer::Srgb; break;
        case JXL_TRANSFER_FUNCTION_709: t = Transfer::Bt1886; break;
        case JXL_TRANSFER_FUNCTION_LINEAR: t = Transfer::Linear; break;
        case JXL_TRANSFER_FUNCTION_PQ: t = Transfer::Pq; break;
        case JXL_TRANSFER_FUNCTION_HLG: t = Transfer::Hlg; break;
        case JXL_TRANSFER_FUNCTION_GAMMA:
            if (std::fabs(1.0 / e.gamma - 563.0 / 256.0) > 0.02) return "";
            t = Transfer::Gamma22;
            break;
        default: return "";
    }
    Primaries p = Primaries::Bt709;
    if (e.color_space == JXL_COLOR_SPACE_GRAY) {
        // (grey: its curve with Rec.709's primaries, which neutral samples do not depend on)
    } else if (e.color_space != JXL_COLOR_SPACE_RGB) {
        return "";
    } else {
        double white[2] = {0.3127, 0.3290};
        if (e.white_point == JXL_WHITE_POINT_CUSTOM) white[0] = e.white_point_xy[0], white[1] = e.white_point_xy[1];
        else if (e.white_point != JXL_WHITE_POINT_D65) return "";
        if (e.primaries == JXL_PRIMARIES_SRGB || e.primaries == JXL_PRIMARIES_2100 || e.primaries == JXL_PRIMARIES_P3) {
            if (!d65(white)) return "";
            p = e.primaries == JXL_PRIMARIES_SRGB ? Primaries::Bt709 : e.primaries == JXL_PRIMARIES_2100 ? Primaries::Bt2020 : Primaries::P3D65;
        } else if (e.primaries == JXL_PRIMARIES_CUSTOM) {
            const double have[8] = {e.primaries_red_xy[0],  e.primaries_red_xy[1],  e.primaries_green_xy[0], e.primaries_green_xy[1],
                                    e.primaries_blue_xy[0], e.primaries_blue_xy[1], white[0],               white[1]};
            bool found = false;
            for (Primaries q : kPrimaries) {
                double xy[8];
                if (!primariesChromaticities(q, xy)) continue;
                bool same = true;
                for (int i = 0; i < 8 && same; ++i) same = std::fabs(xy[i] - have[i]) < 1e-3;
                if (same) {
                    p = q, found = true;
                    break;
                }
            }
            if (!found) return "";
        } else {
            return "";
        }
    }
    if (t == Transfer::Linear) return linearColorSpace(p).id;
    for (const ColorSpace& c : colorSpaces())
        if (c.primaries == p && c.transfer == t && !c.sceneReferred && c.id.find('+') == std::string::npos) return c.id;
    return "";
}

// An ICC profile's space, where it is plainly one of Montage's.
std::string spaceForIcc(const QByteArray& icc) {
    const QColorSpace cs = QColorSpace::fromIccProfile(icc);
    if (!cs.isValid()) return "";
    const auto tf = cs.transferFunction();
    switch (cs.primaries()) {
        case QColorSpace::Primaries::SRgb:
            return tf == QColorSpace::TransferFunction::SRgb ? "srgb" : tf == QColorSpace::TransferFunction::Linear ? "linear-rec709" : "";
        case QColorSpace::Primaries::DciP3D65:
            return tf == QColorSpace::TransferFunction::SRgb     ? "p3d65"
                   : tf == QColorSpace::TransferFunction::Linear ? linearColorSpace(Primaries::P3D65).id
                                                                 : "";
        case QColorSpace::Primaries::AdobeRgb:
            return tf == QColorSpace::TransferFunction::Gamma && std::fabs(cs.gamma() - 563.0 / 256.0) < 0.02 ? "adobergb" : "";
        default: return "";
    }
}

#ifdef MONTAGE_WITH_LCMS2
struct TransformDeleter {
    void operator()(void* t) const { cmsDeleteTransform(t); }
};
using Transform = std::unique_ptr<void, TransformDeleter>;

// A conversion of `channels`-sample pixels (grey, grey and alpha, or RGBA, as libjxl gives them) from an ICC profile's
// colours to linear Rec.709 display light, in float and unbounded (colours past Rec.709's gamut kept, below 0 or past
// 1), alpha copied; null with `why` for a profile Little CMS cannot read or of another colour model (CMYK).
Transform iccToDisplayLight(const QByteArray& icc, int channels, std::string& why) {
    cmsHPROFILE in = cmsOpenProfileFromMem(icc.constData(), cmsUInt32Number(icc.size()));
    if (!in) return why = "The JPEG XL file's colour profile cannot be read", nullptr;
    const bool gray = channels <= 2;
    if (cmsGetColorSpace(in) != (gray ? cmsSigGrayData : cmsSigRgbData)) {
        cmsCloseProfile(in);
        return why = "The JPEG XL file's colours are not RGB or grey (CMYK is not read)", nullptr;
    }
    const cmsCIExyY d65{0.3127, 0.3290, 1};
    const cmsCIExyYTRIPLE rec709{{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}};
    cmsToneCurve* linear = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve* curves[3] = {linear, linear, linear};
    cmsHPROFILE out = linear ? cmsCreateRGBProfile(&d65, &rec709, curves) : nullptr;
    if (linear) cmsFreeToneCurve(linear);
    const cmsUInt32Number from = channels == 1 ? TYPE_GRAY_FLT : channels == 2 ? TYPE_GRAYA_FLT : TYPE_RGBA_FLT;
    // (no cache: shared by the threads converting rows; a grey picture without alpha has its alpha filled in)
    const cmsUInt32Number flags = cmsFLAGS_NOCACHE | (channels == 1 ? 0 : cmsFLAGS_COPY_ALPHA);
    cmsHTRANSFORM t = out ? cmsCreateTransform(in, from, out, TYPE_RGBA_FLT, INTENT_RELATIVE_COLORIMETRIC, flags) : nullptr;
    cmsCloseProfile(in);
    if (out) cmsCloseProfile(out);
    if (!t) why = "The JPEG XL file's colour profile cannot be converted";
    return Transform(t);
}
#endif

// Whether a JPEG XL container holds a JPEG's reconstruction data (a jbrd box).
bool hasJpegBox(const std::vector<uint8_t>& d) {
    static const uint8_t sig[12] = {0, 0, 0, 0x0c, 'J', 'X', 'L', ' ', 0x0d, 0x0a, 0x87, 0x0a};
    if (d.size() < 12 || std::memcmp(d.data(), sig, 12) != 0) return false;
    size_t at = 0;
    while (at + 8 <= d.size()) {
        uint64_t size = uint64_t(d[at]) << 24 | uint64_t(d[at + 1]) << 16 | uint64_t(d[at + 2]) << 8 | d[at + 3];
        size_t header = 8;
        if (size == 1) {
            if (at + 16 > d.size()) return false;
            size = 0;
            for (int i = 0; i < 8; ++i) size = size << 8 | d[at + 8 + size_t(i)];
            header = 16;
        } else if (size == 0) {
            size = d.size() - at;
        }
        if (std::memcmp(&d[at + 4], "jbrd", 4) == 0) return true;
        if (size < header || size > d.size() - at) return false;
        at += size_t(size);
    }
    return false;
}

// A decoding of a whole file in memory: its header read into `info`, frames decoded on request in order.
struct Session {
    Runner runner;  // (declared before the decoder, which uses it, so it outlives it)
    std::vector<uint8_t> data;
    std::unique_ptr<JxlDecoder, DecoderDeleter> dec;
    JxlInfo info;
    bool usesOriginal = true;
    bool pixels = false;
    int next = 0;       // the next shown frame the decoder comes to
    int skip = 0;       // frames to skip once the header is read
    bool started = false;
    std::vector<float> buf;
    int channels = 4;
    int planes = 4;           // libjxl's own: colour and extra channels
    std::string colourError;  // why its colour cannot be read
#ifdef MONTAGE_WITH_LCMS2
    Transform toDisplayLight;  // its ICC profile's colours to linear Rec.709 display light
#endif
    bool converted() const {
#ifdef MONTAGE_WITH_LCMS2
        return toDisplayLight != nullptr;
#else
        return false;
#endif
    }

    bool start(bool wantPixels, std::string* error) {
        pixels = wantPixels;
        skip = 0;
        dec.reset(JxlDecoderCreate(nullptr));
        if (!dec || (wantPixels && !runner.make())) return fail(error, "Out of memory");
        if (wantPixels && JxlDecoderSetParallelRunner(dec.get(), JxlThreadParallelRunner, runner.r) != JXL_DEC_SUCCESS)
            return fail(error, "Cannot start libjxl");
        int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING;
        events |= wantPixels ? JXL_DEC_FULL_IMAGE : JXL_DEC_FRAME;
        if (JxlDecoderSubscribeEvents(dec.get(), events) != JXL_DEC_SUCCESS) return fail(error, "Cannot start libjxl");
        // (the header as stored, to read its orientation: pixels come out turned as it says, with a header to match)
        if (!wantPixels) JxlDecoderSetKeepOrientation(dec.get(), JXL_TRUE);
        return input(error);
    }
    bool input(std::string* error) {
        if (JxlDecoderSetInput(dec.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) return fail(error, "Cannot start libjxl");
        JxlDecoderCloseInput(dec.get());
        next = 0;
        started = true;
        return true;
    }

    bool basicInfo(std::string* error) {
        JxlBasicInfo bi;
        if (JxlDecoderGetBasicInfo(dec.get(), &bi) != JXL_DEC_SUCCESS) return fail(error, "Cannot read the JPEG XL header");
        const bool swap = bi.orientation >= JXL_ORIENT_TRANSPOSE;
        info.width = int(swap ? bi.ysize : bi.xsize);
        info.height = int(swap ? bi.xsize : bi.ysize);
        info.bits = int(bi.bits_per_sample);
        info.exponentBits = int(bi.exponent_bits_per_sample);
        info.gray = bi.num_color_channels == 1;
        info.alpha = bi.alpha_bits > 0;
        info.alphaPremultiplied = info.alpha && bi.alpha_premultiplied;
        info.orientation = int(bi.orientation);
        info.intensityTarget = bi.intensity_target;
        info.animated = bi.have_animation;
        info.tpsNum = bi.have_animation ? bi.animation.tps_numerator : 0;
        info.tpsDen = bi.have_animation ? std::max<uint32_t>(1, bi.animation.tps_denominator) : 1;
        usesOriginal = bi.uses_original_profile;
        channels = info.gray ? (info.alpha ? 2 : 1) : 4;
        planes = int(bi.num_color_channels + bi.num_extra_channels);
        if (info.width <= 0 || info.height <= 0) return fail(error, "The JPEG XL file has no picture");
        // (level 10's pictures go to 2^30 a side: a few hundred bytes of header can ask for any of them)
        if (uint64_t(info.width) * uint64_t(info.height) > kMaxPixels) return fail(error, "The JPEG XL picture is too large (over 67 million pixels)");
        if (skip > 0) {
            JxlDecoderSkipFrames(dec.get(), size_t(skip));
            next += skip;
            skip = 0;
        }
        return true;
    }

    void colour() {
        JxlColorEncoding original;
        const bool encoded = encodedProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &original) == JXL_DEC_SUCCESS;
        info.icc = !encoded;
        // Lossy pictures (coded in XYB) come out in their own encoding when it is one: libjxl converts XYB to any.
        if (encoded && !usesOriginal) JxlDecoderSetPreferredColorProfile(dec.get(), &original);
        colourError.clear();
#ifdef MONTAGE_WITH_LCMS2
        toDisplayLight.reset();
#endif
        JxlColorEncoding data;
        QByteArray icc;
        if (encodedProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA, &data) == JXL_DEC_SUCCESS) {
            info.colorSpace = spaceFor(data);
        } else if (iccProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA, icc)) {
            info.colorSpace = spaceForIcc(icc);
        } else {
            info.colorSpace.clear();
        }
        // (a lossy picture whose colour is an ICC profile comes out in libjxl's linear sRGB: its display's light)
        if (!info.colorSpace.empty() && !usesOriginal && !encoded)
            if (const ColorSpace* cs = findColorSpace(info.colorSpace)) info.colorSpace = displayLightOf(*cs).id;
        if (!info.colorSpace.empty()) return;
        // Any other colour (an ICC profile, or an encoding, that is none of Montage's spaces: ProPhoto, Rec.2020 PQ by
        // profile, DCI white): converted through its ICC profile (libjxl makes one for an encoding) to linear Rec.709
        // display light, else refused rather than read as Rec.709.
        if (icc.isEmpty() && !iccProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA, icc)) {
            colourError = "The JPEG XL file states no colour Montage can read";
            return;
        }
#ifdef MONTAGE_WITH_LCMS2
        toDisplayLight = iccToDisplayLight(icc, channels, colourError);
        if (toDisplayLight) info.colorSpace = displayLightOf(linearColorSpace(Primaries::Bt709)).id;
#else
        colourError = "The JPEG XL file's colour profile is none of Montage's spaces (this build has no Little CMS to convert it)";
#endif
    }

    // Whether decoding a frame fits: libjxl's planes, the samples it gives, and the RGBA made from them (where it is
    // not those samples themselves).
    bool fits(std::string* error) const {
        const uint64_t perPixel = uint64_t(planes) * 4 + uint64_t(channels) * 4 + (channels != 4 || converted() ? 16 : 0);
        if (uint64_t(info.width) * uint64_t(info.height) * perPixel > kMaxDecodeBytes)
            return fail(error, "The JPEG XL picture is too large to decode (over 2 GB)");
        return true;
    }

    // Runs until frame `want` is decoded into `buf` (with pixels), or to the end gathering frame headers (want < 0).
    bool run(int want, std::string* error) {
        for (;;) {
            const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
            switch (st) {
                case JXL_DEC_ERROR: return fail(error, "The JPEG XL file cannot be decoded");
                case JXL_DEC_NEED_MORE_INPUT: return fail(error, "The JPEG XL file is cut short");
                case JXL_DEC_BASIC_INFO:
                    if (!basicInfo(error)) return false;
                    break;
                case JXL_DEC_COLOR_ENCODING:
                    colour();
                    if (!colourError.empty()) return fail(error, colourError);
                    if (!fits(error)) return false;
                    break;
                case JXL_DEC_FRAME:
                    if (info.animated) {
                        JxlFrameHeader fh;
                        if (JxlDecoderGetFrameHeader(dec.get(), &fh) == JXL_DEC_SUCCESS) info.durations.push_back(fh.duration);
                    }
                    ++next;
                    break;
                case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                    size_t bytes = 0;
                    const JxlPixelFormat f{uint32_t(channels), JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
                    if (JxlDecoderImageOutBufferSize(dec.get(), &f, &bytes) != JXL_DEC_SUCCESS) return fail(error, "Cannot decode the JPEG XL picture");
                    buf.resize(bytes / sizeof(float));
                    if (JxlDecoderSetImageOutBuffer(dec.get(), &f, buf.data(), buf.size() * sizeof(float)) != JXL_DEC_SUCCESS)
                        return fail(error, "Cannot decode the JPEG XL picture");
                    break;
                }
                case JXL_DEC_FULL_IMAGE:
                    if (next++ == want) return true;
                    break;
                case JXL_DEC_SUCCESS:
                    if (want < 0) return true;
                    return fail(error, "The JPEG XL file has no frame " + std::to_string(want));
                default: break;
            }
        }
    }

    // The frame just decoded as RGBA, its samples handed over (none kept here between frames).
    void rgba(int& w, int& h, std::vector<float>& out) {
        w = info.width, h = info.height;
        const size_t n = size_t(w) * size_t(h);
#ifdef MONTAGE_WITH_LCMS2
        if (toDisplayLight) {
            const bool alpha = channels == 2 || channels == 4, premultiplied = alpha && info.alphaPremultiplied;
            out.assign(n * 4, 1.0f);
            // (the profile's curves work on straight colour)
            parallelRows(h, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    float* in = &buf[size_t(y) * size_t(w) * size_t(channels)];
                    float* o = &out[size_t(y) * size_t(w) * 4];
                    if (premultiplied)
                        for (int x = 0; x < w; ++x) {
                            float* q = in + size_t(x) * size_t(channels);
                            const float a = q[channels - 1];
                            if (a > 0)
                                for (int c = 0; c < channels - 1; ++c) q[c] /= a;
                        }
                    cmsDoTransform(toDisplayLight.get(), in, o, cmsUInt32Number(w));
                    if (premultiplied)
                        for (int x = 0; x < w; ++x)
                            for (int c = 0; c < 3; ++c) o[size_t(x) * 4 + size_t(c)] *= o[size_t(x) * 4 + 3];
                }
            });
            std::vector<float>().swap(buf);
            return;
        }
#endif
        if (channels == 4) {
            out = std::move(buf);
            out.resize(n * 4);
            std::vector<float>().swap(buf);
            return;
        }
        out.resize(n * 4);
        for (size_t i = 0; i < n; ++i) {
            const float v = buf[i * size_t(channels)];
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = v;
            out[i * 4 + 3] = channels == 2 ? buf[i * 2 + 1] : 1.0f;
        }
        std::vector<float>().swap(buf);
    }
};

bool sessionInfo(Session& s, std::string* error) {
    if (!s.start(false, error) || !s.run(-1, error)) return false;
    s.info.jpeg = hasJpegBox(s.data);
    if (s.info.animated && s.info.durations.empty()) s.info.durations.push_back(0);
    return true;
}

}  // namespace

bool readJxlInfo(const std::string& file, JxlInfo& out, std::string* error) {
    Session s;
    if (!readAll(file, s.data, error) || !sessionInfo(s, error)) return false;
    out = s.info;
    return true;
}

bool readJxlFrame(const std::string& file, int k, int& width, int& height, std::vector<float>& rgba, bool* premultiplied, std::string* error) {
    Session s;
    if (!readAll(file, s.data, error)) return false;
    if (!s.start(true, error)) return false;
    s.skip = std::max(0, k);
    try {
        if (!s.run(std::max(0, k), error)) return false;
        s.rgba(width, height, rgba);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory for the JPEG XL picture");
    }
    if (premultiplied) *premultiplied = s.info.alphaPremultiplied;
    return true;
}

struct JxlAnimationReader::State {
    Session pixels;
    JxlInfo info;
};

JxlAnimationReader::JxlAnimationReader() : s_(std::make_unique<State>()) {}
JxlAnimationReader::~JxlAnimationReader() = default;

bool JxlAnimationReader::open(const std::string& file, std::string* error) {
    s_ = std::make_unique<State>();
    Session head;
    if (!readAll(file, head.data, error) || !sessionInfo(head, error)) return false;
    s_->info = head.info;
    s_->pixels.data = std::move(head.data);
    return s_->pixels.start(true, error);
}

const JxlInfo& JxlAnimationReader::info() const { return s_->info; }

bool JxlAnimationReader::frame(int k, int& width, int& height, std::vector<float>& rgba, std::string* error) {
    Session& p = s_->pixels;
    if (!p.dec) return fail(error, "No JPEG XL file is open");
    k = std::clamp(k, 0, std::max(0, s_->info.frames() - 1));
    if (k < p.next) {
        // (a new decoder: libjxl 0.7's rewound one comes to the end without the frames)
        if (!p.start(true, error)) return false;
        p.skip = k;
    } else if (k > p.next) {
        JxlDecoderSkipFrames(p.dec.get(), size_t(k - p.next));
        p.next = k;
    }
    bool ok = false;
    try {
        ok = p.run(k, error);
        if (ok) p.rgba(width, height, rgba);
    } catch (const std::bad_alloc&) {
        fail(error, "Not enough memory for the JPEG XL picture");
    }
    if (!ok) {
        p.start(true, nullptr);  // (from the start next time)
        return false;
    }
    return true;
}

bool jxlCanState(const ColorSpace& space) {
    JxlColorEncoding e;
    return encodingFor(space, e);
}

namespace {

bool processOutput(JxlEncoder* enc, std::vector<uint8_t>& out, std::string* error) {
    out.resize(1 << 16);
    uint8_t* next = out.data();
    size_t avail = out.size();
    for (;;) {
        const JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st != JXL_ENC_NEED_MORE_OUTPUT) return fail(error, "libjxl could not encode the picture (error " + std::to_string(int(JxlEncoderGetError(enc))) + ")");
        const size_t used = size_t(next - out.data());
        out.resize(out.size() * 2);
        next = out.data() + used;
        avail = out.size() - used;
    }
    out.resize(size_t(next - out.data()));
    return true;
}

bool saveBytes(const std::string& file, const std::vector<uint8_t>& bytes, std::string* error) {
    QSaveFile f(QString::fromStdString(file));
    if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) ||
        !f.commit())
        return fail(error, "Cannot write " + file);
    return true;
}

}  // namespace

bool encodeJxl(std::vector<uint8_t>& out, int width, int height, const std::vector<const float*>& frames, const JxlWriteOptions& o,
               std::string* error, const std::vector<uint32_t>& durations, uint32_t tpsNum, uint32_t tpsDen) {
    if (width <= 0 || height <= 0 || frames.empty()) return fail(error, "Nothing to write");
    const bool animated = frames.size() > 1 || !durations.empty();
    if (animated && (durations.size() != frames.size() || tpsNum == 0 || tpsDen == 0))
        return fail(error, "An animation needs each frame's duration and its ticks per second");
    const bool floats = o.bits == 32 || (o.bits == 16 && o.halfFloat);
    if (!(o.bits == 32 || (o.bits >= 8 && o.bits <= 16))) return fail(error, "JPEG XL is written at 8 to 16 bits, half float or float");
    if (o.distance < 0 || o.distance > 25) return fail(error, "A JPEG XL distance is 0 (lossless) to 25");
    if (o.orientation < 1 || o.orientation > 8) return fail(error, "An orientation is 1 to 8");
    const bool lossless = o.distance == 0;
    if (o.alpha && o.premultiplied && !jxlWritesPremultiplied())
        return fail(error, "This libjxl (before 0.9) cannot write premultiplied alpha: write it straight");
    const ColorSpace* space = o.colorSpace.empty() ? findColorSpace("srgb") : findColorSpace(o.colorSpace);
    JxlColorEncoding colour;
    if (o.iccProfile.empty() && (!space || !encodingFor(*space, colour)))
        return fail(error, "JPEG XL cannot state the colour space " + o.colorSpace + " (camera log): write it as linear light");

    Runner runner;  // (declared before the encoder, which uses it, so it outlives it)
    std::unique_ptr<JxlEncoder, EncoderDeleter> enc(JxlEncoderCreate(nullptr));
    if (!enc || !runner.make()) return fail(error, "Out of memory");
    if (JxlEncoderSetParallelRunner(enc.get(), JxlThreadParallelRunner, runner.r) != JXL_ENC_SUCCESS) return fail(error, "Cannot start libjxl");
    // Level 10 for float samples and pictures past level 5's limits.
    if (o.bits > 16 || int64_t(width) * height > (int64_t(1) << 28) || width > (1 << 18) || height > (1 << 18))
        JxlEncoderSetCodestreamLevel(enc.get(), 10);
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = uint32_t(width);
    bi.ysize = uint32_t(height);
    bi.bits_per_sample = uint32_t(o.bits);
    bi.exponent_bits_per_sample = o.bits == 32 ? 8 : floats ? 5 : 0;
    bi.num_color_channels = 3;
    bi.num_extra_channels = o.alpha ? 1 : 0;
    bi.alpha_bits = o.alpha ? bi.bits_per_sample : 0;
    bi.alpha_exponent_bits = o.alpha ? bi.exponent_bits_per_sample : 0;
    bi.alpha_premultiplied = o.alpha && o.premultiplied;
    bi.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (o.intensityTarget > 0) bi.intensity_target = float(o.intensityTarget);
    bi.orientation = JxlOrientation(o.orientation);
    if (animated) {
        bi.have_animation = JXL_TRUE;
        bi.animation.tps_numerator = tpsNum;
        bi.animation.tps_denominator = tpsDen;
        bi.animation.num_loops = 0;
    }
    if (JxlEncoderSetBasicInfo(enc.get(), &bi) != JXL_ENC_SUCCESS) return fail(error, "libjxl refused the picture's header");
    if (o.alpha) {
        JxlExtraChannelInfo eci;
        JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_ALPHA, &eci);
        eci.bits_per_sample = bi.alpha_bits;
        eci.exponent_bits_per_sample = bi.alpha_exponent_bits;
        eci.alpha_premultiplied = bi.alpha_premultiplied;
        if (JxlEncoderSetExtraChannelInfo(enc.get(), 0, &eci) != JXL_ENC_SUCCESS) return fail(error, "libjxl refused the alpha channel");
    }
    if (!o.iccProfile.empty() ? JxlEncoderSetICCProfile(enc.get(), o.iccProfile.data(), o.iccProfile.size()) != JXL_ENC_SUCCESS
                              : JxlEncoderSetColorEncoding(enc.get(), &colour) != JXL_ENC_SUCCESS)
        return fail(error, "libjxl refused the colour encoding");
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (!fs) return fail(error, "Out of memory");
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, std::clamp(o.effort, 1, 9));
    if (lossless ? JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS : JxlEncoderSetFrameDistance(fs, float(o.distance)) != JXL_ENC_SUCCESS)
        return fail(error, "libjxl refused the distance");

    const int nc = o.alpha ? 4 : 3;
    const size_t samples = size_t(width) * size_t(height) * size_t(nc);
    const JxlDataType type = o.bits == 32 ? JXL_TYPE_FLOAT : floats ? JXL_TYPE_FLOAT16 : o.bits == 8 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16;
    const JxlPixelFormat fmt{uint32_t(nc), type, JXL_NATIVE_ENDIAN, 0};
    std::vector<float> f32;
    std::vector<uint16_t> u16;
    std::vector<uint8_t> u8;
    // (an integer depth below 16 coded from 16-bit samples: libjxl scales them to its bits, so codes come back exactly)
    const double top = o.bits == 8 ? 255.0 : 65535.0;
    const double codes = o.bits <= 16 && !floats ? double((1u << o.bits) - 1) : 0;
    for (size_t fi = 0; fi < frames.size(); ++fi) {
        const float* src = frames[fi];
        std::vector<float> px(samples);
        for (size_t i = 0, n = size_t(width) * size_t(height); i < n; ++i) {
            const float* q = src + i * 4;
            float c[4] = {q[0], q[1], q[2], q[3]};
            if (o.alpha) {
                if (o.sourcePremultiplied && !o.premultiplied) {
                    if (c[3] > 0) c[0] /= c[3], c[1] /= c[3], c[2] /= c[3];
                } else if (!o.sourcePremultiplied && o.premultiplied) {
                    c[0] *= c[3], c[1] *= c[3], c[2] *= c[3];
                }
            }
            for (int k = 0; k < nc; ++k) px[i * size_t(nc) + size_t(k)] = c[k];
        }
        const void* data = nullptr;
        size_t bytes = 0;
        if (type == JXL_TYPE_FLOAT) {
            f32 = std::move(px);
            data = f32.data(), bytes = f32.size() * sizeof(float);
        } else if (type == JXL_TYPE_FLOAT16) {
            u16.resize(samples);
            for (size_t i = 0; i < samples; ++i) {
                const qfloat16 h(px[i]);
                std::memcpy(&u16[i], &h, sizeof(uint16_t));
            }
            data = u16.data(), bytes = u16.size() * sizeof(uint16_t);
        } else if (type == JXL_TYPE_UINT16) {
            u16.resize(samples);
            for (size_t i = 0; i < samples; ++i) {
                const double v = std::clamp(double(px[i]), 0.0, 1.0);
                // (rounded to the depth's codes first, so a code given as code / (2^bits - 1) stays that code)
                u16[i] = uint16_t(std::lround(std::round(v * codes) / codes * top));
            }
            data = u16.data(), bytes = u16.size() * sizeof(uint16_t);
        } else {
            u8.resize(samples);
            for (size_t i = 0; i < samples; ++i) u8[i] = uint8_t(std::lround(std::clamp(double(px[i]), 0.0, 1.0) * top));
            data = u8.data(), bytes = u8.size();
        }
        if (animated) {
            JxlFrameHeader fh;
            JxlEncoderInitFrameHeader(&fh);
            fh.duration = durations[fi];
            if (JxlEncoderSetFrameHeader(fs, &fh) != JXL_ENC_SUCCESS) return fail(error, "libjxl refused a frame's duration");
        }
        if (JxlEncoderAddImageFrame(fs, &fmt, data, bytes) != JXL_ENC_SUCCESS)
            return fail(error, "libjxl could not take the picture (error " + std::to_string(int(JxlEncoderGetError(enc.get()))) + ")");
    }
    JxlEncoderCloseInput(enc.get());
    return processOutput(enc.get(), out, error);
}

bool writeJxl(const std::string& file, int width, int height, const std::vector<const float*>& frames, const JxlWriteOptions& options,
              std::string* error, const std::vector<uint32_t>& durations, uint32_t tpsNum, uint32_t tpsDen) {
    std::vector<uint8_t> bytes;
    return encodeJxl(bytes, width, height, frames, options, error, durations, tpsNum, tpsDen) && saveBytes(file, bytes, error);
}

bool writeJxl(const std::string& file, int width, int height, const float* rgba, const JxlWriteOptions& options, std::string* error) {
    return writeJxl(file, width, height, std::vector<const float*>{rgba}, options, error);
}

bool jpegToJxl(const std::string& jpeg, const std::string& jxl, std::string* error, int effort) {
    std::vector<uint8_t> in;
    if (!readAll(jpeg, in, error)) return false;
    if (in.size() < 4 || in[0] != 0xff || in[1] != 0xd8) return fail(error, jpeg + " is not a JPEG");
    Runner runner;  // (declared before the encoder, which uses it, so it outlives it)
    std::unique_ptr<JxlEncoder, EncoderDeleter> enc(JxlEncoderCreate(nullptr));
    if (!enc || !runner.make()) return fail(error, "Out of memory");
    JxlEncoderSetParallelRunner(enc.get(), JxlThreadParallelRunner, runner.r);
    if (JxlEncoderStoreJPEGMetadata(enc.get(), JXL_TRUE) != JXL_ENC_SUCCESS) return fail(error, "Cannot start libjxl");
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (!fs) return fail(error, "Out of memory");
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, std::clamp(effort, 1, 9));
    if (JxlEncoderAddJPEGFrame(fs, in.data(), in.size()) != JXL_ENC_SUCCESS)
        return fail(error, "libjxl cannot recompress " + jpeg + " losslessly (a progressive or unusual JPEG it does not take)");
    JxlEncoderCloseInput(enc.get());
    std::vector<uint8_t> out;
    return processOutput(enc.get(), out, error) && saveBytes(jxl, out, error);
}

bool jxlToJpeg(const std::string& jxl, std::vector<uint8_t>& jpeg, std::string* error) {
    std::vector<uint8_t> in;
    if (!readAll(jxl, in, error)) return false;
    if (!hasJpegBox(in)) return fail(error, jxl + " holds no JPEG to rebuild");
    std::unique_ptr<JxlDecoder, DecoderDeleter> dec(JxlDecoderCreate(nullptr));
    if (!dec) return fail(error, "Out of memory");
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec.get(), in.data(), in.size()) != JXL_DEC_SUCCESS)
        return fail(error, "Cannot start libjxl");
    JxlDecoderCloseInput(dec.get());
    jpeg.clear();
    size_t used = 0;
    bool rebuilding = false;
    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
        if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT) return fail(error, "Cannot rebuild the JPEG from " + jxl);
        if (st == JXL_DEC_JPEG_RECONSTRUCTION) {
            rebuilding = true;
            jpeg.resize(1 << 16);
            JxlDecoderSetJPEGBuffer(dec.get(), jpeg.data(), jpeg.size());
        } else if (st == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
            used = jpeg.size() - JxlDecoderReleaseJPEGBuffer(dec.get());
            jpeg.resize(jpeg.size() * 2);
            JxlDecoderSetJPEGBuffer(dec.get(), jpeg.data() + used, jpeg.size() - used);
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            return fail(error, jxl + " holds no JPEG to rebuild");
        } else if (st == JXL_DEC_FULL_IMAGE) {
            if (!rebuilding) return fail(error, jxl + " holds no JPEG to rebuild");
            jpeg.resize(jpeg.size() - JxlDecoderReleaseJPEGBuffer(dec.get()));
        } else if (st == JXL_DEC_SUCCESS) {
            return rebuilding || fail(error, jxl + " holds no JPEG to rebuild");
        }
    }
}

#endif

}  // namespace montage
