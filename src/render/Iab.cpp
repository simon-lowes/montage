#include "Iab.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Adm.h"
#include "core/Surround.h"

namespace montage::iab {

namespace {

constexpr uint32_t kIAFrame = 0x08, kBedDefinition = 0x10, kObjectDefinition = 0x40, kAudioDataDlc = 0x200, kAudioDataPcm = 0x400,
                   kFrameSequence = 0x800;

// Fields most significant bit first, as the bitstream sends them.
class BitWriter {
public:
    void put(uint64_t v, int bits) {  // bits <= 32
        acc_ = (acc_ << bits) | (v & ((uint64_t(1) << bits) - 1));
        n_ += bits;
        while (n_ >= 8) {
            n_ -= 8;
            out_.push_back(uint8_t(acc_ >> n_));
        }
        acc_ &= (uint64_t(1) << n_) - 1;
    }
    // Plex(n) (5.2): a field of all ones is an escape to a field twice as wide; the smallest that holds the value.
    void plex(uint32_t v, int n) {
        for (int w = n;; w *= 2) {
            const uint64_t escape = (uint64_t(1) << w) - 1;
            if (v < escape) return put(v, w);
            put(escape, w);
        }
    }
    void align() {
        if (n_) put(0, 8 - n_);
    }
    void bytes(const std::vector<uint8_t>& b) {
        if (n_ == 0) out_.insert(out_.end(), b.begin(), b.end());
        else
            for (uint8_t x : b) put(x, 8);
    }
    std::vector<uint8_t>& data() { return out_; }

private:
    std::vector<uint8_t> out_;
    uint64_t acc_ = 0;
    int n_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* d, size_t size) : d_(d), bits_(uint64_t(size) * 8) {}
    uint32_t get(int bits) {  // bits <= 32
        if (pos_ + uint64_t(bits) > bits_) {
            bad_ = true;
            pos_ = bits_;
            return 0;
        }
        uint64_t v = 0;
        while (bits > 0) {
            const int off = int(pos_ & 7), avail = 8 - off, take = std::min(avail, bits);
            v = (v << take) | ((d_[pos_ >> 3] >> (avail - take)) & ((1u << take) - 1));
            pos_ += uint64_t(take);
            bits -= take;
        }
        return uint32_t(v);
    }
    uint32_t plex(int n) {
        for (int w = n;; w *= 2) {
            const uint64_t escape = (uint64_t(1) << w) - 1;
            const uint32_t v = get(w);
            if (bad_) return 0;
            if (v != escape) return v;
            if (w == 32) {
                bad_ = true;
                return 0;
            }
        }
    }
    void align() { pos_ = std::min(bits_, (pos_ + 7) / 8 * 8); }
    void seek(size_t byte) {
        pos_ = uint64_t(byte) * 8;
        if (pos_ > bits_) bad_ = true, pos_ = bits_;
    }
    size_t byte() const { return size_t(pos_ / 8); }
    bool bad() const { return bad_; }

private:
    const uint8_t* d_;
    uint64_t bits_;
    uint64_t pos_ = 0;
    bool bad_ = false;
};

void element(BitWriter& parent, uint32_t id, const std::vector<uint8_t>& payload) {
    parent.plex(id, 8);
    parent.plex(uint32_t(payload.size()), 8);
    parent.bytes(payload);
}

// DLC sub-blocks (Table 31): how many to a frame and how long, at 48 kHz.
bool dlcBlocks(int code, int& count, int& size) {
    switch (code) {
    case 0: count = 10, size = 200; return true;
    case 1: count = 10, size = 192; return true;
    case 2: count = 8, size = 200; return true;
    case 3: count = 5, size = 200; return true;
    case 4: count = 5, size = 192; return true;
    case 5: count = 4, size = 200; return true;
    case 6: count = 5, size = 100; return true;
    case 7: count = 4, size = 120; return true;
    case 8: count = 4, size = 100; return true;
    default: return false;
    }
}

constexpr int kMaxOrder = 31;  // Order48 is 5 bits

// The lattice coefficients (10-bit codes) as the decoder turns them into direct-form ones (B.7), in its integer maths;
// false when one leaves 32 bits (the decoder's would overflow).
bool directForm(const std::vector<uint32_t>& k, std::vector<int64_t>& a) {
    a.assign(k.size() + 1, 0);
    a[0] = 1048576;
    std::vector<int64_t> tmp(k.size() + 1, 0);
    for (size_t j = 1; j <= k.size(); ++j) {
        const int64_t kj = (int64_t(k[j - 1]) - 512) * 2048;
        for (size_t m = 1; m <= j; ++m) tmp[m] = a[m] + ((kj * a[j - m]) >> 20);
        for (size_t m = 1; m <= j; ++m) {
            a[m] = tmp[m];
            if (a[m] > INT32_MAX || a[m] < INT32_MIN) return false;
        }
    }
    return true;
}

// The residual the decoder adds its prediction to (B.8), from the samples; false when the maths leaves the decoder's
// 32 bits.
bool residual(const int32_t* x, int count, const std::vector<int64_t>& a, std::vector<int64_t>& e) {
    e.resize(size_t(count));
    const size_t order = a.size() - 1;
    if (order == 0) {
        for (int n = 0; n < count; ++n) e[size_t(n)] = x[n];
        return true;
    }
    int32_t buf[64] = {};
    int idx = 0;
    for (int n = 0; n < count; ++n) {
        int64_t acc = 0;
        int i2 = idx;
        for (size_t p = 1; p <= order; ++p) {
            acc -= int64_t(buf[i2]) * a[p];
            i2 = (i2 - 1) & 63;
        }
        const int64_t pred = acc >> 20;
        if (pred > INT32_MAX || pred < INT32_MIN) return false;
        e[size_t(n)] = int64_t(x[n]) - pred;
        if (e[size_t(n)] >= (int64_t(1) << 30) || e[size_t(n)] <= -(int64_t(1) << 30)) return false;
        idx = (idx + 1) & 63;
        buf[idx] = x[n];
    }
    return true;
}

int bitsFor(uint64_t v) {
    int b = 0;
    while (b < 64 && (uint64_t(1) << b) <= v) ++b;
    return b;
}

// The cheaper way to send a sub-block of residuals: PCM of the fewest bits (`rice` -1) or Rice codes with `rice`
// remainder bits. Bits including CodeType and the 5-bit parameter.
int64_t subBlockCost(const int64_t* e, int n, int& rice) {
    uint64_t maxAbs = 0, sum = 0;
    int nonzero = 0;
    for (int i = 0; i < n; ++i) {
        const uint64_t m = uint64_t(e[i] < 0 ? -e[i] : e[i]);
        maxAbs = std::max(maxAbs, m);
        sum += m;
        nonzero += m != 0;
    }
    int64_t best = 6 + int64_t(n) * bitsFor(maxAbs) + nonzero;
    rice = -1;
    // Rice: about log2 of the mean magnitude, tried either side.
    const int guess = sum ? std::max(0, bitsFor(sum / uint64_t(n)) - 1) : 0;
    for (int r = std::max(0, guess - 2); r <= std::min(30, guess + 2); ++r) {
        int64_t bits = 6 + nonzero + int64_t(n) * (r + 1);
        for (int i = 0; i < n && bits < best; ++i) bits += int64_t(uint64_t(e[i] < 0 ? -e[i] : e[i]) >> r);
        if (bits < best) best = bits, rice = r;
    }
    return best;
}

int64_t residualCost(const std::vector<int64_t>& e, int blocks, int size) {
    int64_t bits = 0;
    int r = 0;
    for (int b = 0; b < blocks; ++b) bits += subBlockCost(e.data() + size_t(b) * size_t(size), size, r);
    return bits;
}

void writeResidual(BitWriter& w, const std::vector<int64_t>& e, int blocks, int size) {
    for (int b = 0; b < blocks; ++b) {
        const int64_t* x = e.data() + size_t(b) * size_t(size);
        int rice = -1;
        subBlockCost(x, size, rice);
        if (rice < 0) {
            uint64_t maxAbs = 0;
            for (int i = 0; i < size; ++i) maxAbs = std::max(maxAbs, uint64_t(x[i] < 0 ? -x[i] : x[i]));
            const int depth = bitsFor(maxAbs);
            w.put(0, 1);
            w.put(uint64_t(depth), 5);
            for (int i = 0; i < size; ++i) {
                const uint64_t m = uint64_t(x[i] < 0 ? -x[i] : x[i]);
                if (depth) w.put(m, depth);
                if (m) w.put(x[i] < 0, 1);
            }
        } else {
            w.put(1, 1);
            w.put(uint64_t(rice), 5);
            for (int i = 0; i < size; ++i) {
                const uint64_t m = uint64_t(x[i] < 0 ? -x[i] : x[i]);
                for (uint64_t q = m >> rice; q > 0; --q) w.put(1, 1);
                w.put(0, 1);
                if (rice) w.put(m & ((uint64_t(1) << rice) - 1), rice);
                if (m) w.put(x[i] < 0, 1);
            }
        }
    }
}

uint32_t posXY(double v) { return uint32_t(std::floor(std::clamp(v, 0.0, 1.0) * 32768 + 0.5) + 32767); }
uint32_t posZ(double v) { return uint32_t(std::floor(std::clamp(v, 0.0, 1.0) * 65535 + 0.5)); }
double fromXY(uint32_t d) { return (double(d) - 32767) / 32768; }
double fromZ(uint32_t d) { return double(d) / 65535; }

}  // namespace

int rateCode(uint32_t num, uint32_t den) {
    if (den == 0) return -1;
    static const uint32_t whole[] = {24, 25, 30, 48, 50, 60, 96, 100, 120};
    for (int i = 0; i < 9; ++i)
        if (uint64_t(num) == uint64_t(whole[i]) * den) return i;
    if (uint64_t(num) * 1001 == uint64_t(24000) * den) return 0x9;
    return -1;  // (29.97 and 59.94 among them: ST 2098-2:2022 reserves their codes)
}

void rateOf(int code, uint32_t& num, uint32_t& den) {
    static const uint32_t whole[] = {24, 25, 30, 48, 50, 60, 96, 100, 120};
    if (code >= 0 && code < 9) num = whole[code], den = 1;
    else num = 24000, den = 1001;
}

int frameSamples(int code, int frameIndex) {
    static const int whole[] = {2000, 1920, 1600, 1000, 960, 800, 500, 480, 400};
    (void)frameIndex;
    if (code >= 0 && code < 9) return whole[code];
    if (code == 0x9) return 2002;
    return 0;
}

bool sequenced(int) { return false; }

int panSubBlocks(int code) {
    switch (code) {
    case 3: case 4: case 5: return 4;
    case 6: case 7: case 8: return 2;
    default: return 8;
    }
}

int panSubBlockStart(int code, int frameIndex, int sb) {
    const int n = panSubBlocks(code);
    return int(int64_t(frameSamples(code, frameIndex)) * sb / n);
}

bool dlcAllowed(int code) { return code >= 0 && code < 9; }

const char* channelName(int id) {
    switch (id) {
    case L: return "L";
    case 0x1: return "LC";
    case C: return "C";
    case 0x3: return "RC";
    case R: return "R";
    case Lss: return "Lss";
    case Ls: return "Ls";
    case Lrs: return "Lrs";
    case Rrs: return "Rrs";
    case Rss: return "Rss";
    case Rs: return "Rs";
    case Lts: return "Lts";
    case Rts: return "Rts";
    case LFE: return "LFE";
    default: return "";
    }
}

Point roomPoint(double admX, double admY, double admZ) {
    return {std::clamp((admX + 1) / 2, 0.0, 1.0), std::clamp((1 - admY) / 2, 0.0, 1.0), std::clamp(admZ, 0.0, 1.0)};
}

std::vector<uint8_t> encodeDlc(const int32_t* samples, int count, int code) {
    int blocks = 0, size = 0;
    if (!dlcBlocks(code, blocks, size) || blocks * size != count) return {};
    BitWriter w;
    w.put(0, 2);  // DLCSampleRate: 48 kHz
    w.put(8, 5);  // ShiftBits: 24-bit samples into the decoder's 32
    // The predictor: the autocorrelation method over the frame (the decoder starts it from silence), its lattice
    // coefficients quantized one by one (each next one found for the quantized ones before it). Orders up to 31 are
    // tried, each coded as the decoder's integer maths predicts, until three in a row code no smaller; the smallest
    // is sent, or the samples themselves when that is smaller still.
    std::vector<int64_t> plain;
    residual(samples, count, {1048576}, plain);
    int64_t bestBits = residualCost(plain, blocks, size);
    std::vector<uint32_t> bestK;
    std::vector<int64_t> bestE;
    double r[kMaxOrder + 1] = {};
    for (int lag = 0; lag <= kMaxOrder; ++lag) {
        double s = 0;
        for (int n = lag; n < count; ++n) s += double(samples[n]) * double(samples[n - lag]);
        r[lag] = s;
    }
    std::vector<uint32_t> ks;
    if (r[0] > 0) {
        std::vector<double> a{1};
        double err = r[0];
        for (int j = 1; j <= kMaxOrder && err > 0; ++j) {
            double acc = r[j];
            for (int i = 1; i < j; ++i) acc += a[size_t(i)] * r[j - i];
            const uint32_t K = uint32_t(std::clamp<long>(std::lround(-acc / err * 512) + 512, 1, 1023));
            const double kq = (double(K) - 512) / 512;
            std::vector<double> next(size_t(j) + 1);
            next[0] = 1;
            for (int i = 1; i < j; ++i) next[size_t(i)] = a[size_t(i)] + kq * a[size_t(j - i)];
            next[size_t(j)] = kq;
            err += 2 * kq * acc + kq * kq * err;  // the error left with the quantized coefficient
            a.swap(next);
            ks.push_back(K);
        }
    }
    static const int orders[] = {1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 28, 31};
    int worse = 0;
    std::vector<int64_t> direct, e;
    for (int order : orders) {
        if (order > int(ks.size()) || worse >= 3) break;
        const std::vector<uint32_t> k(ks.begin(), ks.begin() + order);
        // (an order the decoder's 32 bits cannot take is passed over)
        if (!directForm(k, direct) || !residual(samples, count, direct, e)) continue;
        const int64_t bits = residualCost(e, blocks, size) + 11 + 10 * int64_t(order);
        if (bits < bestBits) {
            bestBits = bits, bestK = k, bestE.swap(e), worse = 0;
        } else {
            ++worse;
        }
    }
    if (!bestK.empty()) {
        w.put(1, 2);  // one region over the whole frame
        w.put(uint64_t(blocks), 4);
        w.put(bestK.size(), 5);
        for (uint32_t k : bestK) w.put(k, 10);
        writeResidual(w, bestE, blocks, size);
    } else {
        w.put(0, 2);
        writeResidual(w, plain, blocks, size);
    }
    w.align();
    return std::move(w.data());
}

bool decodeDlc(const uint8_t* data, size_t size, int code, int count, std::vector<int32_t>& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    int blocks = 0, blockSize = 0;
    if (!dlcBlocks(code, blocks, blockSize)) return fail("DLC at a frame rate it does not have");
    if (blocks * blockSize != count) return fail("A DLC waveform of the wrong length");
    BitReader b(data, size);
    const uint32_t rate = b.get(2), shift = b.get(5);
    if (rate != 0) return fail("Montage reads 48 kHz DLC only");
    const uint32_t regions = b.get(2);
    std::vector<uint32_t> lengths, orders;
    std::vector<std::vector<int64_t>> coeffs;
    uint32_t total = 0;
    for (uint32_t i = 0; i < regions; ++i) {
        lengths.push_back(b.get(4));
        orders.push_back(b.get(5));
        std::vector<uint32_t> k;
        for (uint32_t m = 0; m < orders.back(); ++m) k.push_back(b.get(10));
        std::vector<int64_t> a;
        if (!directForm(k, a)) return fail("A DLC predictor that cannot be decoded");
        for (int64_t& v : a) v = int32_t(uint32_t(uint64_t(v)));  // as the decoder's 32 bits would hold it
        coeffs.push_back(std::move(a));
        total += lengths.back();
    }
    if (b.bad()) return fail("A DLC element ends early");
    if (regions && total != uint32_t(blocks)) return fail("DLC predictor regions that do not cover the frame");
    std::vector<int64_t> e(static_cast<size_t>(count));
    for (int n = 0; n < blocks; ++n) {
        const uint32_t type = b.get(1);
        if (type == 0) {
            const uint32_t depth = b.get(5);
            for (int i = 0; i < blockSize; ++i) {
                int64_t v = depth ? b.get(int(depth)) : 0;
                if (v && b.get(1)) v = -v;
                e[size_t(n * blockSize + i)] = v;
            }
        } else {
            const uint32_t rice = b.get(5);
            for (int i = 0; i < blockSize; ++i) {
                uint64_t q = 0;
                while (b.get(1) == 1) {
                    if (b.bad() || ++q > (uint64_t(1) << 31)) return fail("A DLC element ends early");
                }
                int64_t v = int64_t(rice ? b.get(int(rice)) : 0) + int64_t(q << rice);
                if (v && b.get(1)) v = -v;
                e[size_t(n * blockSize + i)] = v;
            }
        }
        if (b.bad()) return fail("A DLC element ends early");
    }
    out.resize(size_t(count));
    std::vector<int32_t> pcm(static_cast<size_t>(count));
    if (regions == 0) {
        for (int n = 0; n < count; ++n) pcm[size_t(n)] = int32_t(e[size_t(n)]);
    } else {
        int32_t buf[64] = {};
        int idx = 0, n = 0;
        for (uint32_t r = 0; r < regions; ++r)
            for (uint32_t j = 0; j < lengths[r]; ++j)
                for (int k = 0; k < blockSize; ++k) {
                    int64_t acc = 0;
                    int i2 = idx;
                    for (uint32_t p = 1; p <= orders[r]; ++p) {
                        acc = int64_t(uint64_t(acc) - uint64_t(int64_t(buf[i2]) * coeffs[r][p]));  // (wrapping, as the decoder's does)
                        i2 = (i2 - 1) & 63;
                    }
                    // (Output = Residual + prediction, in the decoder's 32 bits)
                    const int32_t o = int32_t(uint32_t(uint64_t(e[size_t(n)])) + uint32_t(uint64_t(acc >> 20)));
                    idx = (idx + 1) & 63;
                    buf[idx] = o;
                    pcm[size_t(n++)] = o;
                }
    }
    // The decoder's 32-bit output, back to 24 bits.
    for (int n = 0; n < count; ++n) out[size_t(n)] = int32_t(uint32_t(pcm[size_t(n)]) << shift) >> 8;
    return true;
}

bool writeFrame(const Frame& f, bool dlc, std::vector<uint8_t>& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (f.rate < 0 || f.rate > 0x9) return fail("IAB has no such frame rate");
    if (dlc && !dlcAllowed(f.rate)) return fail("DLC is only defined for whole-number frame rates");
    const int samples = frameSamples(f.rate, f.frameIndex), subBlocks = panSubBlocks(f.rate);
    std::vector<std::vector<uint8_t>> subs;
    std::vector<uint32_t> ids;
    if (sequenced(f.rate)) {
        ids.push_back(kFrameSequence);
        subs.push_back({uint8_t(f.frameIndex)});
    }
    for (const auto& [id, wave] : f.audio) {
        if (id == 0) return fail("AudioDataID 0 means silence and has no waveform");
        if (int(wave.size()) != samples) return fail("A waveform of the wrong length for its frame");
        BitWriter w;
        w.plex(id, 8);
        if (dlc) {
            const std::vector<uint8_t> coded = encodeDlc(wave.data(), samples, f.rate);
            if (coded.size() > 0xFFFF) return fail("A DLC waveform too large for its frame");
            w.put(coded.size(), 16);
            w.bytes(coded);
        } else {
            for (int32_t s : wave) {
                const uint32_t v = uint32_t(s) & 0xFFFFFF;
                w.put(v & 0xFF, 8);
                w.put((v >> 8) & 0xFF, 8);
                w.put(v >> 16, 8);
            }
        }
        w.align();
        ids.push_back(dlc ? kAudioDataDlc : kAudioDataPcm);
        subs.push_back(std::move(w.data()));
    }
    auto known = [&](uint32_t id) { return id == 0 || f.audio.count(id); };
    if (f.hasBed) {
        BitWriter w;
        w.plex(f.bedMetaId, 8);
        w.put(0, 1);  // ConditionalBed
        w.plex(uint32_t(f.bed.size()), 4);
        for (const BedChannel& c : f.bed) {
            if (!known(c.audio)) return fail("A bed channel's waveform is missing");
            w.plex(uint32_t(c.id), 4);
            w.plex(c.audio, 8);
            w.put(0, 2);  // ChannelGainPrefix: unity
            w.put(0, 1);  // ChannelDecorInfoExists
        }
        w.put(0x180, 10);
        w.align();
        w.put(0x01, 8);  // AudioDescription: not indicated
        w.plex(0, 8);    // no sub-elements
        ids.push_back(kBedDefinition);
        subs.push_back(std::move(w.data()));
    }
    for (const Object& o : f.objects) {
        if (!known(o.audio)) return fail("An object's waveform is missing");
        if (int(o.pan.size()) != subBlocks) return fail("An object needs a position for each pan sub-block");
        BitWriter w;
        w.plex(o.metaId, 8);
        w.plex(o.audio, 8);
        w.put(1, 1);     // ConditionalObject, for every use (as cinema processors expect)
        w.put(1, 1);     // reserved
        w.put(0xFF, 8);  // ObjectUseCase: always
        w.put(0, 1);     // reserved
        uint32_t last[3] = {};
        for (int sb = 0; sb < subBlocks; ++sb) {
            const uint32_t p[3] = {posXY(o.pan[size_t(sb)].x), posXY(o.pan[size_t(sb)].y), posZ(o.pan[size_t(sb)].z)};
            if (sb > 0) {
                const bool changed = !std::equal(p, p + 3, last);
                w.put(changed, 1);
                if (!changed) continue;
            }
            std::copy(p, p + 3, last);
            w.put(0, 2);  // ObjectGainPrefix: unity
            w.put(1, 3);  // reserved, 0b001
            w.put(p[0], 16);
            w.put(p[1], 16);
            w.put(p[2], 16);
            w.put(0, 1);   // ObjectSnap
            w.put(0, 1);   // ObjectZoneControl
            w.put(2, 2);   // ObjectSpreadMode: 1D
            w.put(0, 12);  // a point
            w.put(0, 4);   // reserved
            w.put(0, 2);   // ObjectDecorCoefPrefix
        }
        w.align();
        w.put(0x01, 8);
        w.plex(0, 8);
        ids.push_back(kObjectDefinition);
        subs.push_back(std::move(w.data()));
    }
    BitWriter frame;
    frame.put(1, 8);  // Version
    frame.put(0, 2);  // 48 kHz
    frame.put(1, 2);  // 24 bits
    frame.put(uint64_t(f.rate), 4);
    frame.plex(uint32_t(f.maxRendered ? f.maxRendered : int(f.bed.size() + f.objects.size())), 8);
    frame.align();
    frame.plex(uint32_t(subs.size()), 8);
    for (size_t i = 0; i < subs.size(); ++i) element(frame, ids[i], subs[i]);
    BitWriter iaFrame;
    element(iaFrame, kIAFrame, frame.data());
    const std::vector<uint8_t>& body = iaFrame.data();
    out.clear();
    out.reserve(body.size() + 10);
    out.push_back(0x01);  // PreambleTag, an empty preamble
    out.insert(out.end(), {0, 0, 0, 0});
    out.push_back(0x02);  // IAFrameTag
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t(body.size() >> (8 * i)));
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

bool readFrame(const uint8_t* data, size_t size, Frame& f, std::string* error, size_t* used, std::vector<uint32_t>* skipped) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    f = Frame{};
    if (size < 10 || data[0] != 0x01) return fail("Not an IABitstream frame (no preamble)");
    const uint32_t preamble = (uint32_t(data[1]) << 24) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 8) | data[4];
    if (uint64_t(preamble) + 10 > size || data[5 + preamble] != 0x02) return fail("Not an IABitstream frame (no IAFrame)");
    const uint8_t* p = data + 6 + preamble;
    const uint32_t length = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    p += 4;
    if (uint64_t(p - data) + length > size) return fail("An IAFrame runs past its data");
    if (used) *used = size_t(p - data) + length;
    BitReader top(p, length);
    if (top.plex(8) != kIAFrame) return fail("The IAFrame is not an IAFrame element");
    const uint32_t frameSize = top.plex(8);
    const size_t at = top.byte();
    if (top.bad() || at + frameSize > length) return fail("An IAFrame runs past its data");
    BitReader b(p + at, frameSize);
    if (b.get(8) != 1) return fail("An IAFrame of a version Montage does not read");
    const uint32_t rate = b.get(2), depth = b.get(2);
    f.rate = int(b.get(4));
    f.maxRendered = int(b.plex(8));
    b.align();
    const uint32_t count = b.plex(8);
    if (b.bad()) return fail("An IAFrame ends early");
    if (rate != 0) return fail("Montage reads 48 kHz IAB only");
    if (depth != 1) return fail("Montage reads 24-bit IAB only");
    if (f.rate > 0x9) return fail("An IAFrame with a reserved frame rate");
    f.hasBed = false;
    const int subBlocks = panSubBlocks(f.rate);
    const uint8_t* base = p + at;
    {  // (the FrameSequence first, wherever it is: it sets the waveforms' length)
        BitReader scan = b;
        for (uint32_t n = 0; n < count; ++n) {
            const uint32_t id = scan.plex(8), bytes = scan.plex(8);
            const size_t start = scan.byte();
            if (scan.bad() || start + bytes > frameSize) break;
            if (id == kFrameSequence && bytes > 0 && (f.frameIndex = base[start]) > 4) return fail("A FrameIndex past 4");
            scan.seek(start + bytes);
        }
    }
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t id = b.plex(8), bytes = b.plex(8);
        const size_t start = b.byte();
        if (b.bad() || start + bytes > frameSize) return fail("An element runs past its frame");
        BitReader e(base + start, bytes);
        if (id == kFrameSequence) {
            f.frameIndex = int(e.get(8));
            if (f.frameIndex > 4) return fail("A FrameIndex past 4");
        } else if (id == kAudioDataPcm || id == kAudioDataDlc) {
            const uint32_t audio = e.plex(8);
            const int samples = frameSamples(f.rate, f.frameIndex);
            std::vector<int32_t> wave(static_cast<size_t>(samples));
            if (id == kAudioDataPcm) {
                f.pcm = true;
                for (int i = 0; i < samples; ++i) {
                    const uint32_t lo = e.get(8), mid = e.get(8), hi = e.get(8);
                    wave[size_t(i)] = int32_t((hi << 24) | (mid << 16) | (lo << 8)) >> 8;
                }
            } else {
                f.dlc = true;
                const uint32_t dlcSize = e.get(16);
                const size_t from = e.byte();
                if (e.bad() || from + dlcSize > bytes) return fail("A DLC element runs past its size");
                std::string why;
                if (!decodeDlc(base + start + from, dlcSize, f.rate, samples, wave, &why)) return fail(why);
            }
            if (e.bad()) return fail("An audio element ends early");
            if (audio == 0 || !f.audio.emplace(audio, std::move(wave)).second) return fail("A repeated or zero AudioDataID");
        } else if (id == kBedDefinition) {
            if (f.hasBed) {
                if (skipped) skipped->push_back(id);  // a second bed
            } else {
                f.hasBed = true;
                f.bedMetaId = e.plex(8);
                if (e.get(1)) e.get(8);  // BedUseCase
                const uint32_t channels = e.plex(4);
                for (uint32_t c = 0; c < channels && !e.bad(); ++c) {
                    BedChannel ch;
                    ch.id = int(e.plex(4));
                    ch.audio = e.plex(8);
                    if (e.get(2) > 1) e.get(10);
                    if (e.get(1)) {
                        e.get(4);
                        if (e.get(2) > 1) e.get(8);
                    }
                    f.bed.push_back(ch);
                }
                e.get(10);
                e.align();
                if (e.get(8) & 0x80)
                    while (!e.bad() && e.get(8) != 0) {
                    }
                if (e.plex(8) && skipped) skipped->push_back(id);  // remaps and nested beds
                if (e.bad()) return fail("A BedDefinition ends early");
            }
        } else if (id == kObjectDefinition) {
            Object o;
            o.metaId = e.plex(8);
            o.audio = e.plex(8);
            if (e.get(1)) e.get(9);
            e.get(1);
            Point last;
            for (int sb = 0; sb < subBlocks && !e.bad(); ++sb) {
                if (sb > 0 && !e.get(1)) {
                    o.pan.push_back(last);
                    continue;
                }
                if (e.get(2) > 1) e.get(10);
                e.get(3);
                last.x = fromXY(e.get(16));
                last.y = fromXY(e.get(16));
                last.z = fromZ(e.get(16));
                if (e.get(1)) {  // snap
                    if (e.get(1)) e.get(12);
                    e.get(1);
                }
                if (e.get(1))  // zones
                    for (int z = 0; z < 9; ++z)
                        if (e.get(2) > 1) e.get(10);
                switch (e.get(2)) {
                case 0: e.get(8); break;
                case 2: e.get(12); break;
                case 3: e.get(12), e.get(12), e.get(12); break;
                default: break;
                }
                e.get(4);
                if (e.get(2) > 1) e.get(8);
                o.pan.push_back(last);
            }
            e.align();
            if (e.get(8) & 0x80)
                while (!e.bad() && e.get(8) != 0) {
                }
            if (e.plex(8) && skipped) skipped->push_back(id);  // zone definitions and nested objects
            if (e.bad()) return fail("An ObjectDefinition ends early");
            f.objects.push_back(std::move(o));
        } else if (skipped) {
            skipped->push_back(id);
        }
        b.seek(start + bytes);  // the next element, past this one's size
    }
    for (const BedChannel& c : f.bed)
        if (c.audio && !f.audio.count(c.audio)) return fail("A bed channel names a waveform the frame does not have");
    for (const Object& o : f.objects)
        if (o.audio && !f.audio.count(o.audio)) return fail("An object names a waveform the frame does not have");
    return true;
}

BedMap bedMap(const std::string& layout) {
    BedMap m;
    auto feed = [&](int id, int channel, float gain) {
        auto it = std::find_if(m.bed.begin(), m.bed.end(), [&](const BedMap::BedOut& b) { return b.id == id; });
        if (it == m.bed.end()) {
            m.bed.push_back({id, {}});
            it = m.bed.end() - 1;
        }
        it->from.push_back({channel, gain});
    };
    const std::vector<Speaker>& speakers = layoutSpeakers(layout);
    for (size_t c = 0; c < speakers.size(); ++c) {
        const std::string label = speakers[c].label;
        const int ch = int(c);
        if (label == "M+030") feed(L, ch, 1);
        else if (label == "M-030") feed(R, ch, 1);
        else if (label == "M+000") feed(C, ch, 1);
        else if (label == "LFE1") feed(LFE, ch, 1);
        else if (label == "M+090") feed(Lss, ch, 1);
        else if (label == "M-090") feed(Rss, ch, 1);
        else if (label == "M+135") feed(Lrs, ch, 1);
        else if (label == "M-135") feed(Rrs, ch, 1);
        else if (label == "U+090") feed(Lts, ch, 1);
        else if (label == "U-090") feed(Rts, ch, 1);
        else if (label == "M+110") feed(Lss, ch, 0.70710678f), feed(Lrs, ch, 0.70710678f);
        else if (label == "M-110") feed(Rss, ch, 0.70710678f), feed(Rrs, ch, 0.70710678f);
        else {
            const AdmPoint at = admSpeakerPoint(speakers[c]);
            m.objects.push_back({ch, roomPoint(at.x, at.y, at.z)});
        }
    }
    std::sort(m.bed.begin(), m.bed.end(), [](const BedMap::BedOut& a, const BedMap::BedOut& b) { return a.id < b.id; });
    return m;
}

struct Program::Impl {
    ImmersiveMix mix;
    ProgramSettings set;
    int code = 0;
    double seqFps = 24;
    BedMap map;
    int64_t written = 0, at = 0;
    std::vector<float> bed, objects;
    std::vector<float*> objectPtrs;
};

Program::Program() : d_(std::make_unique<Impl>()) {}
Program::~Program() = default;
int64_t Program::written() const { return d_->written; }
int Program::bedChannels() const { return int(d_->map.bed.size()); }
int Program::objects() const { return int(d_->map.objects.size() + d_->mix.objectTracks().size()); }
int Program::rateCode() const { return d_->code; }

bool Program::open(const Project& p, const Sequence& s, const ProgramSettings& settings, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    d_ = std::make_unique<Impl>();
    Impl& d = *d_;
    d.set = settings;
    d.code = iab::rateCode(settings.rateNum, settings.rateDen);
    if (d.code < 0) return fail("IAB has no frame rate of " + std::to_string(settings.rateNum) + "/" + std::to_string(settings.rateDen));
    if (settings.dlc && !dlcAllowed(d.code)) return fail("Lossless IAB (DLC) needs a whole-number frame rate");
    if (!d.mix.open(p, s, settings.mixRate, error)) return false;
    d.seqFps = s.fpsValue();
    d.map = bedMap(d.mix.layout());
    const int objects = this->objects();
    if (settings.cinema && objects > 118) return fail("A cinema IAB track holds at most 118 objects; this mix has " + std::to_string(objects));
    if (bedChannels() + objects > 128) return fail("An IAB frame renders at most 128 channels and objects");
    const int most = frameSamples(d.code, 0) + 2;
    d.bed.resize(size_t(most) * size_t(d.mix.bedChannels()));
    d.objects.resize(size_t(most) * std::max<size_t>(1, d.mix.objectTracks().size()));
    for (size_t k = 0; k < d.mix.objectTracks().size(); ++k) d.objectPtrs.push_back(d.objects.data() + k * size_t(most));
    return true;
}

bool Program::next(std::vector<uint8_t>& out, std::string* error) {
    Impl& d = *d_;
    if (d.written >= d.set.frames) return false;
    Frame f;
    f.rate = d.code;
    f.frameIndex = sequenced(d.code) ? int(d.written % 5) : 0;
    const int n = frameSamples(d.code, f.frameIndex);
    const int64_t start = d.set.firstSample + d.at;
    d.mix.mix(start, n, d.bed.data(), d.objectPtrs.data());
    const int bedN = d.mix.bedChannels();
    auto heard = [&](int i) { return d.set.heard < 0 || d.at + i < d.set.heard; };
    auto pcm = [](double v) { return int32_t(std::lround(std::clamp(v, -1.0, 8388607.0 / 8388608.0) * 8388608.0)); };
    uint32_t audio = 0;
    f.bedMetaId = 0;
    for (const BedMap::BedOut& b : d.map.bed) {
        std::vector<int32_t> wave(static_cast<size_t>(n));
        for (int i = 0; i < n && heard(i); ++i) {
            double v = 0;
            for (const BedMap::Feed& fd : b.from) v += double(d.bed[size_t(i) * size_t(bedN) + size_t(fd.channel)]) * fd.gain;
            wave[size_t(i)] = pcm(v);
        }
        f.bed.push_back({b.id, ++audio});
        f.audio[audio] = std::move(wave);
    }
    const int subBlocks = panSubBlocks(d.code);
    uint32_t meta = 0;
    for (const BedMap::FixedObject& o : d.map.objects) {
        std::vector<int32_t> wave(static_cast<size_t>(n));
        for (int i = 0; i < n && heard(i); ++i) wave[size_t(i)] = pcm(d.bed[size_t(i) * size_t(bedN) + size_t(o.channel)]);
        f.objects.push_back({++meta, ++audio, std::vector<Point>(size_t(subBlocks), o.at)});
        f.audio[audio] = std::move(wave);
    }
    for (size_t k = 0; k < d.mix.objectTracks().size(); ++k) {
        std::vector<int32_t> wave(static_cast<size_t>(n));
        for (int i = 0; i < n && heard(i); ++i) wave[size_t(i)] = pcm(d.objectPtrs[k][i]);
        Object o{++meta, ++audio, {}};
        for (int sb = 0; sb < subBlocks; ++sb) {
            const double frame = double(start + panSubBlockStart(d.code, f.frameIndex, sb)) * d.seqFps / d.set.mixRate;
            const AdmPoint at = d.mix.objectAt(k, frame);
            o.pan.push_back(roomPoint(at.x, at.y, at.z));
        }
        f.objects.push_back(std::move(o));
        f.audio[audio] = std::move(wave);
    }
    if (!writeFrame(f, d.set.dlc, out, error)) return false;
    d.at += n;
    ++d.written;
    return true;
}

}  // namespace montage::iab
