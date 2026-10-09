#include "Jpeg2000.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#ifdef MONTAGE_WITH_OPENJPEG
#include <openjpeg.h>
#endif

namespace montage {

namespace {

// ISO 15444-1 Amd 8: the most samples per second (all components) at each main level, in millions, and the most
// megabits per second at each sub level; the highest sub level each main level allows.
constexpr double kMainLevelMsps[] = {0, 65, 130, 195, 260, 520, 1200, 2400, 4800, 9600, 19200, 38400};
constexpr double kSubLevelMbps[] = {0, 200, 400, 800, 1600, 3200, 6400, 12800, 25600, 51200};
constexpr int kMaxSubLevel[] = {0, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9};

}  // namespace

bool openJpegAvailable() {
#ifdef MONTAGE_WITH_OPENJPEG
    return true;
#else
    return false;
#endif
}

uint16_t imfRsiz(int width, int height, int components, double fps, bool lossless, double megabitsPerSecond) {
    if (width <= 0 || height <= 0 || components <= 0 || !(fps > 0)) return 0;
    uint16_t profile;
    if (width <= 2048 && height <= 1556) profile = lossless ? 0x0700 : 0x0400;
    else if (width <= 4096 && height <= 3112) profile = lossless ? 0x0800 : 0x0500;
    else if (width <= 8192 && height <= 6224) profile = lossless ? 0x0900 : 0x0600;
    else return 0;
    const double msps = double(width) * height * components * fps / 1e6;
    int main = 1;
    while (main <= 11 && msps > kMainLevelMsps[main]) ++main;
    if (main > 11) return 0;
    int sub = 0;
    if (!lossless) {
        sub = 1;
        while (sub <= 9 && megabitsPerSecond > kSubLevelMbps[sub]) ++sub;
        if (sub > 9) return 0;
        while (main <= 11 && sub > kMaxSubLevel[main]) ++main;  // a higher rate needs a higher main level
        if (main > 11) return 0;
    }
    return uint16_t(profile | (sub << 4) | main);
}

void imfPictureCoding(uint16_t rsiz, uint8_t out[16]) {
    std::memset(out, 0, 16);
    const int profile = rsiz & 0xff00, main = rsiz & 0xf, sub = (rsiz >> 4) & 0xf;
    uint8_t kind;
    switch (profile) {
        case 0x0400: kind = 0x02; break;  // 2K lossy
        case 0x0500: kind = 0x03; break;  // 4K lossy
        case 0x0700: kind = 0x05; break;  // 2K reversible
        case 0x0800: kind = 0x06; break;  // 4K reversible
        default: return;
    }
    // The level byte counts on through the sub levels each main level allows.
    static const int base[] = {0, 0x02, 0x04, 0x06, 0x08, 0x0b, 0x0f, 0x14, 0x1a};
    if (main < 1 || main > 8 || sub > kMaxSubLevel[main]) return;
    const uint8_t ul[16] = {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x0d, 0x04, 0x01, 0x02, 0x02, 0x03, 0x01, kind,
                            uint8_t(base[main] + sub)};
    std::memcpy(out, ul, 16);
}

#ifdef MONTAGE_WITH_OPENJPEG

namespace {

struct Buffer {
    std::vector<uint8_t>* out = nullptr;  // writing
    const uint8_t* in = nullptr;          // reading
    size_t size = 0;
    size_t at = 0;
};

OPJ_SIZE_T writeFn(void* data, OPJ_SIZE_T n, void* user) {
    auto* b = static_cast<Buffer*>(user);
    if (b->out->size() < b->at + n) b->out->resize(b->at + n);
    std::memcpy(b->out->data() + b->at, data, n);
    b->at += n;
    return n;
}
OPJ_SIZE_T readFn(void* data, OPJ_SIZE_T n, void* user) {
    auto* b = static_cast<Buffer*>(user);
    if (b->at >= b->size) return OPJ_SIZE_T(-1);
    n = std::min<OPJ_SIZE_T>(n, b->size - b->at);
    std::memcpy(data, b->in + b->at, n);
    b->at += n;
    return n;
}
OPJ_OFF_T skipFn(OPJ_OFF_T n, void* user) {
    auto* b = static_cast<Buffer*>(user);
    const size_t limit = b->out ? SIZE_MAX : b->size;
    if (n < 0 && size_t(-n) > b->at) return -1;
    if (n > 0 && b->at + size_t(n) > limit) n = OPJ_OFF_T(limit - b->at);
    b->at = size_t(OPJ_OFF_T(b->at) + n);
    if (b->out && b->out->size() < b->at) b->out->resize(b->at);
    return n;
}
OPJ_BOOL seekFn(OPJ_OFF_T n, void* user) {
    auto* b = static_cast<Buffer*>(user);
    if (n < 0 || (!b->out && size_t(n) > b->size)) return OPJ_FALSE;
    b->at = size_t(n);
    if (b->out && b->out->size() < b->at) b->out->resize(b->at);
    return OPJ_TRUE;
}

// OpenJPEG's messages for one call, the errors kept for the caller.
struct Messages {
    std::string errors;
    static void error(const char* msg, void* user) { static_cast<Messages*>(user)->errors += msg; }
    static void quiet(const char*, void*) {}
};

}  // namespace

bool encodeJpeg2000(const uint16_t* rgb, int width, int height, int bits, uint16_t rsiz, size_t maxBytes, std::vector<uint8_t>& out,
                    std::string* error) {
    if (!rgb || width <= 0 || height <= 0 || bits < 8 || bits > 16) {
        if (error) *error = "Nothing to encode";
        return false;
    }
    opj_image_cmptparm_t parms[3];
    std::memset(parms, 0, sizeof parms);
    for (auto& c : parms) {
        c.dx = c.dy = 1;
        c.w = OPJ_UINT32(width);
        c.h = OPJ_UINT32(height);
        c.prec = OPJ_UINT32(bits);
        c.sgnd = 0;
    }
    opj_image_t* image = opj_image_create(3, parms, OPJ_CLRSPC_SRGB);
    if (!image) {
        if (error) *error = "Out of memory";
        return false;
    }
    image->x0 = image->y0 = 0;
    image->x1 = OPJ_UINT32(width);
    image->y1 = OPJ_UINT32(height);
    const size_t n = size_t(width) * size_t(height);
    for (int c = 0; c < 3; ++c) {
        OPJ_INT32* d = image->comps[c].data;
        for (size_t i = 0; i < n; ++i) d[i] = rgb[i * 3 + size_t(c)];
    }
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.rsiz = rsiz;
    // The DCI cinema profiles (Rsiz 3 and 4) take X'Y'Z' as it is, 9-7 coded, each frame and component capped; the IMF
    // ones RGB through the component transform.
    const bool cinema = rsiz == 0x0003 || rsiz == 0x0004;
    p.tcp_mct = cinema ? 0 : 1;
    const bool lossless = !cinema && (rsiz & 0xff00) >= 0x0700;
    p.irreversible = lossless ? 0 : 1;
    p.tcp_numlayers = 1;
    p.tcp_rates[0] = 0;
    p.cp_disto_alloc = 1;
    if (!lossless && maxBytes > 0) p.max_cs_size = int(std::min<size_t>(maxBytes, size_t(INT32_MAX)));
    if (cinema && maxBytes > 0) p.max_comp_size = int(std::min<size_t>(maxBytes / 5 * 4, size_t(INT32_MAX)));
    // OpenJPEG sets the profiles' coding itself for an IMF Rsiz (32 x 32 code-blocks, CPRL, 128 x 128 precincts at the
    // lowest resolution and 256 x 256 above, the decompositions the profile allows) and checks it: a picture it cannot
    // code in the profile comes out without it, which the caller sees in the Rsiz.
    Messages msgs;
    opj_codec_t* codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_error_handler(codec, Messages::error, &msgs);
    opj_set_warning_handler(codec, Messages::quiet, nullptr);
    opj_set_info_handler(codec, Messages::quiet, nullptr);
    bool ok = codec && opj_setup_encoder(codec, &p, image);
    out.clear();
    Buffer buf;
    buf.out = &out;
    opj_stream_t* stream = ok ? opj_stream_create(1 << 20, OPJ_FALSE) : nullptr;
    if (stream) {
        opj_stream_set_user_data(stream, &buf, nullptr);
        opj_stream_set_write_function(stream, writeFn);
        opj_stream_set_skip_function(stream, skipFn);
        opj_stream_set_seek_function(stream, seekFn);
        ok = opj_start_compress(codec, image, stream) && opj_encode(codec, stream) && opj_end_compress(codec, stream);
        opj_stream_destroy(stream);
    } else {
        ok = false;
    }
    if (codec) opj_destroy_codec(codec);
    opj_image_destroy(image);
    out.resize(buf.at);
    if (!ok && error) *error = msgs.errors.empty() ? "JPEG 2000 encoding failed" : "JPEG 2000: " + msgs.errors;
    return ok;
}

bool decodeJpeg2000(const uint8_t* data, size_t size, std::vector<uint16_t>& samples, int& width, int& height, int& components, int& bits,
                    std::string* error) {
    Messages msgs;
    opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_set_error_handler(codec, Messages::error, &msgs);
    opj_set_warning_handler(codec, Messages::quiet, nullptr);
    opj_set_info_handler(codec, Messages::quiet, nullptr);
    opj_dparameters_t p;
    opj_set_default_decoder_parameters(&p);
    Buffer buf;
    buf.in = data;
    buf.size = size;
    opj_image_t* image = nullptr;
    opj_stream_t* stream = opj_stream_create(1 << 20, OPJ_TRUE);
    opj_stream_set_user_data(stream, &buf, nullptr);
    opj_stream_set_user_data_length(stream, size);
    opj_stream_set_read_function(stream, readFn);
    opj_stream_set_skip_function(stream, skipFn);
    opj_stream_set_seek_function(stream, seekFn);
    bool ok = opj_setup_decoder(codec, &p) && opj_read_header(stream, codec, &image) && opj_decode(codec, stream, image) &&
              opj_end_decompress(codec, stream);
    ok = ok && image && image->numcomps >= 1 && image->numcomps <= 4;
    if (ok) {
        width = int(image->comps[0].w);
        height = int(image->comps[0].h);
        components = int(image->numcomps);
        bits = int(image->comps[0].prec);
        for (int c = 1; c < components; ++c)
            if (image->comps[c].w != image->comps[0].w || image->comps[c].h != image->comps[0].h) ok = false;
    }
    if (ok) {
        const size_t n = size_t(width) * size_t(height);
        samples.resize(n * size_t(components));
        for (int c = 0; c < components; ++c)
            for (size_t i = 0; i < n; ++i) samples[i * size_t(components) + size_t(c)] = uint16_t(std::clamp(image->comps[c].data[i], 0, 65535));
    }
    if (image) opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    if (!ok && error) *error = msgs.errors.empty() ? "Not a JPEG 2000 codestream" : "JPEG 2000: " + msgs.errors;
    return ok;
}

#else

bool encodeJpeg2000(const uint16_t*, int, int, int, uint16_t, size_t, std::vector<uint8_t>&, std::string* error) {
    if (error) *error = "This build has no OpenJPEG";
    return false;
}
bool decodeJpeg2000(const uint8_t*, size_t, std::vector<uint16_t>&, int&, int&, int&, int&, std::string* error) {
    if (error) *error = "This build has no OpenJPEG";
    return false;
}

#endif

}  // namespace montage
