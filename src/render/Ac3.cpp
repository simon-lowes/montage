#include "Ac3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace montage {

namespace {

// CRC-16 as A/52 clause 6.10.1 gives it (x^16 + x^15 + x^2 + 1, most significant bit first, from 0).
uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = crc & 0x8000 ? uint16_t((crc << 1) ^ 0x8005) : uint16_t(crc << 1);
    }
    return crc;
}

class BitReader {
public:
    BitReader(const uint8_t* d, size_t bytes) : d_(d), bits_(bytes * 8) {}
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (pos_ >= bits_) {
                over_ = true;
                return v;
            }
            v = v << 1 | ((d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1);
            ++pos_;
        }
        return v;
    }
    bool flag() { return get(1) != 0; }
    void skip(size_t n) { pos_ += n; over_ = over_ || pos_ > bits_; }
    size_t pos() const { return pos_; }
    bool over() const { return over_; }

private:
    const uint8_t* d_;
    size_t bits_, pos_ = 0;
    bool over_ = false;
};

class BitWriter {
public:
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bit((v >> i) & 1);
    }
    void copy(const uint8_t* src, size_t from, size_t to) {
        for (size_t p = from; p < to; ++p) bit((src[p >> 3] >> (7 - (p & 7))) & 1);
    }
    size_t bits() const { return bits_; }
    std::vector<uint8_t> bytes;

private:
    void bit(unsigned b) {
        if ((bits_ & 7) == 0) bytes.push_back(0);
        if (b) bytes.back() |= uint8_t(0x80 >> (bits_ & 7));
        ++bits_;
    }
    size_t bits_ = 0;
};

// AC-3's levels by code (dB; -99 for none): cmixlev and surmixlev (2 bits), and the 3-bit Lt/Rt and Lo/Ro ones.
constexpr double kNone = -99;
const double kCmix2[4] = {-3, -4.5, -6, -4.5};  // (3 reserved: -4.5, as decoders take it)
const double kSurmix2[4] = {-3, -6, kNone, -6};
const double kCmix3[8] = {3, 1.5, 0, -1.5, -3, -4.5, -6, kNone};
const double kSurmix3[8] = {-1.5, -1.5, -1.5, -1.5, -3, -4.5, -6, kNone};  // (0-2 reserved: -1.5)

double gainOf(double db) { return db <= kNone + 1 ? 0.0 : std::pow(10.0, db / 20); }

// Bit allocation (A/52 clause 7.2.2), the tables of its clause 7.2.3.
const uint8_t kSlowDec[4] = {0x0f, 0x11, 0x13, 0x15};
const uint8_t kFastDec[4] = {0x3f, 0x53, 0x67, 0x7b};
const uint16_t kSlowGain[4] = {0x540, 0x4d8, 0x478, 0x410};
const uint16_t kDbPerBit[4] = {0x000, 0x700, 0x900, 0xb00};
const int16_t kFloor[8] = {0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, int16_t(-2048)};  // (0xf800)
const uint16_t kFastGain[8] = {0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400};
const uint8_t kBandStart[51] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                                26, 27, 28, 31, 34, 37, 40, 43, 46, 49, 55, 61, 67, 73, 79, 85, 97, 109, 121, 133, 157, 181, 205, 229, 253};
const uint8_t kLogAdd[260] = {
    0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x34, 0x33, 0x32, 0x31, 0x30, 0x2f, 0x2f,
    0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x29, 0x29, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x21, 0x21,
    0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16,
    0x16, 0x15, 0x15, 0x15, 0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f,
    0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};  // (from 210 on, 0)
const uint16_t kHearing[50][3] = {
    {0x4d0, 0x4f0, 0x580}, {0x4d0, 0x4f0, 0x580}, {0x440, 0x460, 0x4b0}, {0x400, 0x410, 0x450}, {0x3e0, 0x3e0, 0x420},
    {0x3c0, 0x3d0, 0x3f0}, {0x3b0, 0x3c0, 0x3e0}, {0x3b0, 0x3b0, 0x3d0}, {0x3a0, 0x3b0, 0x3c0}, {0x3a0, 0x3a0, 0x3b0},
    {0x3a0, 0x3a0, 0x3b0}, {0x3a0, 0x3a0, 0x3b0}, {0x3a0, 0x3a0, 0x3a0}, {0x390, 0x3a0, 0x3a0}, {0x390, 0x390, 0x3a0},
    {0x390, 0x390, 0x3a0}, {0x380, 0x390, 0x3a0}, {0x380, 0x380, 0x3a0}, {0x370, 0x380, 0x3a0}, {0x370, 0x380, 0x3a0},
    {0x360, 0x370, 0x390}, {0x360, 0x370, 0x390}, {0x350, 0x360, 0x390}, {0x350, 0x360, 0x390}, {0x340, 0x350, 0x380},
    {0x340, 0x350, 0x380}, {0x330, 0x340, 0x380}, {0x320, 0x340, 0x370}, {0x310, 0x320, 0x360}, {0x300, 0x310, 0x350},
    {0x2f0, 0x300, 0x340}, {0x2f0, 0x2f0, 0x330}, {0x2f0, 0x2f0, 0x320}, {0x2f0, 0x2f0, 0x310}, {0x300, 0x2f0, 0x300},
    {0x310, 0x300, 0x2f0}, {0x340, 0x320, 0x2f0}, {0x390, 0x350, 0x2f0}, {0x3e0, 0x390, 0x300}, {0x420, 0x3e0, 0x310},
    {0x460, 0x420, 0x330}, {0x490, 0x450, 0x350}, {0x4a0, 0x4a0, 0x3c0}, {0x460, 0x490, 0x410}, {0x440, 0x460, 0x470},
    {0x440, 0x440, 0x4a0}, {0x520, 0x480, 0x460}, {0x800, 0x630, 0x440}, {0x840, 0x840, 0x450}, {0x840, 0x840, 0x4e0}};
const uint8_t kBap[64] = {0,  1,  1,  1,  1,  1,  2,  2,  3,  3,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,
                          7,  8,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13,
                          13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15};
// Bits per mantissa by bap (1, 2 and 4 are grouped and counted apart).
const uint8_t kBapBits[16] = {0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};

int bandOf(int bin) {
    int b = 0;
    while (b < 49 && kBandStart[b + 1] <= bin) ++b;
    return b;
}

int logAdd(int a, int b) {
    const int c = a - b;
    const int address = std::min(std::abs(c) >> 1, 255);
    const int v = address < 260 ? kLogAdd[address] : 0;
    return c >= 0 ? a + v : b + v;
}

int lowComp(int a, int b0, int b1, int bin) {
    if (bin < 7) {
        if (b0 + 256 == b1) a = 384;
        else if (b0 > b1) a = std::max(0, a - 64);
    } else if (bin < 20) {
        if (b0 + 256 == b1) a = 320;
        else if (b0 > b1) a = std::max(0, a - 64);
    } else {
        a = std::max(0, a - 128);
    }
    return a;
}

struct BitAllocation {
    int sdecay = 0, fdecay = 0, sgain = 0, dbknee = 0, floor = 0, fscod = 0;
};

// The bit allocation pointers of one channel's bins [0, end) (fbw or LFE; no coupling).
void allocate(const BitAllocation& ba, const int* exp, int end, int fgain, int snroffset, bool lfe, uint8_t* bap) {
    int psd[256];
    int bndpsd[50] = {0};
    int excite[50] = {0};
    int mask[50] = {0};
    for (int bin = 0; bin < end; ++bin) psd[bin] = 3072 - (exp[bin] << 7);
    // Integration into bands.
    {
        int j = 0, k = bandOf(0), lastbin = 0;
        do {
            lastbin = std::min<int>(kBandStart[k + 1], end);
            bndpsd[k] = psd[j++];
            for (; j < lastbin; ++j) bndpsd[k] = logAdd(bndpsd[k], psd[j]);
            ++k;
        } while (end > lastbin);
    }
    const int bndstrt = bandOf(0), bndend = bandOf(end - 1) + 1;
    int lowcomp = 0, fastleak = 0, slowleak = 0, begin = 0;
    {
        lowcomp = lowComp(lowcomp, bndpsd[0], bndpsd[1], 0);
        excite[0] = bndpsd[0] - fgain - lowcomp;
        lowcomp = lowComp(lowcomp, bndpsd[1], bndpsd[2], 1);
        excite[1] = bndpsd[1] - fgain - lowcomp;
        begin = 7;
        for (int bin = 2; bin < 7; ++bin) {
            if (!(lfe && bin == 6)) lowcomp = lowComp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = bndpsd[bin] - fgain;
            slowleak = bndpsd[bin] - ba.sgain;
            excite[bin] = fastleak - lowcomp;
            if (!(lfe && bin == 6) && bndpsd[bin] <= bndpsd[bin + 1]) {
                begin = bin + 1;
                break;
            }
        }
        for (int bin = begin; bin < std::min(bndend, 22); ++bin) {
            if (!(lfe && bin == 6)) lowcomp = lowComp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = std::max(fastleak - ba.fdecay, bndpsd[bin] - fgain);
            slowleak = std::max(slowleak - ba.sdecay, bndpsd[bin] - ba.sgain);
            excite[bin] = std::max(fastleak - lowcomp, slowleak);
        }
        begin = 22;
    }
    for (int bin = begin; bin < bndend; ++bin) {
        fastleak = std::max(fastleak - ba.fdecay, bndpsd[bin] - fgain);
        slowleak = std::max(slowleak - ba.sdecay, bndpsd[bin] - ba.sgain);
        excite[bin] = std::max(fastleak, slowleak);
    }
    for (int bin = bndstrt; bin < bndend; ++bin) {
        if (bndpsd[bin] < ba.dbknee) excite[bin] += (ba.dbknee - bndpsd[bin]) >> 2;
        mask[bin] = std::max<int>(excite[bin], kHearing[bin][std::min(ba.fscod, 2)]);
    }
    int i = 0, j = bndstrt, lastbin = 0;
    do {
        lastbin = std::min<int>(kBandStart[j + 1], end);
        int m = mask[j] - snroffset - ba.floor;
        if (m < 0) m = 0;
        m &= 0x1fe0;
        m += ba.floor;
        for (; i < lastbin; ++i) bap[i] = kBap[std::clamp((psd[i] - m) >> 5, 0, 63)];
        ++j;
    } while (end > lastbin);
}

// The frame exponent strategies (Table E.1.9): 1 D15, 2 D25, 3 D45, 0 reuse.
const uint8_t kFrameExp[32][6] = {
    {1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 3}, {1, 0, 0, 0, 2, 0}, {1, 0, 0, 0, 3, 3}, {2, 0, 0, 2, 0, 0}, {2, 0, 0, 2, 0, 3},
    {2, 0, 0, 3, 2, 0}, {2, 0, 0, 3, 3, 3}, {2, 0, 1, 0, 0, 0}, {2, 0, 2, 0, 0, 3}, {2, 0, 2, 0, 2, 0}, {2, 0, 2, 0, 3, 3},
    {2, 0, 3, 2, 0, 0}, {2, 0, 3, 2, 0, 3}, {2, 0, 3, 3, 2, 0}, {2, 0, 3, 3, 3, 3}, {3, 1, 0, 0, 0, 0}, {3, 1, 0, 0, 0, 3},
    {3, 2, 0, 0, 2, 0}, {3, 2, 0, 0, 3, 3}, {3, 2, 0, 2, 0, 0}, {3, 2, 0, 2, 0, 3}, {3, 2, 0, 3, 2, 0}, {3, 2, 0, 3, 3, 3},
    {3, 3, 1, 0, 0, 0}, {3, 3, 2, 0, 0, 3}, {3, 3, 2, 0, 2, 0}, {3, 3, 2, 0, 3, 3}, {3, 3, 3, 2, 0, 0}, {3, 3, 3, 2, 0, 3},
    {3, 3, 3, 3, 2, 0}, {3, 3, 3, 3, 3, 3}};

const int kChannels[8] = {2, 1, 2, 3, 3, 4, 4, 5};  // full-bandwidth channels by acmod (1+1 counts two)

bool failWith(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

int ac3Words(int fscod, int frmsizecod) {
    static const int kbps[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
    if (frmsizecod / 2 >= 19) return 0;
    const int rate = kbps[frmsizecod / 2];
    if (fscod == 0) return rate * 2;
    if (fscod == 2) return rate * 3;
    if (fscod == 1) return int(rate * 1000LL * 1536 / 44100 / 16) + (frmsizecod & 1);
    return 0;
}

}  // namespace

Ac3FrameInfo::Ac3FrameInfo() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    centreMixDb = surroundMixDb = ltrtCentreMixDb = ltrtSurroundMixDb = nan;
}

int ac3Dialnorm(double lkfs) {
    if (!std::isfinite(lkfs)) return -31;
    return int(std::clamp<long>(std::lround(lkfs), -31, -1));
}

void ac3EncoderOptions(const Ac3Metadata& m, bool eac3, bool centre, bool surround, AVDictionary** opts) {
    av_dict_set(opts, "per_frame_metadata", "1", 0);
    av_dict_set_int(opts, "dialnorm", std::clamp(m.dialnorm, -31, -1), 0);
    // Levels as linear gains (each encoder takes the nearest its syntax has).
    auto level = [](double db) {
        char b[32];
        std::snprintf(b, sizeof b, "%.4f", gainOf(db));
        return std::string(b);
    };
    if (!eac3) {
        // A/52's own fields (bsid 8): cmixlev -3, -4.5 or -6 dB; surmixlev -3, -6 dB or none.
        if (centre) av_dict_set(opts, "center_mixlev", level(m.centreMixDb).c_str(), 0);
        if (surround) av_dict_set(opts, "surround_mixlev", level(m.surroundMixDb).c_str(), 0);
    }
    if (centre || surround) {
        // The Lo/Ro and Lt/Rt levels (A/52 Annex D's extended syntax for AC-3, E-AC-3's mixing metadata), the same
        // levels both ways, and the downmix preferred.
        if (centre) {
            av_dict_set(opts, "loro_cmixlev", level(m.centreMixDb).c_str(), 0);
            av_dict_set(opts, "ltrt_cmixlev", level(m.centreMixDb).c_str(), 0);
        }
        if (surround) {
            av_dict_set(opts, "loro_surmixlev", level(m.surroundMixDb).c_str(), 0);
            av_dict_set(opts, "ltrt_surmixlev", level(m.surroundMixDb).c_str(), 0);
        }
        av_dict_set(opts, "dmix_mode", m.preferredDownmix == "ltrt" ? "ltrt" : m.preferredDownmix == "loro" ? "loro" : "notindicated", 0);
    }
    if (m.mixingLevel >= 80 && m.mixingLevel <= 111) {
        av_dict_set_int(opts, "mixing_level", m.mixingLevel, 0);
        av_dict_set(opts, "room_type", m.roomType == "large" ? "large" : m.roomType == "small" ? "small" : "notindicated", 0);
    }
}

void ac3SetDialnorm(AVCodecContext* encoder, int dialnorm) {
    if (encoder && encoder->priv_data) av_opt_set_int(encoder->priv_data, "dialnorm", std::clamp(dialnorm, -31, -1), 0);
}

std::vector<Ac3FrameInfo> parseAc3Frames(const uint8_t* data, size_t size) {
    std::vector<Ac3FrameInfo> out;
    size_t at = 0;
    while (at + 8 <= size && data[at] == 0x0b && data[at + 1] == 0x77) {
        const uint8_t* d = data + at;
        Ac3FrameInfo f;
        const int bsid = (d[5] >> 3) & 31;
        BitReader r(d, size - at);
        r.skip(16);
        f.bsid = bsid;
        if (bsid <= 10) {
            r.skip(16);  // crc1
            const int fscod = int(r.get(2)), frmsizecod = int(r.get(6));
            f.sampleRate = fscod == 0 ? 48000 : fscod == 1 ? 44100 : fscod == 2 ? 32000 : 0;
            f.bytes = ac3Words(fscod, frmsizecod) * 2;
            r.skip(5);  // bsid
            r.skip(3);  // bsmod
            f.acmod = int(r.get(3));
            if ((f.acmod & 1) && f.acmod != 1) f.centreMixDb = kCmix2[r.get(2)];
            if (f.acmod & 4) f.surroundMixDb = kSurmix2[r.get(2)];
            if (f.acmod == 2) r.skip(2);
            f.lfe = r.flag();
            f.dialnorm = -int(r.get(5));
            if (f.dialnorm == 0) f.dialnorm = -31;
            if (r.flag()) r.skip(8);  // compr
            if (r.flag()) r.skip(8);  // langcod
            if (r.flag()) {
                f.mixingLevel = int(r.get(5)) + 80;
                f.roomType = int(r.get(2));
            }
            if (f.acmod == 0) {
                r.skip(5);
                if (r.flag()) r.skip(8);
                if (r.flag()) r.skip(8);
                if (r.flag()) r.skip(7);
            }
            r.skip(2);  // copyrightb, origbs
            if (bsid == 6 && r.flag()) {
                // Annex D's extended bit stream information: the Lt/Rt and Lo/Ro levels (Lo/Ro stand for the levels
                // a stereo downmix takes, as decoders read them).
                f.preferredDownmix = int(r.get(2));
                f.ltrtCentreMixDb = kCmix3[r.get(3)];
                f.ltrtSurroundMixDb = kSurmix3[r.get(3)];
                f.centreMixDb = kCmix3[r.get(3)];
                f.surroundMixDb = kSurmix3[r.get(3)];
                if (!((f.acmod & 1) && f.acmod != 1)) f.centreMixDb = f.ltrtCentreMixDb = std::numeric_limits<double>::quiet_NaN();
                if (!(f.acmod & 4)) f.surroundMixDb = f.ltrtSurroundMixDb = std::numeric_limits<double>::quiet_NaN();
            }
        } else {
            f.eac3 = true;
            f.streamType = int(r.get(2));
            f.substream = int(r.get(3));
            f.bytes = (int(r.get(11)) + 1) * 2;
            const int fscod = int(r.get(2));
            int fscod2 = -1;
            if (fscod == 3) fscod2 = int(r.get(2));
            else r.skip(2);  // numblkscod
            f.sampleRate = fscod == 0 ? 48000 : fscod == 1 ? 44100 : fscod == 2 ? 32000 : fscod2 == 0 ? 24000 : fscod2 == 1 ? 22050 : 16000;
            f.acmod = int(r.get(3));
            f.lfe = r.flag();
            r.skip(5);  // bsid
            f.dialnorm = -int(r.get(5));
            if (f.dialnorm == 0) f.dialnorm = -31;
            if (r.flag()) r.skip(8);
            if (f.acmod == 0) {
                r.skip(5);
                if (r.flag()) r.skip(8);
            }
            if (f.streamType == 1 && r.flag()) f.chanmap = int(r.get(16));
            if (r.flag()) {  // mixmdate
                if (f.acmod > 2) f.preferredDownmix = int(r.get(2));
                if ((f.acmod & 1) && f.acmod > 2) {
                    f.ltrtCentreMixDb = kCmix3[r.get(3)];
                    f.centreMixDb = kCmix3[r.get(3)];
                }
                if (f.acmod & 4) {
                    f.ltrtSurroundMixDb = kSurmix3[r.get(3)];
                    f.surroundMixDb = kSurmix3[r.get(3)];
                }
                if (f.lfe && r.flag()) r.skip(5);
                if (f.streamType == 0) {
                    if (r.flag()) r.skip(6);
                    if (f.acmod == 0 && r.flag()) r.skip(6);
                    if (r.flag()) r.skip(6);
                    const int mixdef = int(r.get(2));
                    if (mixdef == 1) r.skip(5);
                    else if (mixdef == 2) r.skip(12);
                    else if (mixdef == 3) r.skip(size_t(8 * (r.get(5) + 2)));  // (mixdeflen: its whole length, roughly; past here only info)
                    if (f.acmod < 2) {
                        if (r.flag()) r.skip(14);
                        if (f.acmod == 0 && r.flag()) r.skip(14);
                    }
                    if (r.flag()) r.skip(6 * 6);  // (frmmixcfginfo: six blocks' worth at most; nothing after it is read but info)
                }
            }
            if (r.flag()) {  // infomdate
                r.skip(3 + 2);
                if (f.acmod == 2) r.skip(4);
                if (f.acmod >= 6) r.skip(2);
                if (r.flag()) {
                    f.mixingLevel = int(r.get(5)) + 80;
                    f.roomType = int(r.get(2));
                    r.skip(1);
                }
            }
        }
        if (f.bytes <= 0 || at + size_t(f.bytes) > size) break;
        f.crcOk = crc16(d + 2, size_t(f.bytes) - 2) == 0;
        out.push_back(f);
        at += size_t(f.bytes);
    }
    return out;
}

bool eac3MakeDependent(std::vector<uint8_t>& frame, uint16_t chanmap, std::string* error) {
    const uint8_t* d = frame.data();
    if (frame.size() < 8 || d[0] != 0x0b || d[1] != 0x77) return failWith(error, "not a syncframe");
    BitReader r(d, frame.size());
    r.skip(16);
    const int strmtyp = int(r.get(2)), substreamid = int(r.get(3));
    const size_t bytes = (size_t(r.get(11)) + 1) * 2;
    if (strmtyp != 0 || substreamid != 0) return failWith(error, "not independent substream 0");
    if (bytes != frame.size()) return failWith(error, "not one whole syncframe");
    if (crc16(d + 2, bytes - 2) != 0) return failWith(error, "its CRC does not check");
    const int fscod = int(r.get(2));
    if (fscod == 3) return failWith(error, "half sample rates are not rewritten");
    const int numblkscod = int(r.get(2));
    if (numblkscod != 3) return failWith(error, "not six blocks a frame");
    const int acmod = int(r.get(3));
    const bool lfeon = r.flag();
    if (r.get(5) != 16) return failWith(error, "not E-AC-3");
    if (acmod == 0) return failWith(error, "dual mono is not rewritten");
    r.skip(5);  // dialnorm
    if (r.flag()) r.skip(8);
    const size_t insertAt = r.pos();  // chanmape and chanmap go here
    // What only an independent substream carries, to leave out: [from, to) bit ranges.
    std::vector<std::pair<size_t, size_t>> cuts;
    const int nfchans = kChannels[acmod];
    if (r.flag()) {  // mixmdate
        if (acmod > 2) r.skip(2);
        if ((acmod & 1) && acmod > 2) r.skip(6);
        if (acmod & 4) r.skip(6);
        if (lfeon && r.flag()) r.skip(5);
        const size_t from = r.pos();
        if (r.flag()) r.skip(6);
        if (r.flag()) r.skip(6);
        if (r.get(2) != 0) return failWith(error, "mixing data is not rewritten");
        if (acmod < 2 && r.flag()) r.skip(14);
        if (r.flag()) return failWith(error, "mixing configuration is not rewritten");
        cuts.emplace_back(from, r.pos());
    }
    if (r.flag()) {  // infomdate
        r.skip(5);
        if (acmod == 2) r.skip(4);
        if (acmod >= 6) r.skip(2);
        if (r.flag()) r.skip(8);
        if (fscod < 3) r.skip(1);
    }
    if (r.flag()) r.skip(size_t(r.get(6) + 1) * 8);  // addbsi
    // audfrm
    const bool expstre = r.flag();
    if (r.flag()) return failWith(error, "the adaptive hybrid transform is not rewritten");
    if (r.get(2) != 0) return failWith(error, "SNR offset strategies other than the first are not rewritten");
    if (r.flag() || r.flag() || r.flag() || r.flag() || r.flag() || r.flag() || r.flag() || r.flag())
        return failWith(error, "the encoder used syntax that is not rewritten (transient processing, block switching, dither, bit "
                               "allocation, fast gains, delta bit allocation, skip fields or spectral extension attenuation)");
    if (acmod > 1) {
        if (r.flag()) return failWith(error, "coupling is not rewritten");
        for (int blk = 1; blk < 6; ++blk)
            if (r.flag() && r.flag()) return failWith(error, "coupling is not rewritten");
    }
    int chexpstr[6][5] = {};
    if (expstre) {
        for (int blk = 0; blk < 6; ++blk)
            for (int ch = 0; ch < nfchans; ++ch) chexpstr[blk][ch] = int(r.get(2));
    } else {
        for (int ch = 0; ch < nfchans; ++ch) {
            const int s = int(r.get(5));
            for (int blk = 0; blk < 6; ++blk) chexpstr[blk][ch] = kFrameExp[s][blk];
        }
    }
    int lfeexpstr[6] = {};
    if (lfeon)
        for (int blk = 0; blk < 6; ++blk) lfeexpstr[blk] = int(r.get(1));
    {
        const size_t from = r.pos();  // convexpstr (convexpstre is 1 with six blocks)
        r.skip(size_t(5 * nfchans));
        cuts.emplace_back(from, r.pos());
    }
    const int csnroffst = int(r.get(6)), fsnroffst = int(r.get(4));
    if (r.flag()) return failWith(error, "block start information is not rewritten");
    BitAllocation ba;
    ba.sdecay = kSlowDec[2], ba.fdecay = kFastDec[1], ba.sgain = kSlowGain[1], ba.dbknee = kDbPerBit[2], ba.floor = kFloor[7], ba.fscod = fscod;
    const int snroffset = (((csnroffst - 15) << 4) + fsnroffst) << 2;
    const bool silent = csnroffst == 0 && fsnroffst == 0;
    int exps[6][256] = {};
    int endmant[6] = {};
    int lfeexps[7] = {};
    for (int blk = 0; blk < 6; ++blk) {
        if (r.flag()) r.skip(8);  // dynrng
        if (blk == 0 || r.flag())  // spxstre
            if (r.flag()) return failWith(error, "spectral extension is not rewritten");
        if (acmod == 2 && (blk == 0 || r.flag())) r.skip(4);  // rematflg (four bands without coupling)
        for (int ch = 0; ch < nfchans; ++ch)
            if (chexpstr[blk][ch] != 0) {
                const int chbwcod = int(r.get(6));
                if (chbwcod > 60) return failWith(error, "a bandwidth code out of range");
                endmant[ch] = (chbwcod + 12) * 3 + 37;
            } else if (blk == 0) {
                return failWith(error, "exponents reused in the first block");
            }
        for (int ch = 0; ch < nfchans; ++ch) {
            const int s = chexpstr[blk][ch];
            if (s == 0) continue;
            const int end = endmant[ch];
            const int grpsize = s == 1 ? 1 : s == 2 ? 2 : 4;
            const int ngrps = s == 1 ? (end - 1) / 3 : s == 2 ? (end - 1 + 3) / 6 : (end - 1 + 9) / 12;
            int prev = int(r.get(4));
            exps[ch][0] = prev;
            int idx = 1;
            for (int g = 0; g < ngrps; ++g) {
                const int v = int(r.get(7));
                if (v > 124) return failWith(error, "an exponent group out of range");
                for (const int dexp : {v / 25, (v % 25) / 5, v % 5}) {
                    prev += dexp - 2;
                    if (prev < 0 || prev > 24) return failWith(error, "an exponent out of range");
                    for (int k = 0; k < grpsize; ++k, ++idx)
                        if (idx < end) exps[ch][idx] = prev;
                }
            }
            r.skip(2);  // gainrng
        }
        if (lfeon && lfeexpstr[blk]) {
            int prev = int(r.get(4));
            lfeexps[0] = prev;
            int idx = 1;
            for (int g = 0; g < 2; ++g) {
                const int v = int(r.get(7));
                for (const int dexp : {v / 25, (v % 25) / 5, v % 5}) {
                    prev += dexp - 2;
                    if (idx < 7) lfeexps[idx++] = prev;
                }
            }
        } else if (lfeon && blk == 0) {
            return failWith(error, "LFE exponents reused in the first block");
        }
        {
            const size_t from = r.pos();  // convsnroffste
            if (r.flag()) r.skip(10);
            cuts.emplace_back(from, r.pos());
        }
        // The mantissas: as many bits as the bit allocation gives each bin (3, 5 and 11 levels grouped across the
        // block's channels).
        int64_t n1 = 0, n2 = 0, n4 = 0, bits = 0;
        auto count = [&](const uint8_t* bap, int end) {
            for (int i = 0; i < end; ++i) {
                if (bap[i] == 1) ++n1;
                else if (bap[i] == 2) ++n2;
                else if (bap[i] == 4) ++n4;
                else bits += kBapBits[bap[i]];
            }
        };
        uint8_t bap[256];
        for (int ch = 0; ch < nfchans; ++ch) {
            if (silent) std::fill(bap, bap + endmant[ch], uint8_t(0));
            else allocate(ba, exps[ch], endmant[ch], kFastGain[4], snroffset, false, bap);
            count(bap, endmant[ch]);
        }
        if (lfeon) {
            if (silent) std::fill(bap, bap + 7, uint8_t(0));
            else allocate(ba, lfeexps, 7, kFastGain[4], snroffset, true, bap);
            count(bap, 7);
        }
        bits += (n1 + 2) / 3 * 5 + (n2 + 2) / 3 * 7 + (n4 + 1) / 2 * 7;
        r.skip(size_t(bits));
        if (r.over() || r.pos() + 18 > bytes * 8) return failWith(error, "the blocks run past the frame");
    }
    // What is left before the error check is the encoder's padding: zeros.
    {
        BitReader rest = r;
        while (rest.pos() < bytes * 8 - 18)
            if (rest.flag()) return failWith(error, "the frame does not end where its blocks do");
    }
    const size_t used = r.pos();
    // Written again: the chanmap in, the independent substream's fields out, padded to the frame's length (a word
    // longer if it no longer fits), the CRC last.
    BitWriter w;
    size_t from = 0;
    auto copyTo = [&](size_t to) {
        for (const auto& c : cuts) {
            if (c.first >= to || c.second <= from) continue;
            if (c.first > from) w.copy(d, from, c.first);
            from = std::max(from, c.second);
        }
        if (to > from) w.copy(d, from, to);
        from = to;
    };
    copyTo(insertAt);
    w.put(1, 1);
    w.put(chanmap, 16);
    copyTo(used);
    size_t words = bytes / 2;
    while (w.bits() + 18 > words * 16) ++words;
    if (words > 2048) return failWith(error, "the frame would be too long");
    while (w.bits() < words * 16 - 16) w.put(0, 1);
    std::vector<uint8_t> out = std::move(w.bytes);
    out.resize(words * 2, 0);
    // strmtyp 1, and the frame's size.
    const uint32_t frmsiz = uint32_t(words - 1);
    out[2] = uint8_t(0x40 | (out[2] & 0x38) | ((frmsiz >> 8) & 7));
    out[3] = uint8_t(frmsiz & 0xff);
    uint16_t crc = crc16(out.data() + 2, out.size() - 4);
    if (crc == 0x0b77) {  // (encinfo flipped so the CRC never reads as a sync word)
        out[out.size() - 3] ^= 1;
        crc = crc16(out.data() + 2, out.size() - 4);
    }
    out[out.size() - 2] = uint8_t(crc >> 8);
    out[out.size() - 1] = uint8_t(crc);
    frame = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------

Eac3Extended::~Eac3Extended() {
    avcodec_free_context(&main_);
    avcodec_free_context(&dep_);
    av_frame_free(&a_);
    av_frame_free(&b_);
    for (AVPacket* p : mainOut_) av_packet_free(&p);
    for (AVPacket* p : depOut_) av_packet_free(&p);
}

bool Eac3Extended::open(int sampleRate, int64_t bitrate, const Ac3Metadata& m, std::string* err) {
    const AVCodec* codec = avcodec_find_encoder_by_name("eac3");
    if (!codec) return failWith(err, "This FFmpeg has no E-AC-3 encoder");
    bitrate_ = bitrate;
    // (a substream of six blocks holds at most 2048 words a frame, 1024 kb/s at 48 kHz; above that FFmpeg's encoder
    // would code fewer blocks, which the rewrite does not take)
    const int64_t most = int64_t(2048) * 16 * sampleRate / 1536 / 8000 * 8000;
    const int64_t first = std::clamp<int64_t>(bitrate * 5 / 8 / 8000 * 8000, 96000, most);
    const int64_t second = std::clamp<int64_t>(bitrate - first, 64000, most);
    for (int k = 0; k < 2; ++k) {
        AVCodecContext*& c = k == 0 ? main_ : dep_;
        c = avcodec_alloc_context3(codec);
        if (!c) return failWith(err, "Out of memory");
        const AVChannelLayout layout = k == 0 ? AVChannelLayout(AV_CHANNEL_LAYOUT_5POINT1) : AVChannelLayout(AV_CHANNEL_LAYOUT_2_2);
        av_channel_layout_copy(&c->ch_layout, &layout);
        c->sample_rate = sampleRate;
        c->sample_fmt = AV_SAMPLE_FMT_FLTP;
        c->bit_rate = k == 0 ? first : second;
        c->time_base = {1, sampleRate};
        AVDictionary* opts = nullptr;
        if (k == 0) {
            ac3EncoderOptions(m, true, true, true, &opts);
        } else {
            // (the surrounds alone: no coupling, which the dependent syntax would carry differently, and no mixing
            // metadata, which is the programme's, in substream 0)
            Ac3Metadata plain;
            plain.dialnorm = m.dialnorm;
            ac3EncoderOptions(plain, true, false, false, &opts);
            av_dict_set(&opts, "channel_coupling", "0", 0);
        }
        const int rc = avcodec_open2(c, codec, &opts);
        av_dict_free(&opts);
        if (rc < 0) {
            char b[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(rc, b, sizeof b);
            return failWith(err, std::string("Cannot open the E-AC-3 encoder: ") + b);
        }
    }
    a_ = av_frame_alloc();
    b_ = av_frame_alloc();
    return a_ && b_;
}

void Eac3Extended::setDialnorm(int dialnorm) {
    ac3SetDialnorm(main_, dialnorm);
    ac3SetDialnorm(dep_, dialnorm);
}

AVCodecContext* Eac3Extended::describe() const {
    const AVCodec* codec = avcodec_find_encoder_by_name("eac3");
    AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!c) return nullptr;
    const AVChannelLayout layout = AV_CHANNEL_LAYOUT_7POINT1;
    av_channel_layout_copy(&c->ch_layout, &layout);
    c->sample_rate = main_ ? main_->sample_rate : 48000;
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->bit_rate = (main_ ? main_->bit_rate : 0) + (dep_ ? dep_->bit_rate : 0);
    c->frame_size = 1536;
    c->initial_padding = main_ ? main_->initial_padding : 256;
    c->time_base = {1, c->sample_rate};
    return c;
}

int Eac3Extended::send(const AVFrame* f) {
    auto pull = [&](AVCodecContext* c, std::deque<AVPacket*>& q) {
        for (;;) {
            AVPacket* p = av_packet_alloc();
            const int rc = avcodec_receive_packet(c, p);
            if (rc < 0) {
                av_packet_free(&p);
                return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : rc;
            }
            q.push_back(p);
        }
    };
    int rc = 0;
    if (!f) {
        flushed_ = true;
        if ((rc = avcodec_send_frame(main_, nullptr)) < 0 && rc != AVERROR_EOF) return rc;
        if ((rc = avcodec_send_frame(dep_, nullptr)) < 0 && rc != AVERROR_EOF) return rc;
    } else {
        if (f->ch_layout.nb_channels != 8 || f->format != AV_SAMPLE_FMT_FLTP) return AVERROR(EINVAL);
        for (int k = 0; k < 2; ++k) {
            AVFrame* o = k == 0 ? a_ : b_;
            av_frame_unref(o);
            o->format = AV_SAMPLE_FMT_FLTP;
            o->sample_rate = f->sample_rate;
            o->nb_samples = f->nb_samples;
            av_channel_layout_copy(&o->ch_layout, &(k == 0 ? main_ : dep_)->ch_layout);
            if ((rc = av_frame_get_buffer(o, 0)) < 0) return rc;
            o->pts = f->pts;
        }
        const size_t n = size_t(f->nb_samples);
        auto src = [&](int c) { return reinterpret_cast<const float*>(f->extended_data[c]); };
        auto dst = [](AVFrame* o, int c) { return reinterpret_cast<float*>(o->extended_data[c]); };
        // 7.1: L R C LFE Lb Rb Ls Rs. Substream 0, 5.1 (side): L R C LFE and each side with its back folded in at
        // -3 dB each; the dependent one: Ls Rs Lrs Rrs.
        for (int c = 0; c < 4; ++c) std::copy(src(c), src(c) + n, dst(a_, c));
        const float g = 0.70710678f;
        for (size_t i = 0; i < n; ++i) {
            dst(a_, 4)[i] = g * (src(6)[i] + src(4)[i]);
            dst(a_, 5)[i] = g * (src(7)[i] + src(5)[i]);
        }
        std::copy(src(6), src(6) + n, dst(b_, 0));
        std::copy(src(7), src(7) + n, dst(b_, 1));
        std::copy(src(4), src(4) + n, dst(b_, 2));
        std::copy(src(5), src(5) + n, dst(b_, 3));
        if ((rc = avcodec_send_frame(main_, a_)) < 0) return rc;
        if ((rc = avcodec_send_frame(dep_, b_)) < 0) return rc;
    }
    if ((rc = pull(main_, mainOut_)) < 0) return rc;
    return pull(dep_, depOut_);
}

int Eac3Extended::receive(AVPacket* pkt) {
    if (mainOut_.empty() || depOut_.empty()) {
        if (flushed_ && (mainOut_.empty() || depOut_.empty())) return AVERROR_EOF;
        return AVERROR(EAGAIN);
    }
    AVPacket* a = mainOut_.front();
    AVPacket* b = depOut_.front();
    mainOut_.pop_front();
    depOut_.pop_front();
    std::vector<uint8_t> dependent(b->data, b->data + b->size);
    // Channel map: Ls (bit 3), Rs (bit 4) and the Lrs/Rrs pair (bit 6), bit 0 the most significant.
    constexpr uint16_t kMap = uint16_t((1u << (15 - 3)) | (1u << (15 - 4)) | (1u << (15 - 6)));
    std::string why;
    int rc = 0;
    if (a->pts != b->pts) {
        error = "the substreams' frames fell out of step";
        rc = AVERROR_BUG;
    } else if (!eac3MakeDependent(dependent, kMap, &why)) {
        error = "Cannot make the dependent substream: " + why;
        rc = AVERROR_EXTERNAL;
    } else if ((rc = av_new_packet(pkt, a->size + int(dependent.size()))) >= 0) {
        std::memcpy(pkt->data, a->data, size_t(a->size));
        std::memcpy(pkt->data + a->size, dependent.data(), dependent.size());
        pkt->pts = a->pts, pkt->dts = a->dts, pkt->duration = a->duration;
        pkt->flags = a->flags;
    }
    av_packet_free(&a);
    av_packet_free(&b);
    return rc;
}

}  // namespace montage
