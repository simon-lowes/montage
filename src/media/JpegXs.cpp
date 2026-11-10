// Montage — JPEG XS through SVT-JPEG-XS (see JpegXs.h).
#include "JpegXs.h"

#include "render/ColorSpace.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#ifdef MONTAGE_WITH_JPEGXS
#include <SvtJpegxsDec.h>
#include <SvtJpegxsEnc.h>
#endif

#ifdef MONTAGE_WITH_BMX
#include <bmx/BMXException.h>
#include <bmx/MXFUtils.h>
#include <bmx/Utils.h>
#include <bmx/clip_writer/ClipWriter.h>
#include <bmx/mxf_reader/MXFFileReader.h>
#include <libMXF++/MXF.h>
#include <mxf/mxf_labels_and_keys.h>

#include "render/MxfFiles.h"
#endif

namespace montage {

namespace {

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// ISO/IEC 21122-2's profile codes and names, and the level and sublevel parts of Plev.
struct Named {
    uint16_t value;
    const char* name;
};
constexpr Named kProfiles[] = {{0x1500, "Light 422.10"}, {0x1A00, "Light 444.12"}, {0x2500, "Light-Subline 422.10"},
                               {0x3240, "Main 420.12"},  {0x3540, "Main 422.10"},  {0x3A40, "Main 444.12"},
                               {0x3E40, "Main 4444.12"}, {0x4240, "High 420.12"},  {0x4A40, "High 444.12"},
                               {0x4E40, "High 4444.12"}, {0x6EC0, "MLS.12"}};
constexpr Named kLevels[] = {{0x0000, "Unrestricted"}, {0x01, "1k-1"}, {0x04, "2k-1"}, {0x08, "4k-1"}, {0x09, "4k-2"}, {0x0A, "4k-3"},
                             {0x0B, "5k-1"},         {0x0C, "8k-1"}, {0x0D, "8k-2"}, {0x0E, "8k-3"}, {0x10, "10k-1"}};
constexpr Named kSublevels[] = {{0x00, "Unrestricted"}, {0x10, "Sublev12bpp"}, {0x0C, "Sublev9bpp"}, {0x08, "Sublev6bpp"},
                                {0x06, "Sublev4bpp"},   {0x04, "Sublev3bpp"},  {0x03, "Sublev2bpp"}};

template <size_t N>
std::string named(const Named (&table)[N], uint16_t v) {
    for (const Named& n : table)
        if (n.value == v) return n.name;
    return {};
}

std::string spaceForTags(int primaries, int transfer) {
    const char* pri = av_color_primaries_name(AVColorPrimaries(primaries));
    const char* trc = av_color_transfer_name(AVColorTransferCharacteristic(transfer));
    std::string id = colorSpaceFromTags(pri ? pri : "", trc ? trc : "");
    return id == "rec709" ? std::string() : id;
}

AVPixelFormat pixelFormat(JxsSampling s, int depth) {
    const int d = depth <= 8 ? 8 : depth <= 10 ? 10 : 12;
    switch (s) {
    case JxsSampling::Ycc422: return d == 8 ? AV_PIX_FMT_YUV422P : d == 10 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P12LE;
    case JxsSampling::Ycc444: return d == 8 ? AV_PIX_FMT_YUV444P : d == 10 ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P12LE;
    case JxsSampling::Rgb: return d == 8 ? AV_PIX_FMT_GBRP : d == 10 ? AV_PIX_FMT_GBRP10LE : AV_PIX_FMT_GBRP12LE;
    }
    return AV_PIX_FMT_NONE;
}

// FFmpeg's GBR planes are G, B, R; a picture's are R, G, B.
int avPlane(JxsSampling s, int c) { return s == JxsSampling::Rgb ? (c + 2) % 3 : c; }

}  // namespace

// ---- Codestream headers.

bool readJxsHeader(const uint8_t* data, size_t size, JxsHeader& out) {
    out = {};
    if (!data || size < 4 || be16(data) != 0xFF10) return false;
    size_t at = 2;
    bool pih = false, cdt = false;
    while (at + 2 <= size) {
        const uint16_t marker = be16(data + at);
        if ((marker >> 8) != 0xFF) return false;
        if (marker == 0xFF20 || marker == 0xFF11) {  // the first slice (or the end): the header is done
            out.headerBytes = at;
            return pih && cdt;
        }
        if (at + 4 > size) return false;
        const size_t len = be16(data + at + 2);
        if (len < 2 || at + 2 + len > size) return false;
        const uint8_t* d = data + at + 4;
        const size_t n = len - 2;
        if (marker == 0xFF12) {
            if (n < 24) return false;
            out.lcod = be32(d);
            out.ppih = be16(d + 4);
            out.plev = be16(d + 6);
            out.width = be16(d + 8);
            out.height = be16(d + 10);
            out.cw = be16(d + 12);
            out.hsl = be16(d + 14);
            out.components = d[16];
            out.cpih = d[21] & 0x0F;
            out.decompV = d[22] & 0x0F;
            pih = true;
        } else if (marker == 0xFF13) {
            if (!pih) return false;
            out.componentTable.assign(data + at, data + at + 2 + len);
            for (size_t c = 0; c < n / 2 && c < 4; ++c) {
                if (c == 0) out.depth = d[0];
                out.sx[c] = d[c * 2 + 1] >> 4;
                out.sy[c] = d[c * 2 + 1] & 0x0F;
            }
            cdt = true;
        }
        at += 2 + len;
    }
    return false;
}

std::string jxsProfileName(uint16_t ppih) { return named(kProfiles, ppih); }
std::string jxsLevelName(uint16_t plev) { return named(kLevels, uint16_t(plev >> 10)); }
std::string jxsSublevelName(uint16_t plev) { return named(kSublevels, uint16_t(plev & 0xFF)); }

size_t jxsCodestreamSize(const uint8_t* data, size_t size) {
    JxsHeader h;
    if (!readJxsHeader(data, size, h)) return 0;
    if (h.lcod) return h.lcod <= size && h.lcod >= h.headerBytes + 2 ? h.lcod : 0;
#ifdef MONTAGE_WITH_JPEGXS
    uint32_t n = 0;
    if (svt_jpeg_xs_decoder_get_single_frame_size(data, size, nullptr, &n, 0) == SvtJxsErrorNone && n <= size) return n;
#endif
    return 0;
}

std::string jxsSamplingName(JxsSampling s) {
    return s == JxsSampling::Ycc422 ? "YCbCr-4:2:2" : s == JxsSampling::Ycc444 ? "YCbCr-4:4:4" : "RGB";
}

JxsPicture::JxsPicture(int w, int h, int d, JxsSampling s) : width(w), height(h), depth(d), sampling(s) {
    for (int c = 0; c < 3; ++c) planes[c].assign(size_t(planeWidth(c)) * size_t(std::max(h, 0)), 0);
}

bool JxsPicture::operator==(const JxsPicture& o) const {
    return width == o.width && height == o.height && depth == o.depth && sampling == o.sampling && planes[0] == o.planes[0] &&
           planes[1] == o.planes[1] && planes[2] == o.planes[2];
}

#ifdef MONTAGE_WITH_JPEGXS

namespace {

// SVT's messages go nowhere: what failed is said by its return codes.
void quietLog(void*, SvtLogLevel, const char*, const char*, va_list) {}
void quietSvt() {
    static const bool once = [] {
        svt_jpeg_xs_set_log_callback(quietLog, nullptr);
        return true;
    }();
    (void)once;
}

ColourFormat_t colourFormat(JxsSampling s) {
    return s == JxsSampling::Ycc422 ? COLOUR_FORMAT_PLANAR_YUV422 : s == JxsSampling::Ycc444 ? COLOUR_FORMAT_PLANAR_YUV444 : COLOUR_FORMAT_PLANAR_RGB;
}

std::string svtError(SvtJxsErrorType_t e) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08X", unsigned(e));
    return buf;
}

int threadsFor(int asked) {
    if (asked > 0) return asked;
    return int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
}

}  // namespace

bool jpegXsAvailable() { return true; }

std::string jpegXsVersion() {
    return "SVT-JPEG-XS " + std::to_string(SVT_JPEGXS_API_VER_MAJOR) + "." + std::to_string(SVT_JPEGXS_API_VER_MINOR);
}

struct JxsEncoder::State {
    JxsEncodeSettings s;
    svt_jpeg_xs_encoder_api_t api{};
    svt_jpeg_xs_image_config_t config{};
    uint32_t bytes = 0;
    bool open = false;
    std::vector<uint8_t> planes8[3];  // (8-bit input)
    std::vector<uint8_t> buffer;
};

JxsEncoder::JxsEncoder() : s_(std::make_unique<State>()) {}
JxsEncoder::~JxsEncoder() { close(); }

const JxsEncodeSettings& JxsEncoder::settings() const { return s_->s; }
uint32_t JxsEncoder::bytesPerPicture() const { return s_->bytes; }

void JxsEncoder::close() {
    if (s_->open) svt_jpeg_xs_encoder_close(&s_->api);
    s_->open = false;
}

bool JxsEncoder::open(const JxsEncodeSettings& s, std::string* error) {
    close();
    quietSvt();
    if (s.width < 16 || s.height < 16 || s.width > 32767 || s.height > 32767) return fail(error, "JPEG XS pictures are 16 to 32767 pixels across and down");
    if (s.depth < 8 || s.depth > 12) return fail(error, "JPEG XS is coded here at 8 to 12 bits");
    if (s.bppNum <= 0 || s.bppDen <= 0) return fail(error, "A JPEG XS bit rate is a positive number of bits a pixel");
    if (s.sampling == JxsSampling::Ycc422 && s.width % 2) return fail(error, "JPEG XS 4:2:2 pictures are an even number of pixels wide");
    State& st = *s_;
    st.s = s;
    std::memset(&st.api, 0, sizeof st.api);
    if (svt_jpeg_xs_encoder_load_default_parameters(SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, &st.api) != SvtJxsErrorNone)
        return fail(error, "SVT-JPEG-XS refused its own defaults");
    st.api.source_width = uint32_t(s.width);
    st.api.source_height = uint32_t(s.height);
    st.api.input_bit_depth = uint8_t(s.depth);
    st.api.colour_format = colourFormat(s.sampling);
    // (R′G′B′ through the reversible colour transform, as decoders take a Cpih=1 stream as R′G′B′)
    st.api.enable_color_transform = s.sampling == JxsSampling::Rgb ? 1 : 0;
    st.api.bpp_numerator = uint32_t(s.bppNum);
    st.api.bpp_denominator = uint32_t(s.bppDen);
    st.api.slice_height = uint32_t(std::max(s.sliceHeight, 4));
    // A budget for each slice, so each is as long in every picture: RFC 9134's slice packetisation then sends as many
    // packets each frame.
    st.api.rate_control_mode = 2;
    st.api.slice_packetization_mode = s.slices ? 1 : 0;
    st.api.threads_num = uint32_t(threadsFor(s.threads));
    st.api.verbose = VERBOSE_NONE;
    if (const SvtJxsErrorType_t e = svt_jpeg_xs_encoder_init(SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, &st.api); e != SvtJxsErrorNone)
        return fail(error, "SVT-JPEG-XS cannot code " + std::to_string(s.width) + " x " + std::to_string(s.height) + " at " +
                               std::to_string(s.depth) + " bits and " + std::to_string(s.bppNum) + (s.bppDen != 1 ? "/" + std::to_string(s.bppDen) : "") +
                               " bits a pixel (" + svtError(e) + ")");
    st.open = true;
    if (svt_jpeg_xs_encoder_get_image_config(SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, &st.api, &st.config, &st.bytes) != SvtJxsErrorNone ||
        st.bytes == 0) {
        close();
        return fail(error, "SVT-JPEG-XS gave no picture size");
    }
    st.buffer.resize(st.bytes);
    st.s.sliceHeight = int(st.api.slice_height);
    return true;
}

bool JxsEncoder::encode(const JxsPicture& p, std::vector<uint8_t>& out, std::vector<uint32_t>* units, std::string* error) {
    State& st = *s_;
    if (!st.open) return fail(error, "The JPEG XS encoder is not open");
    if (p.width != st.s.width || p.height != st.s.height || p.sampling != st.s.sampling || p.depth != st.s.depth)
        return fail(error, "A picture of another size, sampling or depth than the JPEG XS encoder's");
    svt_jpeg_xs_frame_t in;
    std::memset(&in, 0, sizeof in);
    for (int c = 0; c < 3; ++c) {
        const size_t n = size_t(st.config.components[c].width) * st.config.components[c].height;
        if (p.planes[c].size() < n) return fail(error, "A JPEG XS picture's plane is short");
        if (st.s.depth == 8) {
            st.planes8[c].resize(n);
            for (size_t i = 0; i < n; ++i) st.planes8[c][i] = uint8_t(std::min<uint16_t>(p.planes[c][i], 255));
            in.image.data_yuv[c] = st.planes8[c].data();
        } else {
            in.image.data_yuv[c] = const_cast<uint16_t*>(p.planes[c].data());
        }
        in.image.stride[c] = st.config.components[c].width;
        in.image.alloc_size[c] = st.config.components[c].byte_size;
    }
    in.bitstream.buffer = st.buffer.data();
    in.bitstream.allocation_size = uint32_t(st.buffer.size());
    if (const SvtJxsErrorType_t e = svt_jpeg_xs_encoder_send_picture(&st.api, &in, 1); e != SvtJxsErrorNone)
        return fail(error, "SVT-JPEG-XS did not take the picture (" + svtError(e) + ")");
    out.clear();
    if (units) units->clear();
    for (;;) {
        svt_jpeg_xs_frame_t got;
        std::memset(&got, 0, sizeof got);
        const SvtJxsErrorType_t e = svt_jpeg_xs_encoder_get_packet(&st.api, &got, 1);
        if (e != SvtJxsErrorNone) return fail(error, "SVT-JPEG-XS could not code the picture (" + svtError(e) + ")");
        out.insert(out.end(), got.bitstream.buffer, got.bitstream.buffer + got.bitstream.used_size);
        if (units) units->push_back(got.bitstream.used_size);
        if (got.bitstream.last_packet_in_frame || !st.s.slices) break;
    }
    return true;
}

struct JxsDecoder::State {
    svt_jpeg_xs_decoder_api_t api{};
    svt_jpeg_xs_image_config_t config{};
    bool open = false;
    JxsHeader header;
    std::vector<uint8_t> headerBytes;
    std::vector<uint8_t> planes[3];
};

JxsDecoder::JxsDecoder() : s_(std::make_unique<State>()) {}
JxsDecoder::~JxsDecoder() { close(); }

void JxsDecoder::close() {
    if (s_->open) svt_jpeg_xs_decoder_close(&s_->api);
    s_->open = false;
    s_->headerBytes.clear();
}

bool JxsDecoder::decode(const uint8_t* data, size_t size, JxsPicture& out, std::string* error, JxsSampling sampling444) {
    State& st = *s_;
    quietSvt();
    JxsHeader h;
    if (!readJxsHeader(data, size, h)) return fail(error, "Not a JPEG XS codestream");
    if (h.components != 3) return fail(error, "JPEG XS with " + std::to_string(h.components) + " components is not read here (three are)");
    // (the decoder is set up for one header: another one, even of another rate alone, starts it again)
    const bool same = st.open && st.headerBytes.size() == h.headerBytes && std::equal(data, data + h.headerBytes, st.headerBytes.begin());
    if (!same) {
        close();
        std::memset(&st.api, 0, sizeof st.api);
        st.api.use_cpu_flags = CPU_FLAGS_ALL;
        st.api.verbose = VERBOSE_NONE;
        st.api.threads_num = uint32_t(threadsFor(0));
        st.api.packetization_mode = 0;
        st.api.proxy_mode = proxy_mode_full;
        if (const SvtJxsErrorType_t e = svt_jpeg_xs_decoder_init(SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, &st.api, data, size, &st.config);
            e != SvtJxsErrorNone)
            return fail(error, "SVT-JPEG-XS cannot decode this codestream (" + svtError(e) + ")");
        st.open = true;
        st.header = h;
        st.headerBytes.assign(data, data + h.headerBytes);
        if (st.config.components_num != 3) {
            close();
            return fail(error, "JPEG XS with other than three components is not read here");
        }
        for (int c = 0; c < 3; ++c) st.planes[c].resize(st.config.components[c].byte_size);
    }
    JxsSampling sampling;
    if (st.config.format == COLOUR_FORMAT_PLANAR_YUV422) sampling = JxsSampling::Ycc422;
    else if (st.config.format == COLOUR_FORMAT_PLANAR_RGB) sampling = JxsSampling::Rgb;
    else if (st.config.format == COLOUR_FORMAT_PLANAR_YUV444) sampling = sampling444;
    else return fail(error, "This JPEG XS sampling is not read here (4:2:2 and 4:4:4 are)");
    svt_jpeg_xs_frame_t in;
    std::memset(&in, 0, sizeof in);
    for (int c = 0; c < 3; ++c) {
        in.image.data_yuv[c] = st.planes[c].data();
        in.image.stride[c] = st.config.components[c].width;
        in.image.alloc_size[c] = st.config.components[c].byte_size;
    }
    in.bitstream.buffer = const_cast<uint8_t*>(data);
    in.bitstream.used_size = uint32_t(size);
    in.bitstream.allocation_size = uint32_t(size);
    if (const SvtJxsErrorType_t e = svt_jpeg_xs_decoder_send_frame(&st.api, &in, 1); e != SvtJxsErrorNone) {
        close();
        return fail(error, "SVT-JPEG-XS did not take the codestream (" + svtError(e) + ")");
    }
    svt_jpeg_xs_frame_t got;
    std::memset(&got, 0, sizeof got);
    if (const SvtJxsErrorType_t e = svt_jpeg_xs_decoder_get_frame(&st.api, &got, 1); e != SvtJxsErrorNone) {
        close();  // (decoded again from a fresh start next time)
        return fail(error, "SVT-JPEG-XS could not decode the codestream (" + svtError(e) + ")");
    }
    const int depth = st.config.bit_depth;
    out = JxsPicture(int(st.config.width), int(st.config.height), depth, sampling);
    for (int c = 0; c < 3; ++c) {
        const size_t n = std::min(out.planes[c].size(), size_t(st.config.components[c].width) * st.config.components[c].height);
        if (depth <= 8)
            for (size_t i = 0; i < n; ++i) out.planes[c][i] = st.planes[c][i];
        else
            std::memcpy(out.planes[c].data(), st.planes[c].data(), n * 2);
    }
    return true;
}

#else

bool jpegXsAvailable() { return false; }
std::string jpegXsVersion() { return {}; }

struct JxsEncoder::State {
    JxsEncodeSettings s;
};
JxsEncoder::JxsEncoder() : s_(std::make_unique<State>()) {}
JxsEncoder::~JxsEncoder() = default;
const JxsEncodeSettings& JxsEncoder::settings() const { return s_->s; }
uint32_t JxsEncoder::bytesPerPicture() const { return 0; }
void JxsEncoder::close() {}
bool JxsEncoder::open(const JxsEncodeSettings&, std::string* error) { return fail(error, "This build of Montage has no JPEG XS (SVT-JPEG-XS)"); }
bool JxsEncoder::encode(const JxsPicture&, std::vector<uint8_t>&, std::vector<uint32_t>*, std::string* error) {
    return fail(error, "This build of Montage has no JPEG XS (SVT-JPEG-XS)");
}
struct JxsDecoder::State {};
JxsDecoder::JxsDecoder() : s_(std::make_unique<State>()) {}
JxsDecoder::~JxsDecoder() = default;
void JxsDecoder::close() {}
bool JxsDecoder::decode(const uint8_t*, size_t, JxsPicture&, std::string* error, JxsSampling) {
    return fail(error, "This build of Montage has no JPEG XS (SVT-JPEG-XS)");
}

#endif

// ---- Files.

namespace {

#ifdef MONTAGE_WITH_BMX
// MXF labels compared but for their version byte (byte 8).
bool sameLabel(const mxfUL& a, const mxfUL& b) {
    const auto* x = reinterpret_cast<const uint8_t*>(&a);
    const auto* y = reinterpret_cast<const uint8_t*>(&b);
    for (int i = 0; i < 16; ++i)
        if (i != 7 && x[i] != y[i]) return false;
    return true;
}

std::string ulText(const mxfUL& l) {
    const auto* b = reinterpret_cast<const uint8_t*>(&l);
    char s[64];
    std::snprintf(s, sizeof s, "urn:smpte:ul:%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4],
                  b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

struct UlCode {
    const mxfUL* label;
    int code;
};
const UlCode kPrimaries[] = {{&ITU709_COLOR_PRIM, 1}, {&ITU2020_COLOR_PRIM, 9}, {&SMPTE170M_COLOR_PRIM, 6}, {&ITU470_PAL_COLOR_PRIM, 5},
                             {&P3D65_COLOR_PRIM, 12}};
const UlCode kTransfers[] = {{&ITUR_BT709_TRANSFER_CH, 1}, {&SMPTE_ST2084_TRANSFER_CH, 16}, {&HLG_OETF_TRANSFER_CH, 18},
                             {&ITU2020_TRANSFER_CH, 14},  {&LINEAR_TRANSFER_CH, 8},        {&SMPTE240M_TRANSFER_CH, 7}};
const UlCode kMatrices[] = {{&ITUR_BT709_CODING_EQ, 1}, {&ITU2020_NCL_CODING_EQ, 9}, {&ITUR_BT601_CODING_EQ, 6},
                            {&SMPTE_240M_CODING_EQ, 7}, {&GBR_CODING_EQ, 0}};

template <size_t N>
const mxfUL* labelFor(const UlCode (&table)[N], int code) {
    for (const UlCode& u : table)
        if (u.code == code) return u.label;
    return nullptr;
}
template <size_t N>
int codeFor(const UlCode (&table)[N], const mxfUL& l, int otherwise) {
    for (const UlCode& u : table)
        if (sameLabel(*u.label, l)) return u.code;
    return otherwise;
}
#endif

// A raw codestream file's pictures: where each starts and how long it is.
bool indexCodestreams(QFile& f, std::vector<std::pair<int64_t, uint32_t>>& out, JxsHeader& first, std::string* error) {
    out.clear();
    const int64_t total = f.size();
    int64_t at = 0;
    std::vector<uint8_t> whole;  // (a variable-rate file: read whole to find each codestream's end)
    while (at < total) {
        if (!f.seek(at)) return fail(error, "Could not read " + f.fileName().toStdString());
        const QByteArray head = f.read(std::min<int64_t>(total - at, 65536));
        JxsHeader h;
        if (!readJxsHeader(reinterpret_cast<const uint8_t*>(head.constData()), size_t(head.size()), h)) {
            if (out.empty()) return fail(error, f.fileName().toStdString() + " is not a JPEG XS codestream");
            break;  // (anything after the last whole codestream is passed over)
        }
        if (out.empty()) first = h;
        uint32_t n = h.lcod;
        if (!n) {
            if (whole.empty()) {
                if (total > (int64_t(1) << 31)) return fail(error, "A variable-rate JPEG XS file over 2 GB is not read here");
                f.seek(0);
                const QByteArray all = f.readAll();
                whole.assign(all.begin(), all.end());
            }
            n = uint32_t(jxsCodestreamSize(whole.data() + at, whole.size() - size_t(at)));
            if (!n) break;
        }
        if (at + n > total) break;  // (a codestream cut short)
        out.push_back({at, n});
        at += n;
    }
    if (out.empty()) return fail(error, f.fileName().toStdString() + " holds no whole JPEG XS codestream");
    return true;
}

JxsSampling samplingOf(const JxsHeader& h) {
    if (h.cpih == 1) return JxsSampling::Rgb;
    return h.sx[1] == 2 ? JxsSampling::Ycc422 : JxsSampling::Ycc444;
}

}  // namespace

JxsContainer jxsContainer(const std::string& file) {
    QFile f(QString::fromStdString(file));
    if (!f.open(QIODevice::ReadOnly)) return JxsContainer::None;
    const QByteArray head = f.read(65536);
    const auto* p = reinterpret_cast<const uint8_t*>(head.constData());
    const size_t n = size_t(head.size());
    if (n >= 4 && be16(p) == 0xFF10 && (be16(p + 2) == 0xFF50 || be16(p + 2) == 0xFF12)) return JxsContainer::Codestream;
    // An MXF whose header names a JPEG XS essence container (ST 2124's MXFGCJPEGXSPictures, any version byte).
    static const uint8_t partition[] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01, 0x0d, 0x01, 0x02, 0x01, 0x01, 0x02};
    if (n < 16 || std::memcmp(p, partition, sizeof partition) != 0) return JxsContainer::None;
    static const uint8_t ec[] = {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01};
    for (size_t i = 0; i + 16 <= n; ++i)
        if (std::memcmp(p + i, ec, sizeof ec) == 0 && p[i + 8] == 0x0d && p[i + 9] == 0x01 && p[i + 10] == 0x03 && p[i + 11] == 0x01 &&
            p[i + 12] == 0x02 && p[i + 13] == 0x21)
            return JxsContainer::Mxf;
    return JxsContainer::None;
}

JxsContainer jxsContainerForPath(const std::string& file) {
    const QString s = QString::fromStdString(file);
    return s.endsWith(".mxf", Qt::CaseInsensitive) ? JxsContainer::Mxf : JxsContainer::Codestream;
}

struct JxsReader::State {
    JxsFileInfo info;
    std::string file;
    QFile f;
    std::vector<std::pair<int64_t, uint32_t>> index;  // (raw codestreams)
#ifdef MONTAGE_WITH_BMX
    std::unique_ptr<bmx::MXFFileReader> mxf;
    size_t pictureTrack = 0;
#endif
    JxsDecoder decoder;
    std::vector<uint8_t> bytes;
};

JxsReader::JxsReader() : s_(std::make_unique<State>()) {}
JxsReader::~JxsReader() {
#ifdef MONTAGE_WITH_BMX
    if (s_->mxf) {
        BmxUse use;
        s_->mxf.reset();
    }
#endif
}

const JxsFileInfo& JxsReader::info() const { return s_->info; }

bool JxsReader::open(const std::string& file, std::string* error) {
    State& st = *s_;
    st.file = file;
    st.info = {};
    st.info.container = jxsContainer(file);
    if (st.info.container == JxsContainer::Codestream) {
        st.f.setFileName(QString::fromStdString(file));
        if (!st.f.open(QIODevice::ReadOnly)) return fail(error, "Could not open " + file);
        if (!indexCodestreams(st.f, st.index, st.info.header, error)) return false;
        st.info.frames = int64_t(st.index.size());
        st.info.sampling = samplingOf(st.info.header);
        if (st.info.sampling == JxsSampling::Rgb) {
            st.info.matrix = 0;
            st.info.fullRange = true;
        }
        return true;
    }
    if (st.info.container != JxsContainer::Mxf) return fail(error, file + " is not a JPEG XS file");
#ifndef MONTAGE_WITH_BMX
    return fail(error, "This build of Montage reads no MXF through bmx, so no JPEG XS in MXF");
#else
    using namespace bmx;
    BmxUse use;
    try {
        st.mxf = std::make_unique<MXFFileReader>();
        st.mxf->SetFileFactory(new Utf8MxfFileFactory, true);
        const MXFFileReader::OpenResult r = st.mxf->Open(file);
        if (r != MXFFileReader::MXF_RESULT_SUCCESS) return fail(error, "Could not read " + file + ": " + MXFFileReader::ResultToString(r));
        const mxfRational rate = st.mxf->GetEditRate();
        st.info.fpsNum = int(rate.numerator);
        st.info.fpsDen = int(rate.denominator);
        st.info.frames = st.mxf->GetDuration();
        if (st.mxf->HaveMaterialTimecode()) st.info.timecode = bmx::get_timecode_string(st.mxf->GetMaterialTimecode(0));
        bool picture = false;
        for (size_t i = 0; i < st.mxf->GetNumTrackReaders(); ++i) {
            MXFTrackReader* tr = st.mxf->GetTrackReader(i);
            const MXFTrackInfo* t = tr->GetTrackInfo();
            if (t->essence_type == JPEGXS_CDCI || t->essence_type == JPEGXS_RGBA) {
                if (!picture) {
                    picture = true;
                    st.pictureTrack = i;
                    st.info.essenceContainer = ulText(t->essence_container_label);
                    mxfpp::FileDescriptor* fd = tr->GetFileDescriptor();
                    if (auto* pd = dynamic_cast<mxfpp::GenericPictureEssenceDescriptor*>(fd)) {
                        if (pd->havePictureEssenceCoding()) st.info.pictureCoding = ulText(pd->getPictureEssenceCoding());
                        if (pd->haveColorPrimaries()) st.info.primaries = codeFor(kPrimaries, pd->getColorPrimaries(), 2);
                        if (pd->haveCaptureGamma()) st.info.transfer = codeFor(kTransfers, pd->getCaptureGamma(), 2);
                        if (pd->haveCodingEquations()) st.info.matrix = codeFor(kMatrices, pd->getCodingEquations(), 2);
                    }
                    if (auto* cd = dynamic_cast<mxfpp::CDCIEssenceDescriptor*>(fd)) {
                        st.info.horizontalSubsampling = int(cd->getHorizontalSubsampling());
                        st.info.verticalSubsampling = cd->haveVerticalSubsampling() ? int(cd->getVerticalSubsampling()) : 1;
                        st.info.componentDepth = int(cd->getComponentDepth());
                        st.info.fullRange = cd->haveBlackRefLevel() && cd->getBlackRefLevel() == 0;
                    } else if (auto* rd = dynamic_cast<mxfpp::RGBAEssenceDescriptor*>(fd)) {
                        st.info.fullRange = !rd->haveComponentMinRef() || rd->getComponentMinRef() == 0;
                        if (!dynamic_cast<mxfpp::GenericPictureEssenceDescriptor*>(fd)->haveCodingEquations()) st.info.matrix = 0;
                    }
                    if (fd && fd->haveSubDescriptors())
                        for (mxfpp::SubDescriptor* sd : fd->getSubDescriptors())
                            if (auto* x = dynamic_cast<mxfpp::JPEGXSSubDescriptor*>(sd)) {
                                st.info.subDescriptor = true;
                                st.info.subPpih = x->getJPEGXSPpih();
                                st.info.subPlev = x->getJPEGXSPlev();
                                st.info.subWf = x->getJPEGXSWf();
                                st.info.subHf = x->getJPEGXSHf();
                                st.info.subNc = x->getJPEGXSNc();
                                const mxfpp::ByteArray t = x->getJPEGXSComponentTable();
                                st.info.subComponentTable.assign(t.data, t.data + t.length);
                                if (x->haveItem(&MXF_ITEM_K(JPEGXSSubDescriptor, JPEGXSCw))) st.info.subCw = x->getJPEGXSCw();
                                if (x->haveItem(&MXF_ITEM_K(JPEGXSSubDescriptor, JPEGXSHsl))) st.info.subHsl = x->getJPEGXSHsl();
                                if (x->haveItem(&MXF_ITEM_K(JPEGXSSubDescriptor, JPEGXSMaximumBitRate))) st.info.subMaxBitRate = x->getJPEGXSMaximumBitRate();
                            }
                    continue;
                }
            } else if (dynamic_cast<const MXFSoundTrackInfo*>(t)) {
                ++st.info.soundTracks;
            }
            tr->SetEnable(false);
        }
        if (!picture) return fail(error, file + " holds no JPEG XS pictures");
        if (st.info.frames <= 0) return fail(error, file + " holds no frames");
    } catch (const std::exception& e) {
        return fail(error, "Could not read " + file + ": " + e.what());
    }
    // The first picture's header (a raw read: the decoder needs the lock no longer).
    std::vector<uint8_t> first;
    {
        try {
            st.mxf->Seek(0);
            if (st.mxf->Read(1) != 1) return fail(error, "Could not read the first picture of " + file);
            std::unique_ptr<Frame> fr(st.mxf->GetTrackReader(st.pictureTrack)->GetFrameBuffer()->GetLastFrame(true));
            if (!fr || fr->IsEmpty()) return fail(error, "The first picture of " + file + " is empty");
            first.assign(fr->GetBytes(), fr->GetBytes() + fr->GetSize());
        } catch (const std::exception& e) {
            return fail(error, "Could not read " + file + ": " + e.what());
        }
    }
    if (!readJxsHeader(first.data(), first.size(), st.info.header)) return fail(error, "The pictures of " + file + " are not JPEG XS codestreams");
    st.info.sampling = samplingOf(st.info.header);
    if (st.info.sampling == JxsSampling::Ycc444 && st.mxf->GetTrackReader(st.pictureTrack)->GetTrackInfo()->essence_type == JPEGXS_RGBA)
        st.info.sampling = JxsSampling::Rgb;  // (R′G′B′ coded without the colour transform: the RGBA descriptor says so)
    st.info.colorSpace = spaceForTags(st.info.primaries, st.info.transfer);
    return true;
#endif
}

bool JxsReader::codestream(int64_t n, std::vector<uint8_t>& out, std::string* error) {
    State& st = *s_;
    if (n < 0 || n >= st.info.frames) return fail(error, "No frame " + std::to_string(n) + " in " + st.file);
    if (st.info.container == JxsContainer::Codestream) {
        const auto [at, size] = st.index[size_t(n)];
        if (!st.f.seek(at)) return fail(error, "Could not read " + st.file);
        out.resize(size);
        if (st.f.read(reinterpret_cast<char*>(out.data()), size) != int64_t(size)) return fail(error, "Could not read " + st.file);
        return true;
    }
#ifdef MONTAGE_WITH_BMX
    BmxUse use;
    try {
        st.mxf->Seek(n);
        if (st.mxf->Read(1) != 1) return fail(error, "Could not read frame " + std::to_string(n) + " of " + st.file);
        std::unique_ptr<bmx::Frame> fr(st.mxf->GetTrackReader(st.pictureTrack)->GetFrameBuffer()->GetLastFrame(true));
        if (!fr || fr->IsEmpty()) return fail(error, "Frame " + std::to_string(n) + " of " + st.file + " is empty");
        out.assign(fr->GetBytes(), fr->GetBytes() + fr->GetSize());
        return true;
    } catch (const std::exception& e) {
        return fail(error, "Could not read " + st.file + ": " + e.what());
    }
#else
    return fail(error, "No MXF reader in this build");
#endif
}

bool JxsReader::picture(int64_t n, JxsPicture& out, std::string* error) {
    State& st = *s_;
    if (!codestream(n, st.bytes, error)) return false;
    return st.decoder.decode(st.bytes.data(), st.bytes.size(), out, error, st.info.sampling == JxsSampling::Rgb ? JxsSampling::Rgb : JxsSampling::Ycc444);
}

bool JxsReader::frame(int64_t n, AVFrame* into, std::string* error) {
    JxsPicture p;
    if (!picture(n, p, error)) return false;
    av_frame_unref(into);
    into->width = p.width;
    into->height = p.height;
    into->format = pixelFormat(p.sampling, p.depth);
    if (av_frame_get_buffer(into, 0) < 0) return fail(error, "Out of memory");
    for (int c = 0; c < 3; ++c) {
        const int w = p.planeWidth(c);
        uint8_t* dst = into->data[avPlane(p.sampling, c)];
        const int ls = into->linesize[avPlane(p.sampling, c)];
        for (int y = 0; y < p.height; ++y) {
            const uint16_t* src = p.planes[c].data() + size_t(y) * size_t(w);
            if (p.depth <= 8)
                for (int x = 0; x < w; ++x) dst[ptrdiff_t(y) * ls + x] = uint8_t(src[x]);
            else
                std::memcpy(dst + ptrdiff_t(y) * ls, src, size_t(w) * 2);
        }
    }
    const JxsFileInfo& in = s_->info;
    into->color_primaries = AVColorPrimaries(in.primaries);
    into->color_trc = AVColorTransferCharacteristic(in.transfer);
    into->colorspace = p.sampling == JxsSampling::Rgb ? AVCOL_SPC_RGB : AVColorSpace(in.matrix);
    into->color_range = in.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    into->chroma_location = AVCHROMA_LOC_LEFT;
    into->pts = n;
    return true;
}

bool readJxsFileInfo(const std::string& file, JxsFileInfo& out, std::string* error) {
    JxsReader r;
    if (!r.open(file, error)) return false;
    out = r.info();
    return true;
}

// ---- Writing.

struct JxsWriter::State {
    JxsWriteSettings s;
    std::string path;
    JxsEncoder encoder;
    std::vector<uint8_t> bytes;
    int64_t frames = 0;
    bool open = false, finished = false;
    QFile f;  // (a raw codestream)
    SwsContext* sws = nullptr;
    AVFrame* converted = nullptr;
    std::vector<float> sound;  // (MXF: samples waiting for their frame)
#ifdef MONTAGE_WITH_BMX
    std::unique_ptr<bmx::ClipWriter> clip;
    bmx::ClipWriterTrack* pictureTrack = nullptr;
    std::vector<uint32_t> sequence;  // samples a frame, in turn
    bool descriptorFixed = false;
#endif
    void discard() {
#ifdef MONTAGE_WITH_BMX
        if (clip) {
            BmxUse use;
            clip.reset();
        }
#endif
        if (f.isOpen()) f.close();
        if (open && !finished) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path), ec);
        }
        open = false;
    }
};

JxsWriter::JxsWriter() : s_(std::make_unique<State>()) {}
JxsWriter::~JxsWriter() {
    s_->discard();
    sws_freeContext(s_->sws);
    av_frame_free(&s_->converted);
}

int64_t JxsWriter::framesWritten() const { return s_->frames; }
const std::string& JxsWriter::path() const { return s_->path; }

bool JxsWriter::open(const std::string& path, const JxsWriteSettings& settings, std::string* error) {
    State& st = *s_;
    st.discard();
    st.s = settings;
    st.path = path;
    st.frames = 0;
    st.finished = false;
    if (!jpegXsAvailable()) return fail(error, "This build of Montage has no JPEG XS (SVT-JPEG-XS)");
    if (!st.encoder.open(settings.encode, error)) return false;
    if (settings.container == JxsContainer::Codestream) {
        if (settings.channels > 0) return fail(error, "A JPEG XS codestream file holds no sound: write MXF for sound");
        st.f.setFileName(QString::fromStdString(path));
        if (!st.f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail(error, "Could not write " + path);
        st.open = true;
        return true;
    }
#ifndef MONTAGE_WITH_BMX
    return fail(error, "This build of Montage writes no MXF through bmx, so no JPEG XS in MXF");
#else
    using namespace bmx;
    if (settings.channels < 0 || settings.channels > 16) return fail(error, "JPEG XS MXF takes up to 16 sound channels");
    BmxUse use;
    try {
        Utf8MxfFileFactory files;
        const mxfRational rate = {settings.fpsNum, settings.fpsDen};
        st.clip.reset(ClipWriter::OpenNewOP1AClip(OP1A_DEFAULT_FLAVOUR, files.OpenNew(path), rate));
        st.open = true;
        Timecode start(rate, settings.dropFrame);
        int hh = 0, mm = 0, ss = 0, ff = 0;
        if (std::sscanf(settings.timecode.c_str(), "%d:%d:%d%*c%d", &hh, &mm, &ss, &ff) == 4) start.Init(rate, settings.dropFrame, uint8_t(hh), uint8_t(mm), uint8_t(ss), uint8_t(ff));
        st.clip->SetStartTimecode(start);
        if (!settings.clipName.empty()) st.clip->SetClipName(settings.clipName);
        mxfProductVersion version = {0, 0, 0, 0, 0};
        std::sscanf(MONTAGE_VERSION, "%hu.%hu.%hu", &version.major, &version.minor, &version.patch);
        static const mxfUUID productUid = {0x4d, 0x6f, 0x6e, 0x74, 0x61, 0x67, 0x45, 0x8a, 0x9c, 0x31, 0x52, 0x0e, 0x7b, 0x2d, 0x61, 0x10};
        st.clip->SetProductInfo("Montage", "Montage", version, MONTAGE_VERSION, productUid);
        st.pictureTrack = st.clip->CreateTrack(settings.encode.sampling == JxsSampling::Rgb ? JPEGXS_RGBA : JPEGXS_CDCI);
        st.pictureTrack->SetAspectRatio({settings.encode.width, settings.encode.height});
        st.pictureTrack->SetComponentDepth(uint32_t(settings.encode.depth));
        for (int c = 0; c < settings.channels; ++c) {
            ClipWriterTrack* t = st.clip->CreateTrack(WAVE_PCM);
            t->SetSamplingRate({settings.sampleRate, 1});
            t->SetQuantizationBits(uint32_t(settings.bits));
            t->SetChannelCount(1);
            t->SetLocked(true);
            if (c == 0) st.sequence = t->GetShiftedSampleSequence();
        }
        if (auto* pd = dynamic_cast<PictureMXFDescriptorHelper*>(st.pictureTrack->GetMXFDescriptorHelper())) {
            if (const mxfUL* l = labelFor(kPrimaries, settings.primaries)) pd->SetColorPrimaries(*l);
            if (const mxfUL* l = labelFor(kTransfers, settings.transfer)) pd->SetTransferCharacteristic(*l);
            if (settings.encode.sampling != JxsSampling::Rgb)
                if (const mxfUL* l = labelFor(kMatrices, settings.matrix)) pd->SetCodingEquations(*l);
        }
        st.clip->PrepareHeaderMetadata();
        st.clip->PrepareWrite();
        if (settings.channels > 0 && st.sequence.empty()) return fail(error, "No sound sample sequence for this frame rate in MXF");
        return true;
    } catch (const std::exception& e) {
        const std::string why = std::string("JPEG XS MXF: ") + e.what() + (use.log().empty() ? "" : " (" + use.log() + ")");
        st.clip.reset();
        st.discard();
        return fail(error, why);
    }
#endif
}

bool JxsWriter::write(const JxsPicture& p, std::string* error) {
    State& st = *s_;
    if (!st.encoder.encode(p, st.bytes, nullptr, error)) return false;
    return writeCodestream(st.bytes.data(), st.bytes.size(), error);
}

bool JxsWriter::write(const AVFrame* frame, std::string* error) {
    State& st = *s_;
    const JxsEncodeSettings& e = st.s.encode;
    const AVPixelFormat want = pixelFormat(e.sampling, e.depth);
    const AVFrame* f = frame;
    if (frame->format != want || frame->width != e.width || frame->height != e.height) {
        st.sws = sws_getCachedContext(st.sws, frame->width, frame->height, AVPixelFormat(frame->format), e.width, e.height, want, SWS_BICUBIC,
                                      nullptr, nullptr, nullptr);
        if (!st.sws) return fail(error, "Could not convert the picture for JPEG XS");
        if (!st.converted) st.converted = av_frame_alloc();
        av_frame_unref(st.converted);
        st.converted->format = want;
        st.converted->width = e.width;
        st.converted->height = e.height;
        if (av_frame_get_buffer(st.converted, 0) < 0) return fail(error, "Out of memory");
        sws_scale(st.sws, frame->data, frame->linesize, 0, frame->height, st.converted->data, st.converted->linesize);
        f = st.converted;
    }
    JxsPicture p(e.width, e.height, e.depth, e.sampling);
    for (int c = 0; c < 3; ++c) {
        const int w = p.planeWidth(c);
        const uint8_t* src = f->data[avPlane(e.sampling, c)];
        const int ls = f->linesize[avPlane(e.sampling, c)];
        for (int y = 0; y < e.height; ++y) {
            uint16_t* dst = p.planes[c].data() + size_t(y) * size_t(w);
            if (e.depth <= 8)
                for (int x = 0; x < w; ++x) dst[x] = src[ptrdiff_t(y) * ls + x];
            else
                std::memcpy(dst, src + ptrdiff_t(y) * ls, size_t(w) * 2);
        }
    }
    return write(p, error);
}

bool JxsWriter::audio(const float* samples, int frames, std::string* error) {
    State& st = *s_;
    if (st.s.channels <= 0) return frames <= 0 || fail(error, "This JPEG XS file holds no sound");
    st.sound.insert(st.sound.end(), samples, samples + size_t(std::max(frames, 0)) * size_t(st.s.channels));
    return true;
}

bool JxsWriter::writeCodestream(const uint8_t* data, size_t size, std::string* error) {
    State& st = *s_;
    if (!st.open) return fail(error, "The JPEG XS file is not open");
    if (st.s.container == JxsContainer::Codestream) {
        if (st.f.write(reinterpret_cast<const char*>(data), qint64(size)) != qint64(size)) {
            st.discard();
            return fail(error, "Could not write " + st.path);
        }
        ++st.frames;
        return true;
    }
#ifdef MONTAGE_WITH_BMX
    using namespace bmx;
    BmxUse use;
    try {
        st.clip->WriteSamples(0, data, uint32_t(size), 1);
        if (!st.descriptorFixed) {
            // bmx fills the JPEG XS Picture SubDescriptor from the first codestream; its CDCI descriptor's sampling and
            // range are set here, as ST 377-1 has them (bmx's own guess from the profile does not set 4:2:2's).
            st.descriptorFixed = true;
            const JxsEncodeSettings& e = st.s.encode;
            mxfpp::FileDescriptor* fd = st.pictureTrack->GetMXFDescriptorHelper()->GetFileDescriptor();
            const uint32_t d = uint32_t(e.depth);
            if (auto* cd = dynamic_cast<mxfpp::CDCIEssenceDescriptor*>(fd)) {
                cd->setHorizontalSubsampling(e.sampling == JxsSampling::Ycc422 ? 2 : 1);
                cd->setVerticalSubsampling(1);
                cd->setComponentDepth(d);
                cd->setColorSiting(MXF_COLOR_SITING_COSITING);
                cd->setBlackRefLevel(st.s.fullRange ? 0 : 16u << (d - 8));
                cd->setWhiteReflevel(st.s.fullRange ? (1u << d) - 1 : 235u << (d - 8));
                cd->setColorRange(st.s.fullRange ? (1u << d) : (225u << (d - 8)) + 1);
            } else if (auto* rd = dynamic_cast<mxfpp::RGBAEssenceDescriptor*>(fd)) {
                rd->setComponentMinRef(st.s.fullRange ? 0 : 16u << (d - 8));
                rd->setComponentMaxRef(st.s.fullRange ? (1u << d) - 1 : 235u << (d - 8));
            }
            if (fd && fd->haveSubDescriptors())
                for (mxfpp::SubDescriptor* sd : fd->getSubDescriptors())
                    if (auto* x = dynamic_cast<mxfpp::JPEGXSSubDescriptor*>(sd))
                        x->setJPEGXSMaximumBitRate(uint32_t(std::ceil(double(size) * 8 * st.s.fpsNum / st.s.fpsDen / 1e6)));
        }
        if (st.s.channels > 0) {
            const uint32_t n = st.sequence[size_t(st.frames % int64_t(st.sequence.size()))];
            const int ch = st.s.channels;
            const int bytes = (st.s.bits + 7) / 8;
            std::vector<uint8_t> mono(size_t(n) * size_t(bytes));
            for (int c = 0; c < ch; ++c) {
                for (uint32_t i = 0; i < n; ++i) {
                    const size_t at = size_t(i) * size_t(ch) + size_t(c);
                    const float v = at < st.sound.size() ? st.sound[at] : 0.f;
                    const double scale = double(int64_t(1) << (st.s.bits - 1));
                    const int32_t q = int32_t(std::clamp(std::lround(double(v) * scale), long(-scale), long(scale - 1)));
                    for (int b = 0; b < bytes; ++b) mono[size_t(i) * size_t(bytes) + size_t(b)] = uint8_t(uint32_t(q) >> (8 * b));
                }
                st.clip->WriteSamples(uint32_t(1 + c), mono.data(), uint32_t(mono.size()), n);
            }
            st.sound.erase(st.sound.begin(), st.sound.begin() + ptrdiff_t(std::min(st.sound.size(), size_t(n) * size_t(ch))));
        }
        ++st.frames;
        return true;
    } catch (const std::exception& e) {
        const std::string why = std::string("JPEG XS MXF: ") + e.what() + (use.log().empty() ? "" : " (" + use.log() + ")");
        st.clip.reset();
        st.discard();
        return fail(error, why);
    }
#else
    (void)data;
    (void)size;
    return fail(error, "No MXF writer in this build");
#endif
}

bool JxsWriter::finish(std::string* error) {
    State& st = *s_;
    if (!st.open) return fail(error, "The JPEG XS file is not open");
    if (st.frames == 0) {
        st.discard();
        return fail(error, "No pictures were written");
    }
    if (st.s.container == JxsContainer::Codestream) {
        st.f.close();
        st.finished = true;
        st.open = false;
        return true;
    }
#ifdef MONTAGE_WITH_BMX
    {
        BmxUse use;
        try {
            st.clip->CompleteWrite();
            st.clip.reset();
        } catch (const std::exception& e) {
            const std::string why = std::string("JPEG XS MXF: ") + e.what() + (use.log().empty() ? "" : " (" + use.log() + ")");
            st.clip.reset();
            st.discard();
            return fail(error, why);
        }
    }
    st.finished = true;
    st.open = false;
    return true;
#else
    return fail(error, "No MXF writer in this build");
#endif
}

}  // namespace montage
