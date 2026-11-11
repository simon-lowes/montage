// Montage — smart rendering of long-GOP exports (see LongGopCopy.h).
#include "LongGopCopy.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "core/Interpretation.h"
#include "media/Decoder.h"
#include "render/ColorSpace.h"
#include "render/NalBits.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/timecode.h>
}

namespace montage {

namespace {

constexpr int kH264 = AV_CODEC_ID_H264, kHevc = AV_CODEC_ID_HEVC, kMpeg2 = AV_CODEC_ID_MPEG2VIDEO, kAv1 = AV_CODEC_ID_AV1, kVp9 = AV_CODEC_ID_VP9;

// AV1: the OBUs of a temporal unit (or of av1C's configOBUs), each with its size field (5.3), as type and payload.
template <typename F>
void forEachObu(const uint8_t* d, size_t n, F&& f) {
    size_t i = 0;
    while (i < n) {
        const uint8_t h = d[i];
        const int type = (h >> 3) & 15;
        const bool ext = h & 4, sized = h & 2;
        i += 1 + (ext ? 1 : 0);
        size_t size = n - std::min(n, i);
        if (sized) {
            size = 0;
            for (int k = 0; k < 8 && i < n; ++k) {
                const uint8_t b = d[i++];
                size |= size_t(b & 0x7f) << (7 * k);
                if (!(b & 0x80)) break;
            }
        }
        if (i + size > n) return;
        f(type, d + i, size);
        i += size;
    }
}

// AV1: a stream's sequence header OBU's payload (from a temporal unit, an encoder's extradata, or av1C).
std::vector<uint8_t> av1SequenceHeader(const uint8_t* d, size_t n) {
    if (n >= 4 && (d[0] & 0x80) && (d[0] & 0x7f) == 1) d += 4, n -= 4;  // (av1C: its four bytes, then configOBUs)
    std::vector<uint8_t> out;
    forEachObu(d, n, [&](int type, const uint8_t* p, size_t len) {
        if (type == 1 && out.empty()) out.assign(p, p + len);
    });
    return out;
}

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

std::string averr(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, buf, sizeof buf);
    return buf;
}

// ---- NAL units ----

// The NAL units of a packet whose units each follow their length (`size` bytes, big-endian); false if they overrun it.
template <class F>
bool forEachNal(const uint8_t* d, size_t n, int size, F&& f) {
    size_t i = 0;
    while (i + size_t(size) <= n) {
        size_t len = 0;
        for (int k = 0; k < size; ++k) len = len << 8 | d[i + size_t(k)];
        i += size_t(size);
        if (len == 0 || i + len > n) return false;
        f(d + i, len);
        i += len;
    }
    return i == n;
}

// The NAL units of Annex B bytes (between start codes, trailing zeros dropped).
std::vector<std::pair<const uint8_t*, size_t>> annexBNals(const uint8_t* d, size_t n) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) --end;
                if (end > start) out.push_back({d + start, end - start});
            }
            i += 3;
            start = i;
            continue;
        }
        ++i;
    }
    if (start != SIZE_MAX && start < n) {
        size_t end = n;
        while (end > start && d[end - 1] == 0) --end;
        if (end > start) out.push_back({d + start, end - start});
    }
    return out;
}

void putNal(std::vector<uint8_t>& out, const uint8_t* d, size_t n, int size) {
    for (int k = size - 1; k >= 0; --k) out.push_back(uint8_t(n >> (8 * k)));
    out.insert(out.end(), d, d + n);
}

// Annex B bytes as length-prefixed units; false when a unit is too long for `size` bytes.
bool toLengthPrefixed(const uint8_t* d, size_t n, int size, std::vector<uint8_t>& out) {
    for (const auto& [p, len] : annexBNals(d, n)) {
        if (size < 4 && len >= (size_t(1) << (8 * size))) return false;
        putNal(out, p, len, size);
    }
    return true;
}

// Exp-Golomb reading over a NAL unit's payload (emulation prevention bytes taken out).
struct Bits {
    std::vector<uint8_t> b;
    size_t pos = 0;
    Bits(const uint8_t* d, size_t n) {
        int zeros = 0;
        for (size_t i = 0; i < n && b.size() < 64; ++i) {
            if (zeros >= 2 && d[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = d[i] == 0 ? zeros + 1 : 0;
            b.push_back(d[i]);
        }
    }
    int bit() {
        if (pos >= b.size() * 8) return -1;
        const int v = (b[pos / 8] >> (7 - pos % 8)) & 1;
        ++pos;
        return v;
    }
    int64_t ue() {
        int z = 0;
        for (;;) {
            const int v = bit();
            if (v < 0 || z > 31) return -1;
            if (v) break;
            ++z;
        }
        int64_t v = 0;
        for (int k = 0; k < z; ++k) {
            const int x = bit();
            if (x < 0) return -1;
            v = v << 1 | x;
        }
        return (int64_t(1) << z) - 1 + v;
    }
};

int h264SpsId(const uint8_t* nal, size_t n) {
    if (n < 5) return -1;
    Bits b(nal + 4, n - 4);  // (after the header byte, profile, constraints and level)
    return int(b.ue());
}

int h264PpsId(const uint8_t* nal, size_t n) {
    if (n < 2) return -1;
    Bits b(nal + 1, n - 1);
    return int(b.ue());
}

// An avcC record, taken apart.
struct Avcc {
    uint8_t profile = 0, compat = 0, level = 0;
    int lengthSize = 4;
    std::vector<std::vector<uint8_t>> sps, pps;
    std::vector<uint8_t> tail;  // (High profiles' chroma format and bit depths)
};

bool parseAvcc(const uint8_t* d, size_t n, Avcc& a) {
    if (n < 7 || d[0] != 1) return false;
    a.profile = d[1], a.compat = d[2], a.level = d[3];
    a.lengthSize = (d[4] & 3) + 1;
    size_t i = 6;
    for (int k = 0, count = d[5] & 31; k < count; ++k) {
        if (i + 2 > n) return false;
        const size_t len = size_t(d[i]) << 8 | d[i + 1];
        i += 2;
        if (i + len > n) return false;
        a.sps.emplace_back(d + i, d + i + len);
        i += len;
    }
    if (i >= n) return false;
    for (int k = 0, count = d[i++]; k < count; ++k) {
        if (i + 2 > n) return false;
        const size_t len = size_t(d[i]) << 8 | d[i + 1];
        i += 2;
        if (i + len > n) return false;
        a.pps.emplace_back(d + i, d + i + len);
        i += len;
    }
    a.tail.assign(d + i, d + n);
    return !a.sps.empty() && !a.pps.empty();
}

std::vector<uint8_t> buildAvcc(const Avcc& a) {
    std::vector<uint8_t> o{1, a.profile, a.compat, a.level, uint8_t(0xfc | (a.lengthSize - 1)), uint8_t(0xe0 | a.sps.size())};
    for (const auto& s : a.sps) {
        o.push_back(uint8_t(s.size() >> 8)), o.push_back(uint8_t(s.size()));
        o.insert(o.end(), s.begin(), s.end());
    }
    o.push_back(uint8_t(a.pps.size()));
    for (const auto& s : a.pps) {
        o.push_back(uint8_t(s.size() >> 8)), o.push_back(uint8_t(s.size()));
        o.insert(o.end(), s.begin(), s.end());
    }
    o.insert(o.end(), a.tail.begin(), a.tail.end());
    return o;
}

// An hvcC record's VPS, SPS and PPS units (in that order) and its length size.
bool parseHvcc(const uint8_t* d, size_t n, std::vector<std::vector<uint8_t>>& sets, int& lengthSize) {
    if (n < 23 || d[0] != 1) return false;
    lengthSize = (d[21] & 3) + 1;
    std::vector<std::vector<uint8_t>> byType[3];
    size_t i = 23;
    for (int a = 0, arrays = d[22]; a < arrays; ++a) {
        if (i + 3 > n) return false;
        const int type = d[i] & 0x3f;
        const int count = d[i + 1] << 8 | d[i + 2];
        i += 3;
        for (int k = 0; k < count; ++k) {
            if (i + 2 > n) return false;
            const size_t len = size_t(d[i]) << 8 | d[i + 1];
            i += 2;
            if (i + len > n) return false;
            if (type >= 32 && type <= 34) byType[type - 32].emplace_back(d + i, d + i + len);
            i += len;
        }
    }
    sets.clear();
    for (auto& t : byType) sets.insert(sets.end(), t.begin(), t.end());
    return !byType[1].empty() && !byType[2].empty();
}

// ---- What a packet holds ----

enum : uint8_t {
    kCopyPoint = 1,  // a GOP may start here (H.264 IDR; HEVC IRAP; MPEG-2 GOP header with an I-picture)
    kIdr = 2,        // and nothing after it refers to anything before it (H.264 IDR, HEVC IDR, MPEG-2 closed GOP)
    kRasl = 4,       // HEVC: a RASL picture; H.264: a picture before its recovery point's I-picture (marked with the GOP)
    kRef = 8,        // H.264: a reference picture (nal_ref_idc not 0)
    kRecovery = 16,  // H.264: an I-picture with a recovery point SEI (recovery_frame_cnt 0): an open GOP's start
};

// H.264: whether an SEI NAL unit holds a recovery point whose pictures are right from this one on (recovery_frame_cnt 0).
bool h264RecoveryNow(const uint8_t* nal, size_t n) {
    const std::vector<uint8_t> rb = nal::unescape(nal, n);
    size_t i = 1;
    while (i + 2 <= rb.size()) {
        if (rb[i] == 0x80 && i + 1 == rb.size()) break;
        int type = 0, size = 0;
        while (i < rb.size() && rb[i] == 0xff) type += 255, ++i;
        if (i >= rb.size()) break;
        type += rb[i++];
        while (i < rb.size() && rb[i] == 0xff) size += 255, ++i;
        if (i >= rb.size()) break;
        size += rb[i++];
        if (i + size_t(size) > rb.size()) break;
        if (type == 6) {
            // (recovery_frame_cnt 0 and exact_match_flag: the pictures from it on decode exactly as without what is before)
            nal::Reader r(rb.data() + i, size_t(size));
            const uint64_t count = r.ue();
            const bool exact = r.flag();
            return count == 0 && exact && r.ok;
        }
        i += size_t(size);
    }
    return false;
}

// H.264: the slice type of a slice NAL unit (0-9; -1 when it cannot be read).
int h264SliceType(const uint8_t* nal, size_t n) {
    const std::vector<uint8_t> rb = nal::unescape(nal, std::min<size_t>(n, 16));
    nal::Reader r(rb, 8);
    r.ue();
    const int t = int(r.ue());
    return r.ok ? t : -1;
}

// H.264 slice header fields up to the reference marking (7.3.3), for rewriting a slice.
struct H264Slice {
    std::vector<uint8_t> rb;  // the RBSP, its header byte first
    const hrd::Sps* sps = nullptr;
    const hrd::Pps* pps = nullptr;
    uint64_t firstMb = 0, type = 0, ppsId = 0;
    size_t frameNumAt = 0;   // bit of frame_num
    uint64_t frameNum = 0;
    size_t afterFrame = 0;   // after frame_num and the field flags
    size_t pocLsbAt = 0;     // pic_order_cnt_lsb (pic_order_cnt_type 0; else 0)
    uint64_t pocLsb = 0;
    size_t markingAt = 0;    // dec_ref_pic_marking()
    size_t afterMarking = 0;
    bool adaptive = false;   // adaptive_ref_pic_marking_mode_flag, and its operations (op, then its one or two values)
    std::vector<std::array<uint64_t, 3>> ops;
    size_t dataAt = 0;       // where slice_data() begins (CABAC: before its alignment bits)
};

bool readH264Slice(const uint8_t* d, size_t n, const std::map<int, hrd::Sps>& sps, const std::map<int, hrd::Pps>& pps, H264Slice& sl,
                   bool full) {
    sl.rb = nal::unescape(d, n);
    nal::Reader r(sl.rb, 8);
    sl.firstMb = r.ue(), sl.type = r.ue(), sl.ppsId = r.ue();
    auto pi = pps.find(int(sl.ppsId));
    if (!r.ok || pi == pps.end() || !pi->second.valid) return false;
    auto si = sps.find(pi->second.spsId);
    if (si == sps.end() || !si->second.valid) return false;
    sl.pps = &pi->second, sl.sps = &si->second;
    const hrd::Sps& s = *sl.sps;
    if (s.separateColourPlane) r.u(2);
    sl.frameNumAt = r.pos;
    sl.frameNum = r.u(s.log2MaxFrameNum);
    bool field = false;
    if (!s.frameMbsOnly) {
        field = r.flag();
        if (field) r.flag();
    }
    sl.afterFrame = r.pos;
    const bool idr = (d[0] & 31) == 5;
    if (idr) r.ue();
    if (s.pocType == 0) {
        sl.pocLsbAt = r.pos;
        sl.pocLsb = r.u(s.log2MaxPocLsb);
        if (!full) return r.ok;
        if (sl.pps->bottomFieldPicOrder && !field) r.se();
    } else {
        if (!full) return r.ok;
        if (s.pocType == 1 && !s.deltaPicOrderAlwaysZero) {
            r.se();
            if (sl.pps->bottomFieldPicOrder && !field) r.se();
        }
    }
    if (sl.pps->redundantPicCnt) r.ue();
    const int st = int(sl.type % 5);  // (0 P, 1 B, 2 I, 3 SP, 4 SI)
    const bool intra = st == 2 || st == 4, bi = st == 1;
    if (!intra) {
        // The reference lists' sizes, their modifications and the prediction weights (7.3.3.1, 7.3.3.2).
        if (bi) r.flag();  // direct_spatial_mv_pred_flag
        uint64_t l0 = uint64_t(sl.pps->numRefIdxL0), l1 = uint64_t(sl.pps->numRefIdxL1);
        if (r.flag()) {
            l0 = r.ue() + 1;
            if (bi) l1 = r.ue() + 1;
        }
        if (l0 > 32 || l1 > 32) return false;
        for (int list = 0; list < (bi ? 2 : 1); ++list)
            if (r.flag())
                for (int k = 0; k < 100 && r.ok; ++k) {
                    const uint64_t idc = r.ue();
                    if (idc == 3) break;
                    if (idc > 5) return false;
                    r.ue();
                }
        if ((sl.pps->weightedPred && !bi) || (sl.pps->weightedBipred == 1 && bi)) {
            const bool chroma = s.chromaFormat != 0 && !s.separateColourPlane;
            r.ue();
            if (chroma) r.ue();
            for (int list = 0; list < (bi ? 2 : 1); ++list)
                for (uint64_t i = 0; i < (list ? l1 : l0) && r.ok; ++i) {
                    if (r.flag()) r.se(), r.se();
                    if (chroma && r.flag()) r.se(), r.se(), r.se(), r.se();
                }
        }
    }
    sl.markingAt = r.pos;
    if (d[0] & 0x60) {
        if (idr) {
            r.flag(), r.flag();
        } else if ((sl.adaptive = r.flag())) {
            for (int k = 0; k < 64 && r.ok; ++k) {
                const uint64_t op = r.ue();
                if (op == 0) break;
                if (op > 6) return false;
                std::array<uint64_t, 3> o{op, 0, 0};
                if (op == 1 || op == 3) o[1] = r.ue();
                if (op == 2) o[1] = r.ue();
                if (op == 3 || op == 6) o[2] = r.ue();
                if (op == 4) o[1] = r.ue();
                sl.ops.push_back(o);
            }
        }
    }
    sl.afterMarking = r.pos;
    if (sl.pps->cabac && !intra) r.ue();  // cabac_init_idc
    r.se();                   // slice_qp_delta
    if (st == 3) r.flag();    // sp_for_switch_flag
    if (st == 3 || st == 4) r.se();  // slice_qs_delta
    if (sl.pps->deblockingControl && r.ue() != 1) r.se(), r.se();
    sl.dataAt = r.pos;
    return r.ok;
}

// H.264: a slice of a non-IDR I-picture made an IDR picture's (7.4.1.2.4): nal_unit_type 5, frame_num 0, an
// idr_pic_id, its reference marking an IDR's (the pictures before it still output, it not long-term); the slice data
// itself as it was (CABAC's realigned to the byte). Empty when it cannot be.
std::vector<uint8_t> h264SliceAsIdr(const uint8_t* d, size_t n, const std::map<int, hrd::Sps>& sps, const std::map<int, hrd::Pps>& pps,
                                    int idrPicId) {
    H264Slice sl;
    if (n < 2 || (d[0] & 31) != 1 || !(d[0] & 0x60) || !readH264Slice(d, n, sps, pps, sl, true) || sl.type % 5 != 2) return {};
    nal::Writer w;
    w.copy(sl.rb, 8, sl.frameNumAt);
    w.u(0, sl.sps->log2MaxFrameNum);
    w.copy(sl.rb, sl.frameNumAt + size_t(sl.sps->log2MaxFrameNum), sl.afterFrame);
    w.ue(uint64_t(idrPicId & 0xffff));
    if (sl.sps->pocType == 0) {
        // (an IDR picture's order count is 0, 8.2.1: the pictures after it have its lsb taken off theirs)
        w.u(0, sl.sps->log2MaxPocLsb);
        w.copy(sl.rb, sl.pocLsbAt + size_t(sl.sps->log2MaxPocLsb), sl.markingAt);
    } else {
        w.copy(sl.rb, sl.afterFrame, sl.markingAt);
    }
    w.flag(false);  // no_output_of_prior_pics_flag
    w.flag(false);  // long_term_reference_flag
    if (sl.pps->cabac) {
        w.copy(sl.rb, sl.afterMarking, sl.dataAt);
        while (!w.aligned()) w.bit(1);  // (cabac_alignment_one_bit)
        const size_t from = (sl.dataAt + 7) / 8;
        w.out.insert(w.out.end(), sl.rb.begin() + std::ptrdiff_t(from), sl.rb.end());
    } else {
        const size_t stop = nal::stopBit(sl.rb);
        if (stop < sl.dataAt) return {};
        w.copy(sl.rb, sl.afterMarking, stop);
        w.trailing();
    }
    const uint8_t header = uint8_t((d[0] & 0xe0) | 5);
    return nal::escape(&header, 1, w.out);
}

// H.264, after an I-picture made an IDR picture: what its pictures' numbers are taken from, until the next IDR picture.
struct H264Renumber {
    uint64_t frameNumShift = 0;  // the I-picture's frame_num
    uint64_t pocShift = 0;       // its pic_order_cnt_lsb (pic_order_cnt_type 0)
    uint64_t sinceIdr = 0;       // the latest picture's frame_num as rewritten, not wrapped (reference pictures since it)
};

// H.264: a slice with the frame_num and pic_order_cnt_lsb of `rn` taken off its own (modulo their maxima), as after
// an I-picture made an IDR one. A reference picture's marking operations on pictures from before that IDR picture
// (gone from the decoder there) are left out; with none left, the sliding window marks as they did (it removes
// nothing the operations kept). Empty when it cannot be done. `rn.sinceIdr` follows the pictures.
std::vector<uint8_t> h264SliceRenumbered(const uint8_t* d, size_t n, const std::map<int, hrd::Sps>& sps, const std::map<int, hrd::Pps>& pps,
                                         H264Renumber& rn) {
    H264Slice sl;
    if (!readH264Slice(d, n, sps, pps, sl, false)) return {};
    const int bits = sl.sps->log2MaxFrameNum, pocBits = sl.sps->log2MaxPocLsb;
    const uint64_t wrap = uint64_t(1) << bits, pocWrap = uint64_t(1) << pocBits;
    const uint64_t frameNum = (sl.frameNum + wrap - rn.frameNumShift % wrap) % wrap;
    // (frame_num rises by one after each reference picture: unwrapped, the first value at or past the one before)
    uint64_t since = rn.sinceIdr - rn.sinceIdr % wrap + frameNum;
    if (since < rn.sinceIdr) since += wrap;
    rn.sinceIdr = since;
    const bool poc = sl.sps->pocType == 0;
    nal::Writer w;
    w.copy(sl.rb, 8, sl.frameNumAt);
    w.u(frameNum, bits);
    // Up to `to`: the rest of the header as it was, the new pic_order_cnt_lsb in its place.
    auto header = [&](size_t to) {
        if (!poc) {
            w.copy(sl.rb, sl.frameNumAt + size_t(bits), to);
            return;
        }
        w.copy(sl.rb, sl.frameNumAt + size_t(bits), sl.pocLsbAt);
        w.u((sl.pocLsb + pocWrap - rn.pocShift % pocWrap) % pocWrap, pocBits);
        w.copy(sl.rb, sl.pocLsbAt + size_t(pocBits), to);
    };
    if ((d[0] & 0x60) && readH264Slice(d, n, sps, pps, sl, true) && sl.adaptive) {
        std::vector<std::array<uint64_t, 3>> kept;
        for (const auto& o : sl.ops) {
            // (difference_of_pic_nums_minus1: the picture CurrPicNum - (it + 1); before the IDR picture when that is
            // below 0, counted without wrapping)
            const bool before = (o[0] == 1 || o[0] == 3) && o[1] + 1 > since;
            if (before && o[0] == 3) return {};  // (a long-term reference made of a picture that is not there)
            if (!before) kept.push_back(o);
        }
        if (kept.size() != sl.ops.size()) {
            header(sl.markingAt);
            w.flag(!kept.empty());
            if (!kept.empty()) {
                for (const auto& o : kept) {
                    w.ue(o[0]);
                    if (o[0] == 1 || o[0] == 2 || o[0] == 3 || o[0] == 4) w.ue(o[1]);
                    if (o[0] == 3 || o[0] == 6) w.ue(o[2]);
                }
                w.ue(0);
            }
            if (sl.pps->cabac) {
                w.copy(sl.rb, sl.afterMarking, sl.dataAt);
                while (!w.aligned()) w.bit(1);  // (cabac_alignment_one_bit)
                const size_t from = (sl.dataAt + 7) / 8;
                w.out.insert(w.out.end(), sl.rb.begin() + std::ptrdiff_t(from), sl.rb.end());
            } else {
                const size_t stop = nal::stopBit(sl.rb);
                if (stop < sl.dataAt) return {};
                w.copy(sl.rb, sl.afterMarking, stop);
                w.trailing();
            }
            return nal::escape(d, 1, w.out);
        }
    }
    header(sl.rb.size() * 8);
    return nal::escape(d, 1, w.out);
}

uint8_t packetKind(int codec, const uint8_t* d, size_t n, int lengthSize) {
    uint8_t kind = 0;
    if (codec == kH264) {
        bool recovery = false, intra = true, slices = false;
        forEachNal(d, n, lengthSize, [&](const uint8_t* nal, size_t len) {
            const int type = nal[0] & 31;
            if (type == 5) kind |= kCopyPoint | kIdr;
            if (type == 6 && !slices && h264RecoveryNow(nal, len)) recovery = true;
            if (type == 1 || type == 5) {
                if (!slices && (nal[0] & 0x60)) kind |= kRef;
                slices = true;
                const int st = h264SliceType(nal, len);
                intra = intra && (st % 5 == 2);
            }
        });
        // (an I-picture, a reference, that starts a recovery point: copies may start there)
        if (!(kind & kIdr) && recovery && slices && intra && (kind & kRef)) kind |= kCopyPoint | kRecovery;
    } else if (codec == kHevc) {
        bool seen = false;
        forEachNal(d, n, lengthSize, [&](const uint8_t* nal, size_t len) {
            if (len < 2 || seen) return;
            const int type = (nal[0] >> 1) & 0x3f;
            if (type >= 32) return;  // (not a picture's slice)
            seen = true;
            if (type >= 16 && type <= 23) kind |= kCopyPoint;
            if (type == 19 || type == 20) kind |= kIdr;
            if (type == 8 || type == 9) kind |= kRasl;
        });
    } else if (codec == kMpeg2) {
        bool seq = false, gop = false, closed = false;
        int type = 0;
        for (size_t i = 0; i + 8 <= n; ++i) {
            if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
            if (d[i + 3] == 0xb3) seq = true;
            else if (d[i + 3] == 0xb8) {
                gop = true;
                closed = (d[i + 7] >> 6) & 1;
            } else if (d[i + 3] == 0x00) {
                type = (d[i + 5] >> 3) & 7;
                break;
            }
        }
        // (with a sequence header: the GOP is then decoded with its own matrices and settings, whatever came before)
        if (seq && gop && type == 1) kind |= kCopyPoint | (closed ? kIdr : 0);
    } else if (codec == kAv1) {
        // A shown key frame with its sequence header: nothing after it refers to anything before it (7.5).
        bool seq = false, reduced = false, seen = false;
        forEachObu(d, n, [&](int type, const uint8_t* p, size_t len) {
            if (type == 1 && len > 0) seq = true, reduced = (p[0] >> 3) & 1;
            if ((type == 3 || type == 6) && len > 0 && !seen) {
                seen = true;
                const bool showExisting = p[0] & 0x80;
                const int frameType = (p[0] >> 5) & 3;
                const bool show = (p[0] >> 4) & 1;
                if (seq && !reduced && !showExisting && frameType == 0 && show) kind |= kCopyPoint | kIdr;
            }
        });
    } else if (codec == kVp9) {
        // A shown key frame: it sets every reference buffer and every probability context anew, so nothing after it
        // refers to anything before it. Its header's first bits (the VP9 bitstream's uncompressed_header): the frame
        // marker, the profile, show_existing_frame, frame_type (0: key), show_frame.
        if (n >= 1 && (d[0] >> 6) == 2) {
            const int profile = ((d[0] >> 5) & 1) | (((d[0] >> 4) & 1) << 1);
            int bit = profile == 3 ? 5 : 4;  // (profile 3 has a reserved bit after it)
            auto next = [&] {
                const int v = size_t(bit / 8) < n ? (d[bit / 8] >> (7 - bit % 8)) & 1 : 1;
                ++bit;
                return v;
            };
            const bool showExisting = next(), inter = next(), show = next();
            if (!showExisting && !inter && show) kind |= kCopyPoint | kIdr;
        }
    }
    return kind;
}

// Whether the stream is shown turned (a display matrix with a rotation): its pictures are then decoded turned, as
// coded they are not.
bool turned(const AVStream* st) {
    const uint8_t* matrix = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
    if (const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                             AV_PKT_DATA_DISPLAYMATRIX))
        matrix = sd->data;
#else
    matrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
#endif
    if (!matrix) return false;
    const double theta = av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    return !std::isnan(theta) && std::fabs(std::remainder(theta, 360.0)) > 0.5;
}

const char* x264Profile(int profile) {
    switch (profile & 0xff) {
    case 66: return "baseline";
    case 77: return "main";
    case 100: return "high";
    case 110: return "high10";
    case 122: return "high422";
    case 244: return "high444";
    default: return nullptr;
    }
}

// ---- What copied pictures keep ----

// Whether an SEI unit's payload (after its NAL header) holds only ITU-T T.35 messages (captions, HDR10+, AFD, bar data):
// what the export did not ask for.
bool onlyT35(const uint8_t* d, size_t n) {
    std::vector<uint8_t> b;
    b.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && d[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = d[i] == 0 ? zeros + 1 : 0;
        b.push_back(d[i]);
    }
    size_t i = 0;
    int messages = 0;
    while (i < b.size() && !(b[i] == 0x80 && i + 1 == b.size())) {  // (up to the RBSP's stop bit)
        int64_t type = 0, size = 0;
        while (i < b.size() && b[i] == 0xff) type += 255, ++i;
        if (i >= b.size()) return false;
        type += b[i++];
        while (i < b.size() && b[i] == 0xff) size += 255, ++i;
        if (i >= b.size()) return false;
        size += b[i++];
        if (type != 4 || size > int64_t(b.size() - i)) return false;
        i += size_t(size);
        ++messages;
    }
    return messages > 0;
}

// A copied picture without what rides along with the source's pictures but the export was not asked for: closed
// captions, HDR10+ and AFD in T.35 messages (H.264 and HEVC SEI; MPEG-2 user data "GA94" and "DTG1") and Dolby Vision's
// units (HEVC types 62 and 63). False when nothing was taken out (`out` then left empty).
bool withoutExtras(int codec, const uint8_t* d, size_t n, int lengthSize, std::vector<uint8_t>& out) {
    out.clear();
    bool dropped = false;
    if (codec == kH264 || codec == kHevc) {
        std::vector<uint8_t> kept;
        const bool ok = forEachNal(d, n, lengthSize, [&](const uint8_t* nal, size_t len) {
            bool drop = false;
            if (codec == kH264) drop = (nal[0] & 31) == 6 && onlyT35(nal + 1, len - 1);
            else if (len >= 2) {
                const int type = (nal[0] >> 1) & 0x3f;
                drop = type == 62 || type == 63 || ((type == 39 || type == 40) && onlyT35(nal + 2, len - 2));
            }
            if (drop) dropped = true;
            else putNal(kept, nal, len, lengthSize);
        });
        if (!ok || !dropped) return false;
        out = std::move(kept);
        return true;
    }
    if (codec == kMpeg2) {
        size_t i = 0, copyFrom = 0;
        while (i + 8 <= n) {
            if (d[i] || d[i + 1] || d[i + 2] != 1 || d[i + 3] != 0xb2 ||
                !(std::memcmp(d + i + 4, "GA94", 4) == 0 || std::memcmp(d + i + 4, "DTG1", 4) == 0)) {
                ++i;
                continue;
            }
            size_t j = i + 4;
            while (j + 3 <= n && !(d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1)) ++j;
            if (j + 3 > n) j = n;
            out.insert(out.end(), d + copyFrom, d + i);
            copyFrom = i = j;
            dropped = true;
        }
        if (!dropped) return false;
        out.insert(out.end(), d + copyFrom, d + n);
        return true;
    }
    return false;
}

// MPEG-2: a picture's vbv_delay (90 kHz ticks; -1 when it has none, 0xFFFF as at a variable rate).
int64_t vbvDelay(const uint8_t* d, size_t n) {
    for (size_t i = 0; i + 8 <= n; ++i)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1 && d[i + 3] == 0x00) {
            const uint32_t v = uint32_t(d[i + 4]) << 24 | uint32_t(d[i + 5]) << 16 | uint32_t(d[i + 6]) << 8 | d[i + 7];
            const int64_t delay = (v >> 3) & 0xffff;  // (after the temporal reference's 10 bits and the type's 3)
            return delay == 0xffff ? -1 : delay;
        }
    return -1;
}

// MPEG-2: the GOP header's time code set to hh:mm:ss:ff (its drop-frame, closed and broken-link flags kept).
void setGopTimecode(uint8_t* d, size_t n, int hh, int mm, int ss, int ff) {
    for (size_t i = 0; i + 8 <= n; ++i)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1 && d[i + 3] == 0xb8) {
            uint32_t v = uint32_t(d[i + 4]) << 24 | uint32_t(d[i + 5]) << 16 | uint32_t(d[i + 6]) << 8 | d[i + 7];
            // drop(1) hours(5) minutes(6) marker(1) seconds(6) pictures(6) closed(1) broken(1), from the top bit
            v &= ~(uint32_t(0x1f) << 26 | uint32_t(0x3f) << 20 | uint32_t(0x3f) << 13 | uint32_t(0x3f) << 7);
            v |= uint32_t(hh & 0x1f) << 26 | uint32_t(mm & 0x3f) << 20 | 1u << 19 | uint32_t(ss & 0x3f) << 13 | uint32_t(ff & 0x3f) << 7;
            d[i + 4] = uint8_t(v >> 24), d[i + 5] = uint8_t(v >> 16), d[i + 6] = uint8_t(v >> 8), d[i + 7] = uint8_t(v);
            return;
        }
}

// The time code `frames` after `start` as text (hh:mm:ss:ff, ';' before the frames when dropping) and its fields.
std::string timecodeText(AVRational rate, int64_t start, bool drop, int64_t frames, int f[4]) {
    AVTimecode tc{};
    if (av_timecode_init(&tc, rate, drop ? AV_TIMECODE_FLAG_DROPFRAME : 0, int(std::clamp<int64_t>(start, 0, INT_MAX)), nullptr) < 0)
        av_timecode_init(&tc, rate, 0, 0, nullptr);
    char buf[AV_TIMECODE_STR_SIZE] = {};
    av_timecode_make_string(&tc, buf, int(std::clamp<int64_t>(frames, 0, INT_MAX)));
    int h = 0, m = 0, sec = 0, fr = 0;
    char sep = ':';
    if (std::sscanf(buf, "%d:%d:%d%c%d", &h, &m, &sec, &sep, &fr) == 5) f[0] = h % 24, f[1] = m, f[2] = sec, f[3] = fr;
    return buf;
}

// H.264 profiles by what they hold (a decoder of a later one decodes the earlier).
int h264ProfileRank(int idc) {
    switch (idc) {
    case 66: return 1;
    case 77: return 2;
    case 100: return 3;
    case 110: return 4;
    case 122: return 5;
    case 244: return 6;
    default: return 0;
    }
}

// The profile, tier and level an opened encoder's parameter sets state (its extradata, Annex B), 0 when they state none.
struct Stated {
    int profile = 0, level = 0;
    bool highTier = false;
};

Stated statedBy(int codec, const AVCodecContext* m) {
    Stated st;
    if (!m->extradata || m->extradata_size <= 0) return st;
    for (const auto& [d, n] : annexBNals(m->extradata, size_t(m->extradata_size))) {
        if (codec == kH264 && (d[0] & 31) == 7 && n >= 4) {
            st.profile = d[1], st.level = d[3];
            break;
        }
        if (codec == kHevc && n > 2 && ((d[0] >> 1) & 0x3f) == 33) {
            // (after the SPS's VPS id, sub-layers and nesting: profile space, tier and profile, 4 bytes of compatibility
            // flags, 6 of constraints, then the level)
            const Bits b(d + 2, n - 2);
            if (b.b.size() < 13) continue;
            st.profile = b.b[1] & 0x1f, st.highTier = (b.b[1] >> 5) & 1, st.level = b.b[12];
            break;
        }
    }
    return st;
}

// HEVC: whether a decoder of the export's profile decodes the source's (the same pixels either way).
bool hevcProfileFits(int source, int exported) {
    return source == exported || (source == 1 && exported == 2) || (source == 3 && (exported == 1 || exported == 2));
}

// An avcC's High profiles' extension (ISO/IEC 14496-15, 5.3.3.1.2): chroma format, bit depths and the SPS extensions
// (those of `sources`' extensions, each once). Empty for Baseline, Main and Extended.
std::vector<uint8_t> avccHighExtension(int profile, AVPixelFormat format, const std::vector<std::vector<uint8_t>>& sources) {
    if (profile == 66 || profile == 77 || profile == 88) return {};
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(format);
    if (!d) return {};
    const int chroma = d->nb_components < 3 ? 0 : d->log2_chroma_w == 1 && d->log2_chroma_h == 1 ? 1 : d->log2_chroma_w == 1 ? 2 : 3;
    const int depth = std::clamp(d->comp[0].depth - 8, 0, 7);
    std::vector<std::vector<uint8_t>> ext;
    for (const auto& t : sources) {
        if (t.size() < 4) continue;
        for (size_t i = 4, k = 0; k < t[3] && i + 2 <= t.size(); ++k) {
            const size_t len = size_t(t[i]) << 8 | t[i + 1];
            if (i + 2 + len > t.size()) break;
            std::vector<uint8_t> e(t.begin() + std::ptrdiff_t(i + 2), t.begin() + std::ptrdiff_t(i + 2 + len));
            if (std::find(ext.begin(), ext.end(), e) == ext.end() && ext.size() < 255) ext.push_back(std::move(e));
            i += 2 + len;
        }
    }
    std::vector<uint8_t> o{uint8_t(0xfc | chroma), uint8_t(0xf8 | depth), uint8_t(0xf8 | depth), uint8_t(ext.size())};
    for (const auto& e : ext) {
        o.push_back(uint8_t(e.size() >> 8)), o.push_back(uint8_t(e.size()));
        o.insert(o.end(), e.begin(), e.end());
    }
    return o;
}

// The HRD a stream's parameter sets state (its first SPS with one), whole NAL units: what the copies and encoded parts
// keep across the joins.
struct StatedHrd {
    bool has = false;
    double rate = 0, size = 0;
    bool cbr = false;
    std::vector<uint8_t> sps;
    bool same(const StatedHrd& o) const { return has == o.has && (!has || (rate == o.rate && size == o.size && cbr == o.cbr)); }
};

StatedHrd statedHrd(int codec, const std::vector<std::vector<uint8_t>>& sets) {
    StatedHrd h;
    for (const auto& u : sets) {
        if (u.size() < 4) continue;
        const int type = codec == kH264 ? (u[0] & 31) : (u[0] >> 1) & 0x3f;
        if (type != (codec == kH264 ? 7 : 33)) continue;
        const hrd::Sps sp = codec == kH264 ? hrd::parseH264Sps(u.data(), u.size()) : hrd::parseHevcSps(u.data(), u.size());
        const hrd::Params* pr = sp.valid ? sp.active() : nullptr;
        if (!pr || pr->cpbs.empty() || sp.tick() <= 0 || pr->subPic) continue;
        h.has = true;
        h.rate = pr->cpbs[0].bitRate, h.size = pr->cpbs[0].size, h.cbr = pr->cpbs[0].cbr;
        h.sps = u;
        break;
    }
    return h;
}

}  // namespace

// ---- The plan ----

struct LongGopCopier::Packet {
    int64_t dts = 0, pts = 0;
    int64_t frame = 0;  // the source's frame (display order)
    int size = 0;
    uint8_t kind = 0;
    bool key = false;
    double asked = -1;  // a copy point's initial delay (seconds: its buffering period's, MPEG-2 its vbv_delay); -1 none
    double rate = 0, buffer = 0;  // MPEG-2: a copy point's sequence header's bit rate (bits/s) and VBV buffer (bits)
};

struct LongGopCopier::Gop {
    size_t begin = 0, end = 0;  // packets [begin, end), decode order
    int64_t lo = 0, hi = 0;     // the frames it shows
    int64_t entryLo = 0;        // the first it shows when started after pictures not its own (HEVC: RASL left out)
    bool whole = false;         // its frames run on, one each
    bool entryWhole = false;
    bool idr = false;
    int64_t bytes = 0;
    double asked = -1;    // the initial delay its first picture asks for (see Packet)
    double margin = 1e9;  // the least time (seconds) any of its pictures' bits are in before it is decoded, so started
    // MPEG-2: what its sequence header states, and at a variable rate (no vbv_delay) the fewest bits the buffer must
    // hold when its first picture is decoded for it and the run's pictures after it to be there in time.
    double rate = 0, buffer = 0, need = 0;
};

struct LongGopCopier::Source {
    Id media = 0;
    AVFormatContext* fmt = nullptr;
    int stream = -1;
    AVRational tb{1, 1};
    int64_t start = 0, ticks = 1;
    AVRational rate{0, 1};                    // its frame rate
    bool exact = true;                        // a frame is a whole number of `tb`'s ticks (else: times are rounded)
    std::vector<uint8_t> config;              // the decoder configuration (avcC, hvcC)
    std::vector<uint8_t> sets;                // HEVC: its VPS, SPS and PPS, length-prefixed
    Avcc avcc;                                // H.264
    std::map<int, hrd::Sps> hsps;             // H.264: its parameter sets, read
    std::map<int, hrd::Pps> hpps;
    int profile = 0, level = 0;
    AVPacket* held = nullptr;                 // read, not yet used
    int64_t nextDts = AV_NOPTS_VALUE;         // the packet a plain read gives next
    bool usable = false;
    ~Source() {
        av_packet_free(&held);
        if (fmt) avformat_close_input(&fmt);
    }
};

struct LongGopCopier::Run {
    int source = -1;
    FrameTime e0 = 0;      // the export's frame (relative) showing source frame s0
    int64_t s0 = 0, len = 0;
    std::vector<Packet> packets;
    std::vector<Gop> gops;
};

struct LongGopCopier::Copy {
    int run = -1;
    size_t gop = 0;
    FrameTime lo = 0, hi = 0;  // export frames shown (relative, inclusive)
    bool dropRasl = false;
    bool broken = false;  // HEVC: its CRA picture marked a broken link (BLA), for what came before is not its own
    bool chained = false;  // after the copy of the GOP before it in the source, so decoded on from it
    bool standalone = true;  // decodes after any picture (else, chained, only after the GOP it follows)
    int64_t bytes = 0;
};

// The most a frame of footage may take to be copied: a tenth over the export's own rate; at a quality (CRF), half as
// much again as the encoder typically spends there (x264's CRF 23 at a tenth of a bit a pixel at 1080p, x265's CRF 28
// at half that; more a pixel on smaller pictures, less on larger, as bits go with the pixels' count to the power 3/4;
// twice as much at the fastest presets, which leave out most of the coding tools), halved every six steps worse. (So a
// phone's 4K H.264 at 50 Mb/s, a fifth of a bit a pixel, is encoded for a draft at CRF 28, and its HEVC at 40 Mb/s for
// an HEVC export at CRF 22.)
double LongGopCopier::ceilingBitsPerFrame() const {
    const AVCodecContext* m = enc_.main;
    const double fps = m->framerate.num > 0 && m->framerate.den > 0 ? av_q2d(m->framerate) : 25.0;
    if (codec_ == kMpeg2) return m->bit_rate > 0 ? 1.1 * double(m->bit_rate) / fps : 1e300;
    if (enc_.bitrate > 0) return 1.1 * double(enc_.bitrate) / fps;
    const double pixels = std::max(1.0, double(m->width) * double(m->height));
    // (AV1's and VP9's scales run to 63: their 35 spends about what x265's 28 does)
    const double perPixel = codec_ == kHevc  ? 0.05 * std::exp2((28 - enc_.crf) / 6.0)
                            : codec_ == kAv1 || codec_ == kVp9 ? 0.05 * std::exp2((35 - enc_.crf) / 6.0)
                                             : 0.1 * std::exp2((23 - enc_.crf) / 6.0);
    const double size = std::pow(pixels / (1920.0 * 1080.0), -0.25);
    const double fastest = enc_.preset == "ultrafast" || enc_.preset == "superfast" || (codec_ == kAv1 && std::atoi(enc_.preset.c_str()) >= 10) ? 2.0 : 1.0;
    return 1.5 * perPixel * size * fastest * pixels;
}

LongGopCopier::~LongGopCopier() {
    avcodec_free_context(&ctx_);
    av_packet_free(&pkt_);
}

std::unique_ptr<LongGopCopier> LongGopCopier::plan(const Project& p, const Sequence& seq, FrameTime in, FrameTime out, const ColorSpace& seqSpace,
                                                   const LongGopEncoder& encoder, AVFormatContext* oc, AVStream* vst, std::string* why) {
    auto none = [&](const std::string& w) -> std::unique_ptr<LongGopCopier> {
        if (why) *why = w;
        return nullptr;
    };
    const AVCodecContext* m = encoder.main;
    if (!m || !m->codec || !oc || !vst || out <= in) return none("no video");
    const std::string name = m->codec->name;
    const int codec = m->codec_id;
    const bool av1 = (name == "libsvtav1" || name == "libaom-av1") && codec == kAv1;
    const bool vp9 = name == "libvpx-vp9" && codec == kVp9;
    if (!(name == "libx264" && codec == kH264) && !(name == "libx265" && codec == kHevc) && !(name == "mpeg2video" && codec == kMpeg2) && !av1 && !vp9)
        return none("copies H.264 (x264), HEVC (x265), MPEG-2, AV1 (SVT-AV1, libaom) and VP9 (libvpx), not " + name);
    const std::string format = oc->oformat->name;
    const bool isoFile = format.find("mp4") != std::string::npos || format.find("mov") != std::string::npos;
    const bool mkvFile = format == "matroska" || format == "webm";
    if (codec == kMpeg2 ? !(isoFile || format == "mxf" || format.rfind("mxf", 0) == 0)
        : av1           ? !(isoFile || mkvFile)
        : vp9           ? !(format == "mp4" || mkvFile)
                        : !isoFile)
        return none("copies into MP4 and MOV (MPEG-2: MXF and MOV; AV1: MP4, Matroska and WebM; VP9: MP4, Matroska and WebM)");
    const bool interlaced = m->field_order != AV_FIELD_PROGRESSIVE && m->field_order != AV_FIELD_UNKNOWN;
    if (interlaced && codec != kMpeg2) return none("copies interlaced MPEG-2 only");

    std::unique_ptr<LongGopCopier> lg(new LongGopCopier);
    lg->enc_ = encoder;
    lg->oc_ = oc;
    lg->vst_ = vst;
    lg->codec_ = codec;
    lg->frames_ = out - in;
    lg->gop_ = std::max(1, m->gop_size);
    if (codec == kMpeg2 && !encoder.startTimecode.empty()) {
        AVTimecode tc{};
        if (av_timecode_init_from_string(&tc, m->framerate, encoder.startTimecode.c_str(), nullptr) >= 0) {
            lg->timecodeStart_ = tc.start;
            lg->dropFrame_ = tc.flags & AV_TIMECODE_FLAG_DROPFRAME;
        }
    }

    // The runs: untouched frames of one clip, its source's frames one after another.
    for (FrameTime f = in; f < out; ++f) {
        const Clip* c = untouchedClipAt(seq, f);
        if (!c) continue;
        const double sf = c->sourceFrameAt(f);
        const int64_t s = std::llround(sf);
        if (std::fabs(sf - double(s)) > 1e-6 || s < 0) continue;
        Run* last = lg->runs_.empty() ? nullptr : &lg->runs_.back();
        if (last && last->e0 + last->len == f - in && last->s0 + last->len == s && lg->sources_[size_t(last->source)]->media == c->mediaId) {
            ++last->len;
            continue;
        }
        int src = -1;
        for (size_t i = 0; i < lg->sources_.size(); ++i)
            if (lg->sources_[i]->media == c->mediaId) src = int(i);
        if (src < 0) {
            lg->sources_.push_back(std::make_unique<Source>());
            lg->sources_.back()->media = c->mediaId;
            src = int(lg->sources_.size()) - 1;
        }
        Run r;
        r.source = src;
        r.e0 = f - in;
        r.s0 = s;
        r.len = 1;
        lg->runs_.push_back(std::move(r));
    }
    if (lg->runs_.empty()) return none("no untouched footage");

    // The sources: the same pictures the encoder would make, in a file whose packets can be taken as they are, at no
    // profile, tier or level above what the export's encoder states (when it states them).
    const Stated exported = statedBy(codec, m);
    // The buffer model: the first source's HRD (H.264, HEVC), kept across every join; sources stating another are not
    // copied beside it. MPEG-2 at a constant rate keeps the export's (XDCAM HD422's) VBV.
    StatedHrd mode;
    bool modeSet = false;
    auto hrdFits = [&](const StatedHrd& h) {
        if (!modeSet) {
            mode = h, modeSet = true;
            return true;
        }
        return mode.same(h);
    };
    std::set<int> spsIds, ppsIds;
    std::map<int, std::vector<uint8_t>> spsById, ppsById;
    for (auto& sp : lg->sources_) {
        Source& src = *sp;
        const MediaItem* mi = p.findMedia(src.media);
        if (!mi || mi->kind != MediaKind::Video || mi->path.empty()) continue;
        if (mediaColorSpace(*mi).id != seqSpace.id) continue;  // (would be converted)
        if (!interpretationOf(*mi).empty() || !mi->stereo.empty()) continue;
        if (openMediaInput(&src.fmt, mi->path) < 0) continue;
        if (avformat_find_stream_info(src.fmt, nullptr) < 0) continue;
        src.stream = av_find_best_stream(src.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (src.stream < 0) continue;
        for (unsigned i = 0; i < src.fmt->nb_streams; ++i)
            if (int(i) != src.stream) src.fmt->streams[i]->discard = AVDISCARD_ALL;
        const AVStream* st = src.fmt->streams[src.stream];
        const AVCodecParameters* par = st->codecpar;
        const AVRational rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        // (MPEG-2 states its scan in every sequence: unknown there is not taken as progressive)
        const bool progressive = par->field_order == AV_FIELD_PROGRESSIVE || (par->field_order == AV_FIELD_UNKNOWN && codec != kMpeg2);
        if (par->codec_id != codec || par->width != m->width || par->height != m->height || par->format != m->pix_fmt ||
            av_cmp_q(rate, m->framerate) != 0 || seq.width != par->width || seq.height != par->height)
            continue;
        if (interlaced ? par->field_order != m->field_order : !progressive) continue;
        // Shown as coded: not turned, square or the same pixels, the same range (a full-range HEVC is yuv420p as a
        // limited one is).
        const AVRational sar = par->sample_aspect_ratio.num > 0 ? par->sample_aspect_ratio : AVRational{1, 1};
        const AVRational msar = m->sample_aspect_ratio.num > 0 ? m->sample_aspect_ratio : AVRational{1, 1};
        if (turned(st) || av_cmp_q(sar, msar) != 0) continue;
        if (par->color_range == AVCOL_RANGE_JPEG ? m->color_range != AVCOL_RANGE_JPEG : m->color_range == AVCOL_RANGE_JPEG) continue;
        src.tb = st->time_base;
        // (frames counted from the file's start, as the decoder counts them: the earliest of its streams)
        src.start = src.fmt->start_time != AV_NOPTS_VALUE ? av_rescale_q(src.fmt->start_time, AV_TIME_BASE_Q, src.tb) : 0;
        src.ticks = av_rescale_q(1, av_inv_q(rate), src.tb);
        src.rate = rate;
        src.exact = src.ticks > 0 && av_cmp_q(av_mul_q(AVRational{int(src.ticks), 1}, src.tb), av_inv_q(rate)) == 0;
        // A frame that is not a whole number of the file's ticks: Matroska and WebM count in milliseconds, so at 24,
        // 30 or 60 frames a second each packet's time is its frame's, rounded. Its frame is then the nearest one (AV1
        // and VP9 only, whose packets come in the order shown; frames under four ticks long are not told apart so).
        const bool rounded = (codec == kAv1 || codec == kVp9) && src.ticks >= 4 && src.fmt->iformat && src.fmt->iformat->name &&
                             std::string(src.fmt->iformat->name).find("matroska") != std::string::npos;
        if (src.ticks <= 0 || (!src.exact && !rounded)) continue;
        src.profile = par->profile;
        src.level = par->level;
        src.config.assign(par->extradata, par->extradata + par->extradata_size);
        bool highTier = false;
        if (codec == kHevc && src.config.size() >= 23) {
            highTier = (src.config[1] >> 5) & 1;
            src.level = src.config[12];
            if (exported.profile > 0 && (!hevcProfileFits(src.config[1] & 0x1f, exported.profile) || (highTier && !exported.highTier) ||
                                         src.level > exported.level))
                continue;
        }
        if (codec == kH264) {
            // Its parameter sets beside the others' in the one avcC: none under an id another holds differently.
            if (!parseAvcc(src.config.data(), src.config.size(), src.avcc)) continue;
            bool above = false;
            for (const auto& s : src.avcc.sps)
                above = above || s.size() < 4 || h264ProfileRank(s[1]) == 0 ||
                        (exported.profile > 0 && (h264ProfileRank(s[1]) > h264ProfileRank(exported.profile) || s[3] > exported.level));
            if (above) continue;
            if (lg->firstSource_ >= 0 && src.avcc.lengthSize != lg->lengthSize_) continue;
            for (const auto& u : src.avcc.sps) {
                const hrd::Sps sp = hrd::parseH264Sps(u.data(), u.size());
                if (sp.valid) src.hsps[sp.id] = sp;
            }
            for (const auto& u : src.avcc.pps) {
                const hrd::Pps pp = hrd::parseH264Pps(u.data(), u.size(), src.hsps);
                if (pp.valid) src.hpps[pp.id] = pp;
            }
            bool clash = false;
            for (const auto& s : src.avcc.sps) {
                const int id = h264SpsId(s.data(), s.size());
                if (id < 0 || id > 31 || (spsById.count(id) && spsById[id] != s)) clash = true;
            }
            for (const auto& s : src.avcc.pps) {
                const int id = h264PpsId(s.data(), s.size());
                if (id < 0 || id > 255 || (ppsById.count(id) && ppsById[id] != s)) clash = true;
            }
            if (clash) continue;
            // (last: the first source taken sets the buffer model, so one turned away for another reason must not)
            if (!hrdFits(statedHrd(codec, src.avcc.sps))) continue;
            for (const auto& s : src.avcc.sps) spsById[h264SpsId(s.data(), s.size())] = s;
            for (const auto& s : src.avcc.pps) ppsById[h264PpsId(s.data(), s.size())] = s;
            if (lg->firstSource_ < 0) lg->lengthSize_ = src.avcc.lengthSize;
        } else if (codec == kHevc) {
            std::vector<std::vector<uint8_t>> sets;
            int size = 4;
            if (!parseHvcc(src.config.data(), src.config.size(), sets, size)) continue;
            if (lg->firstSource_ >= 0 && size != lg->lengthSize_) continue;
            if (!hrdFits(statedHrd(codec, sets))) continue;
            if (lg->firstSource_ < 0) lg->lengthSize_ = size;
            for (const auto& s : sets) putNal(src.sets, s.data(), s.size(), size);
        }
        src.held = av_packet_alloc();
        if (!src.held) continue;
        src.usable = true;
        lg->highTier_ = lg->highTier_ || highTier;
        if (lg->firstSource_ < 0) lg->firstSource_ = int(&sp - lg->sources_.data());
    }
    if (lg->firstSource_ < 0) return none("no source coded as the export is");
    if (codec == kAv1) {
        // AV1: one sequence header for the whole stream (the file's av1C states it): footage whose header is the one the
        // export's encoder writes, so the encoded parts beside it share it.
        lg->gop_ = std::max(1, m->gop_size);
        std::string e;
        AVCodecContext* probe = lg->openEncoder(0, &e);
        if (!probe) return none(e);
        const std::vector<uint8_t> ours = av1SequenceHeader(probe->extradata, size_t(std::max(0, probe->extradata_size)));
        avcodec_free_context(&probe);
        if (ours.empty()) return none("the AV1 encoder states no sequence header");
        lg->firstSource_ = -1;
        for (auto& sp : lg->sources_) {
            if (!sp->usable) continue;
            if (av1SequenceHeader(sp->config.data(), sp->config.size()) != ours) {
                sp->usable = false;
                continue;
            }
            if (lg->firstSource_ < 0) lg->firstSource_ = int(&sp - lg->sources_.data());
        }
        if (lg->firstSource_ < 0) return none("no AV1 footage has the sequence header the export's encoder writes");
    }

    // Each run's packets, from the copy point before it to the one after it.
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return none("out of memory");
    std::vector<int64_t> gopLengths;
    for (Run& r : lg->runs_) {
        Source& src = *lg->sources_[size_t(r.source)];
        if (!src.usable || r.len < 2) continue;
        const int64_t s1 = r.s0 + r.len;
        const int64_t target = src.start + std::max<int64_t>(0, r.s0 - 4) * src.ticks;
        if (av_seek_frame(src.fmt, src.stream, target, AVSEEK_FLAG_BACKWARD) < 0 &&
            av_seek_frame(src.fmt, src.stream, src.start, AVSEEK_FLAG_BACKWARD) < 0)
            continue;
        src.nextDts = AV_NOPTS_VALUE;
        av_packet_unref(src.held);
        bool seenCopyPoint = false, bad = false;
        // (its parameter sets, for what each copy point's buffering period asks)
        std::map<int, hrd::Sps> hsps;
        std::map<int, hrd::Pps> hpps;
        if (mode.has) {
            std::vector<std::vector<uint8_t>> sets;
            int size = 4;
            if (codec == kH264) {
                sets = src.avcc.sps;
                sets.insert(sets.end(), src.avcc.pps.begin(), src.avcc.pps.end());
            } else {
                parseHvcc(src.config.data(), src.config.size(), sets, size);
            }
            if (codec == kH264) sets.clear(), hsps = src.hsps, hpps = src.hpps;
            for (const auto& u : sets) {
                hrd::AccessUnit dummy;
                std::vector<uint8_t> one{0, 0, 0, 1};
                one.insert(one.end(), u.begin(), u.end());
                hrd::readAccessUnit(codec, one.data(), one.size(), 0, hsps, hpps, dummy);
            }
        }
        for (;;) {
            av_packet_unref(pkt);
            if (av_read_frame(src.fmt, pkt) < 0) break;
            if (pkt->stream_index != src.stream) continue;
            constexpr int64_t kFar = int64_t(1) << 60;  // (times beyond this are not a frame's)
            if (pkt->pts == AV_NOPTS_VALUE || pkt->dts == AV_NOPTS_VALUE || std::llabs(pkt->pts) > kFar || std::llabs(src.start) > kFar) {
                bad = true;
                break;
            }
            Packet k;
            k.dts = pkt->dts;
            k.pts = pkt->pts;
            k.size = pkt->size;
            k.key = pkt->flags & AV_PKT_FLAG_KEY;
            const int64_t rel = pkt->pts - src.start;
            if (src.exact) {
                k.frame = rel >= 0 ? (rel + src.ticks / 2) / src.ticks : -1 - (-rel - 1 + src.ticks / 2) / src.ticks;
                if (std::llabs(rel - k.frame * src.ticks) > src.ticks / 4) {  // (not on a frame)
                    bad = true;
                    break;
                }
            } else {
                // (rounded times: the nearest frame, whose own time, rounded, is this one give or take a tick)
                const int64_t num = int64_t(src.tb.num) * src.rate.num, den = int64_t(src.tb.den) * src.rate.den;
                k.frame = av_rescale_rnd(rel, num, den, AV_ROUND_NEAR_INF);
                if (std::llabs(rel - av_rescale_rnd(k.frame, den, num, AV_ROUND_NEAR_INF)) > 1) {
                    bad = true;
                    break;
                }
            }
            k.kind = packetKind(codec, pkt->data, size_t(pkt->size), lg->lengthSize_);
            if ((k.kind & kCopyPoint) && seenCopyPoint && k.frame >= s1) break;  // (the GOP after the run's)
            if ((k.kind & kCopyPoint) && codec == kMpeg2) {
                const int64_t d = vbvDelay(pkt->data, size_t(pkt->size));
                k.asked = d >= 0 ? double(d) / 90000 : -1;
                hrd::mpeg2Rate(pkt->data, size_t(pkt->size), k.rate, k.buffer);
            } else if ((k.kind & kCopyPoint) && mode.has) {
                hrd::AccessUnit au;
                if (hrd::readAccessUnit(codec, pkt->data, size_t(pkt->size), lg->lengthSize_, hsps, hpps, au) && au.bp.present) {
                    const auto& d = !au.bp.nalDelay.empty() ? au.bp.nalDelay : au.bp.vclDelay;
                    if (!d.empty()) k.asked = double(d[0]) / 90000;
                }
            }
            seenCopyPoint = seenCopyPoint || (k.kind & kCopyPoint);
            r.packets.push_back(k);
        }
        src.nextDts = AV_NOPTS_VALUE;  // (where the reader is now is not kept)
        if (bad) {
            r.packets.clear();
            continue;
        }
        // GOPs, from each copy point to the next (the scan stopped at the file's end or at a copy point: the last is whole).
        for (size_t i = 0; i < r.packets.size(); ++i) {
            if (!(r.packets[i].kind & kCopyPoint)) continue;
            size_t j = i + 1;
            while (j < r.packets.size() && !(r.packets[j].kind & kCopyPoint)) ++j;
            Gop g;
            g.begin = i, g.end = j;
            g.idr = r.packets[i].kind & kIdr;
            // H.264 from a recovery point: the pictures before its I-picture refer to the GOP before (as HEVC's RASL);
            // entered after other pictures they are left out, which only non-reference ones can be.
            bool leadingRef = false;
            if (codec == kH264 && (r.packets[i].kind & kRecovery))
                for (size_t k = i + 1; k < j; ++k)
                    if (r.packets[k].frame < r.packets[i].frame) {
                        r.packets[k].kind |= kRasl;
                        leadingRef = leadingRef || (r.packets[k].kind & kRef);
                    }
            std::vector<int64_t> all, entry;
            for (size_t k = i; k < j; ++k) {
                all.push_back(r.packets[k].frame);
                if (!(r.packets[k].kind & kRasl)) entry.push_back(r.packets[k].frame);
                g.bytes += r.packets[k].size;
            }
            auto runsOn = [](std::vector<int64_t>& v, int64_t& lo, int64_t& hi) {
                if (v.empty()) return false;
                std::sort(v.begin(), v.end());
                lo = v.front(), hi = v.back();
                return hi - lo + 1 == int64_t(v.size()) && std::adjacent_find(v.begin(), v.end()) == v.end();
            };
            int64_t elo = 0, ehi = 0;
            g.whole = runsOn(all, g.lo, g.hi);
            g.entryWhole = runsOn(entry, elo, ehi) && ehi == g.hi && !leadingRef;
            g.entryLo = elo;
            if (codec == kMpeg2 && !g.idr) {
                // An open GOP with no leading B-pictures needs nothing before it.
                const int64_t first = r.packets[i].frame;
                if (g.lo == first) g.idr = true;
            }
            // At the buffer's rate, started as its first picture asks: how early the bits of each are in (its own
            // pictures alone; a join before it that leaves the buffer lower takes from this).
            g.asked = r.packets[i].asked;
            g.rate = r.packets[i].rate, g.buffer = r.packets[i].buffer;
            const double R = codec == kMpeg2 ? double(m->rc_max_rate) : mode.rate;
            const double fps = av_q2d(m->framerate);
            if (g.asked >= 0 && R > 0 && fps > 0) {
                double need = 0;
                for (size_t k = i; k < j; ++k) {
                    need += double(r.packets[k].size) * 8;
                    g.margin = std::min(g.margin, g.asked + double(k - i) / fps - need / R);
                }
            }
            r.gops.push_back(g);
            gopLengths.push_back(g.hi - g.lo + 1);
            i = j - 1;
        }
        // MPEG-2 at a variable rate: the buffer fills at the stated maximum, a frame's worth between pictures. Counted
        // back from the run's end, the bits that must be there before each picture for it and those after it.
        if (codec == kMpeg2 && !r.gops.empty() && r.gops.front().rate > 0 && m->framerate.num > 0) {
            const double perFrame = r.gops.front().rate / av_q2d(m->framerate);
            std::vector<double> need(r.packets.size() + 1, 0);
            for (size_t k = r.packets.size(); k-- > 0;) {
                const double bits = double(r.packets[k].size) * 8;
                need[k] = std::max(bits, need[k + 1] + bits - perFrame);
            }
            for (Gop& g : r.gops) g.need = need[g.begin];
        }
        // Footage coded at more than the export would spend is encoded, not copied: copied, the export would come out
        // at the footage's rate, whatever its quality or bit rate says.
        int64_t bytes = 0, frames = 0;
        for (const Gop& g : r.gops) bytes += g.bytes, frames += g.hi - g.lo + 1;
        if (frames > 0 && double(bytes) * 8 / double(frames) > lg->ceilingBitsPerFrame()) r.gops.clear();
    }
    av_packet_free(&pkt);
    if (!gopLengths.empty()) {
        std::nth_element(gopLengths.begin(), gopLengths.begin() + gopLengths.size() / 2, gopLengths.end());
        lg->gop_ = int(std::clamp<int64_t>(gopLengths[gopLengths.size() / 2], 1, 600));
    }
    if (encoder.gop > 0) lg->gop_ = encoder.gop;

    std::vector<std::vector<uint8_t>> hrdSets;  // the stream's parameter sets, for the buffer model
    // Which GOPs are copied: wholly inside a run, and decodable where they land.
    FrameTime lastCopyEnd = -2;  // the last copied frame so far
    // MPEG-2: the first GOP copied says how the stream is joined: at a constant rate (it states its vbv_delay), or at a
    // variable one (0xFFFF) with the maximum rate and buffer its sequence header states. GOPs that say otherwise are
    // encoded, so one stream never states two.
    int mpegMode = -1;  // 0: a constant rate; 1: a variable one
    double mpegRate = 0, mpegBuffer = 0;
    for (size_t ri = 0; ri < lg->runs_.size(); ++ri) {
        const Run& r = lg->runs_[ri];
        const int64_t s1 = r.s0 + r.len;
        size_t prev = SIZE_MAX;  // the run's GOP copied just before
        for (size_t gi = 0; gi < r.gops.size(); ++gi) {
            const Gop& g = r.gops[gi];
            Copy c;
            c.run = int(ri);
            c.gop = gi;
            c.bytes = g.bytes;
            bool ok = false;
            if (g.bytes > encoder.maxGopBytes) {
                ok = false;  // (encoded: too large to hold while it is read)
            } else if (prev != SIZE_MAX && prev + 1 == gi && r.e0 + (g.lo - r.s0) == lastCopyEnd + 1) {
                // After the GOP before it, as in the source.
                ok = g.whole && g.lo >= r.s0 && g.hi < s1;
                c.lo = r.e0 + (g.lo - r.s0);
                c.chained = true;
                c.standalone = (codec != kHevc && codec != kH264) || g.idr;
            } else {
                // (HEVC from a CRA, H.264 from a recovery point: entered without what refers to the GOP before)
                const bool hevcCra = (codec == kHevc || codec == kH264) && !g.idr;
                const int64_t lo = hevcCra ? g.entryLo : g.lo;
                ok = (hevcCra ? g.entryWhole : g.whole) && lo >= r.s0 && g.hi < s1;
                c.lo = r.e0 + (lo - r.s0);
                const bool afterCopy = c.lo == lastCopyEnd + 1, atStart = c.lo == 0;
                if (ok && hevcCra) {
                    // A CRA after pictures not its own: its RASL pictures go, and it is marked a broken link (BLA), so
                    // the decoder starts afresh there, as HEVC means splices to be made. H.264's I-picture is made an
                    // IDR picture (and the frame numbers after it counted from it).
                    c.dropRasl = true;
                    c.broken = true;
                } else if (ok && codec == kMpeg2 && !g.idr) {
                    // Its leading B-pictures refer to the frame before them: that frame shown, encoded, just before.
                    ok = !afterCopy && !atStart && g.lo - 1 >= r.s0;
                }
            }
            if (ok && codec == kMpeg2) {
                const bool variable = g.asked < 0;
                if (variable && (g.rate <= 0 || g.buffer < 16384 || g.need > g.buffer)) {
                    ok = false;  // (no maximum stated to keep, or pictures its own buffer could not hold)
                } else if (mpegMode < 0) {
                    mpegMode = variable ? 1 : 0, mpegRate = g.rate, mpegBuffer = g.buffer;
                } else if ((variable ? 1 : 0) != mpegMode || (variable && (g.rate != mpegRate || g.buffer != mpegBuffer))) {
                    ok = false;
                }
            }
            if (!ok) {
                prev = SIZE_MAX;
                continue;
            }
            c.hi = r.e0 + (g.hi - r.s0);
            if (c.dropRasl) {
                c.bytes = 0;
                for (size_t k = g.begin; k < g.end; ++k)
                    if (!(r.packets[k].kind & kRasl)) c.bytes += r.packets[k].size;
            }
            lg->copies_.push_back(c);
            lastCopyEnd = c.hi;
            prev = gi;
        }
    }
    if (lg->copies_.empty()) return none("no whole GOP of a source is untouched");

    // The decode delay: the most any picture is decoded ahead of where it is shown, the footage's or the joins'. The
    // joins' B-pictures reorder no deeper than the footage does (none for footage without, unnested for one deep): an
    // output delay beyond what the footage's own parameter sets allow would break its output timing.
    int footage = 0;
    for (const Copy& c : lg->copies_) {
        const Run& r = lg->runs_[size_t(c.run)];
        const Gop& g = r.gops[c.gop];
        const int64_t first = c.lo - r.e0 + r.s0;  // its first frame shown, in the source
        int64_t i = 0;
        for (size_t k = g.begin; k < g.end; ++k) {
            if (c.dropRasl && (r.packets[k].kind & kRasl)) continue;
            footage = int(std::max<int64_t>(footage, i - (r.packets[k].frame - first)));
            ++i;
        }
    }
    lg->joinBframes_ = footage == 0 ? 0 : 3;
    lg->joinPyramid_ = footage >= 2;
    const int joins = codec == kMpeg2 ? std::max(0, m->max_b_frames) : codec == kAv1 || codec == kVp9 ? 0 : footage == 0 ? 0 : footage == 1 ? 1 : 3;
    lg->depth_ = std::max(joins, footage);
    // H.264: the encoder's parameter sets under an id no source's has.
    if (codec == kH264) {
        for (int id = 0; id < 32; ++id)
            if (!spsById.count(id) && !ppsById.count(id)) {
                lg->spsId_ = id;
                break;
            }
        if (lg->spsId_ < 0) return none("the sources use every parameter set id");
        // The parts at the highest profile and level of the sources copied (so mixed Main and High footage is said to
        // be High), from their parameter sets themselves.
        for (const auto& [id, sps] : spsById)
            if (sps.size() >= 4) {
                if (h264ProfileRank(sps[1]) > h264ProfileRank(lg->profile_)) lg->profile_ = sps[1];
                lg->level_ = std::max(lg->level_, int(sps[3]));
            }
        std::string e;
        AVCodecContext* probe = lg->openEncoder(4000000, &e);
        if (!probe) return none(e);
        Avcc encoderSets;
        for (const auto& [d, n] : annexBNals(probe->extradata, size_t(std::max(0, probe->extradata_size)))) {
            const int type = d[0] & 31;
            if (type == 7 && h264SpsId(d, n) == lg->spsId_) encoderSets.sps.emplace_back(d, d + n);
            if (type == 8 && h264PpsId(d, n) == lg->spsId_) encoderSets.pps.emplace_back(d, d + n);
        }
        avcodec_free_context(&probe);
        if (encoderSets.sps.size() != 1 || encoderSets.pps.size() != 1) return none("x264 did not take the parameter set id");
        if (mode.has) {
            // (the encoded parts state the footage's HRD, so the stream has one model throughout)
            std::vector<uint8_t> spliced = hrd::spliceHrd(kH264, encoderSets.sps[0].data(), encoderSets.sps[0].size(), mode.sps.data(), mode.sps.size());
            if (spliced.empty()) return none("the encoder's parameter sets cannot take the footage's HRD");
            encoderSets.sps[0] = std::move(spliced);
        }
        for (const auto* list : {&encoderSets.sps, &encoderSets.pps})
            for (const auto& s : *list) putNal(lg->encoderSets_, s.data(), s.size(), lg->lengthSize_);
        lg->activeEncoderSets_ = lg->encoderSets_;
        // The decoder configuration: every source's sets and the encoder's.
        Avcc all = lg->sources_[size_t(lg->firstSource_)]->avcc;
        all.sps.clear(), all.pps.clear();
        for (const auto& [id, s] : spsById) all.sps.push_back(s);
        for (const auto& [id, s] : ppsById) all.pps.push_back(s);
        all.sps.push_back(encoderSets.sps[0]);
        all.pps.push_back(encoderSets.pps[0]);
        // Its profile what every set's decoder needs (the highest), its constraints those all of them keep (an intra
        // source's constraint does not hold for the encoded parts' P and B pictures), its level the highest.
        all.profile = 0, all.compat = 0xff, all.level = 0;
        for (const auto& sps : all.sps)
            if (sps.size() >= 4) {
                if (h264ProfileRank(sps[1]) > h264ProfileRank(all.profile)) all.profile = sps[1];
                all.compat &= sps[2];
                all.level = std::max(all.level, sps[3]);
            }
        // (raised to a High profile above the first source's, the configuration takes the extension those carry)
        std::vector<std::vector<uint8_t>> tails;
        for (const auto& sp : lg->sources_)
            if (sp->usable) tails.push_back(sp->avcc.tail);
        all.tail = avccHighExtension(all.profile, m->pix_fmt, tails);
        lg->profile_ = all.profile, lg->level_ = all.level;
        if (all.sps.size() > 31 || all.pps.size() > 255) return none("too many parameter sets");
        hrdSets = all.sps;
        hrdSets.insert(hrdSets.end(), all.pps.begin(), all.pps.end());
        lg->sources_[size_t(lg->firstSource_)]->config = buildAvcc(all);
    }
    if (codec == kHevc) {
        lg->firstSource_ = lg->runs_[size_t(lg->copies_.front().run)].source;
        // Every set out of band (hvc1, as Apple's players need) when all the copies are one source's and nothing is
        // encoded; else the sets go in the video (hev1), only when asked for.
        FrameTime covered = 0;
        bool oneSource = true;
        for (const Copy& c : lg->copies_) {
            covered += c.hi - c.lo + 1;
            oneSource = oneSource && lg->runs_[size_t(c.run)].source == lg->firstSource_;
        }
        lg->hvc1_ = oneSource && covered == lg->frames_;
        if (!lg->hvc1_ && !encoder.inBandHevc)
            return none("HEVC copied beside encoded parts needs its parameter sets in the video (hev1), which Apple's players refuse");
        for (const auto& sp : lg->sources_)
            if (sp->usable) lg->level_ = std::max(lg->level_, sp->level);
        lg->profile_ = lg->sources_[size_t(lg->firstSource_)]->profile;
        // (the encoded parts' SPS must be able to state the footage's HRD, or the stream would have two models: x265
        // writes timing information with hrd=1, and one sub-layer, so footage stating more is not copied)
        if (mode.has && hrd::parseHevcSps(mode.sps.data(), mode.sps.size()).maxSubLayersMinus1 != 0)
            return none("the encoder's parameter sets cannot take the footage's HRD (it states sub-layers)");
    } else if (codec == kMpeg2) {
        lg->profile_ = lg->sources_[size_t(lg->firstSource_)]->profile;
        lg->level_ = lg->sources_[size_t(lg->firstSource_)]->level;
    }
    // The buffer model kept across the joins.
    const double fps = av_q2d(m->framerate);
    if (mode.has && codec != kMpeg2) {
        if (codec == kHevc) {
            int size = 4;
            parseHvcc(lg->sources_[size_t(lg->firstSource_)]->config.data(), lg->sources_[size_t(lg->firstSource_)]->config.size(), hrdSets, size);
        }
        lg->hrdOn_ = lg->hrd_.start(codec, hrdSets, lg->lengthSize_, fps, lg->depth_);
        lg->hrdSps_ = mode.sps;
    } else if (codec == kMpeg2 && mpegMode == 1) {
        // A variable rate: the footage's stated maximum and buffer kept by every part, every vbv_delay 0xFFFF, and each
        // copy asking for the bits its pictures need there (as seconds at the rate, what a constant rate's delay is).
        lg->hrdOn_ = lg->hrd_.start(codec, {}, 0, fps, lg->depth_, mpegRate, mpegBuffer, false);
        if (lg->hrdOn_)
            for (Run& r : lg->runs_)
                for (Gop& g : r.gops)
                    if (g.asked < 0) g.asked = g.need / mpegRate, g.margin = 0;
    } else if (codec == kMpeg2 && m->rc_max_rate > 0 && m->rc_max_rate == m->rc_min_rate && m->rc_buffer_size >= 16384) {
        // (only footage that states its delays: a variable-rate source's 0xFFFF would be made into a constant-rate claim)
        bool stated = true;
        for (const Copy& c : lg->copies_) stated = stated && lg->runs_[size_t(c.run)].gops[c.gop].asked >= 0;
        const double buffer = std::floor(double(m->rc_buffer_size) / 16384) * 16384;
        if (stated) lg->hrdOn_ = lg->hrd_.start(codec, {}, 0, fps, lg->depth_, double(m->rc_max_rate), buffer);
    }
    if (lg->hrdOn_) {
        lg->hrdRate_ = lg->hrd_.bitRate(), lg->hrdBuffer_ = lg->hrd_.bufferBits();
    }
    lg->pkt_ = av_packet_alloc();
    if (!lg->pkt_) return none("out of memory");
    return lg;
}

bool LongGopCopier::prepareStream(std::string* error) {
    AVCodecParameters* par = vst_->codecpar;
    if (codec_ == kAv1) {
        // (the footage's av1C: its sequence header is the encoder's too)
        const std::vector<uint8_t>& config = sources_[size_t(firstSource_)]->config;
        av_freep(&par->extradata);
        par->extradata_size = 0;
        par->extradata = static_cast<uint8_t*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) return fail(error, "Out of memory");
        std::memcpy(par->extradata, config.data(), config.size());
        par->extradata_size = int(config.size());
        return true;
    }
    if (codec_ == kVp9) {
        // (VP9 has no parameter sets: every key frame states its own profile, depth and size, and the file's
        // description is written from the export's encoder, which the footage was matched to)
        return true;
    }
    if (codec_ == kH264 || codec_ == kHevc) {
        const Source& first = *sources_[size_t(firstSource_)];
        const std::vector<uint8_t>& config = first.config;
        av_freep(&par->extradata);
        par->extradata_size = 0;
        par->extradata = static_cast<uint8_t*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) return fail(error, "Out of memory");
        std::memcpy(par->extradata, config.data(), config.size());
        par->extradata_size = int(config.size());
        par->profile = codec_ == kH264 && profile_ > 0 ? profile_ : first.profile;
        par->level = level_ > 0 ? level_ : first.level;
        if (codec_ == kHevc) {
            par->codec_tag = hvc1_ ? MKTAG('h', 'v', 'c', '1') : MKTAG('h', 'e', 'v', '1');
            // (the tier and level all the sources fit in)
            if (par->extradata_size > 12) {
                par->extradata[1] = uint8_t((par->extradata[1] & ~0x20) | (highTier_ ? 0x20 : 0));
                if (level_ > 0) par->extradata[12] = uint8_t(std::min(level_, 255));
            }
            if (!hvc1_) {
                // (in the video too: the arrays not complete)
                size_t i = 23;
                for (int a = 0, arrays = par->extradata_size > 22 ? par->extradata[22] : 0; a < arrays && i + 3 <= size_t(par->extradata_size); ++a) {
                    par->extradata[i] &= 0x7f;
                    const int count = par->extradata[i + 1] << 8 | par->extradata[i + 2];
                    i += 3;
                    for (int k = 0; k < count && i + 2 <= size_t(par->extradata_size); ++k) i += 2 + (size_t(par->extradata[i]) << 8 | par->extradata[i + 1]);
                }
            }
        }
    }
    return true;
}

// ---- Writing ----

bool LongGopCopier::write(AVPacket* pkt, FrameTime display, bool ours, std::string* error) {
    if (!hrdOn_) return writeFinal(pkt, display, error);
    // Through the buffer model: each access unit held until the next is known (its filler, before a join, depends on
    // what the next asks for), its picture timing and buffering period rewritten for its place.
    int64_t shown = 0;
    std::vector<uint8_t> before = hrd_.push(std::vector<uint8_t>(pkt->data, pkt->data + pkt->size), display, ours, true, &shown);
    const int flags = heldFlags_;
    heldFlags_ = pkt->flags & (AV_PKT_FLAG_KEY | AV_PKT_FLAG_DISPOSABLE);
    if (before.empty()) return true;
    AVPacket* out = av_packet_alloc();
    if (!out || av_new_packet(out, int(before.size())) < 0) {
        av_packet_free(&out);
        return fail(error, "Out of memory");
    }
    std::memcpy(out->data, before.data(), before.size());
    out->flags = flags;
    const bool ok = writeFinal(out, FrameTime(shown), error);
    av_packet_free(&out);
    return ok;
}

bool LongGopCopier::writeHeld(std::vector<uint8_t> au, FrameTime display, std::string* error) {
    if (au.empty()) return true;
    AVPacket* out = av_packet_alloc();
    if (!out || av_new_packet(out, int(au.size())) < 0) {
        av_packet_free(&out);
        return fail(error, "Out of memory");
    }
    std::memcpy(out->data, au.data(), au.size());
    out->flags = heldFlags_;
    const bool ok = writeFinal(out, display, error);
    av_packet_free(&out);
    return ok;
}

std::vector<uint8_t> LongGopCopier::withHrd(const uint8_t* sps, size_t n) const {
    if (hrdSps_.empty()) return std::vector<uint8_t>(sps, sps + n);
    std::vector<uint8_t> out = hrd::spliceHrd(codec_, sps, n, hrdSps_.data(), hrdSps_.size());
    return out.empty() ? std::vector<uint8_t>(sps, sps + n) : out;
}

void LongGopCopier::setsWithHrd(std::vector<uint8_t>& sets) const {
    if (hrdSps_.empty()) return;
    std::vector<uint8_t> out;
    forEachNal(sets.data(), sets.size(), lengthSize_, [&](const uint8_t* nal, size_t len) {
        const int type = codec_ == kH264 ? (nal[0] & 31) : (nal[0] >> 1) & 0x3f;
        if (type == (codec_ == kH264 ? 7 : 33)) {
            const std::vector<uint8_t> v = withHrd(nal, len);
            putNal(out, v.data(), v.size(), lengthSize_);
        } else {
            putNal(out, nal, len, lengthSize_);
        }
    });
    sets = std::move(out);
}

bool LongGopCopier::writeFinal(AVPacket* pkt, FrameTime display, std::string* error) {
    const AVRational tb = enc_.main->time_base;
    pkt->pts = display;
    pkt->dts = decoded_++ - depth_;
    if (pkt->dts > pkt->pts) return fail(error, "Smart rendering: a picture would be decoded after it is shown");
    pkt->duration = 1;
    pkt->pos = -1;
    pkt->stream_index = vst_->index;
    av_packet_rescale_ts(pkt, tb, vst_->time_base);
    const int rc = av_interleaved_write_frame(oc_, pkt);
    if (rc < 0) return fail(error, "Writing video failed: " + averr(rc));
    return true;
}

int64_t LongGopCopier::partBitrate(FrameTime a, FrameTime b) const {
    // The copied GOPs either side of [a, b): their bits a frame at the export's rate.
    const double fps = av_q2d(enc_.main->framerate);
    int64_t bytes = 0, frames = 0;
    for (const Copy& c : copies_)
        if (c.hi + 1 == a || c.lo == b) bytes += c.bytes, frames += c.hi - c.lo + 1;
    if (frames == 0)
        for (const Copy& c : copies_) bytes += c.bytes, frames += c.hi - c.lo + 1;
    if (frames == 0) return 4000000;
    return std::clamp<int64_t>(int64_t(double(bytes) * 8.0 / double(frames) * fps), 100000, 800000000);
}

AVCodecContext* LongGopCopier::openEncoder(int64_t bitrate, std::string* error) const {
    const AVCodecContext* m = enc_.main;
    const AVCodec* codec = avcodec_find_encoder_by_name(m->codec->name);
    AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!c) {
        fail(error, std::string("Video encoder not available: ") + m->codec->name);
        return nullptr;
    }
    c->width = m->width;
    c->height = m->height;
    c->time_base = m->time_base;
    c->framerate = m->framerate;
    c->sample_aspect_ratio = m->sample_aspect_ratio;
    c->pix_fmt = m->pix_fmt;
    c->color_primaries = m->color_primaries;
    c->color_trc = m->color_trc;
    c->colorspace = m->colorspace;
    c->color_range = m->color_range;
    c->chroma_sample_location = m->chroma_sample_location;
    c->field_order = m->field_order;
    c->flags |= m->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME);
    c->thread_count = 0;
    c->gop_size = gop_;
    AVDictionary* opts = nullptr;
    // The footage's buffer model: the encoder told the buffer stands where the stream's does, less what the copy after
    // the part asks to find there, and a buffer that much smaller (so, keeping to its own, the part leaves the stream's
    // at least that full).
    int64_t vbvRate = 0, vbvSize = 0, vbvInit = 0;
    if (hrdOn_) {
        const double R = hrdRate_, B = hrdBuffer_;
        const double level = std::clamp(hrd_.nextDelay() * R, 0.05 * B, B);
        double ask = 0;
        if (cursor_ < copies_.size()) ask = std::max(0.0, runs_[size_t(copies_[cursor_].run)].gops[copies_[cursor_].gop].asked) * R;
        const double keep = std::clamp(ask, 0.0, 0.7 * level);
        vbvRate = int64_t(R);
        vbvSize = int64_t(B - keep);
        vbvInit = std::clamp<int64_t>(int64_t(level - keep), 1, vbvSize);
    }
    if (codec_ == kH264 || codec_ == kHevc) {
        // The export's quality, held under the rate of the GOPs copied either side (over a second). (Aiming at that
        // rate instead overshoots it several times in a part this short, its rate control starting low.)
        if (enc_.bitrate > 0) c->bit_rate = std::min(enc_.bitrate, bitrate);
        else av_dict_set_int(&opts, "crf", enc_.crf, 0);
        c->rc_max_rate = bitrate;
        c->rc_buffer_size = int(std::min<int64_t>(bitrate, INT_MAX));
        if (hrdOn_) {
            if (enc_.bitrate > 0) c->bit_rate = std::min<int64_t>(c->bit_rate, vbvRate);
            c->rc_max_rate = vbvRate;
            c->rc_buffer_size = int(std::min<int64_t>(vbvSize, INT_MAX));
            c->rc_initial_buffer_occupancy = int(std::min<int64_t>(vbvInit, INT_MAX));
        }
    }
    if (codec_ == kH264) {
        // Closed GOPs (x264's own), the source's profile and level, its parameter sets under their own id, kept out of
        // the pictures (they go in the decoder configuration).
        c->max_b_frames = joinBframes_;
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (const char* profile = x264Profile(profile_)) av_dict_set(&opts, "profile", profile, 0);
        if (level_ > 0) c->level = level_;
        if (!enc_.preset.empty()) av_dict_set(&opts, "preset", enc_.preset.c_str(), 0);
        av_dict_set(&opts, "x264-params", ("sps-id=" + std::to_string(spsId_) + (joinPyramid_ ? "" : ":b-pyramid=none")).c_str(), 0);
    } else if (codec_ == kHevc) {
        c->max_b_frames = joinBframes_;
        std::string params = enc_.x265Params.empty() ? std::string("log-level=error") : enc_.x265Params;
        // (closed GOPs; no settings text with each keyframe's repeated headers)
        params += ":open-gop=0:repeat-headers=1:info=0:bframes=" + std::to_string(joinBframes_) + (joinPyramid_ ? "" : ":b-pyramid=0") +
                  ":keyint=" + std::to_string(gop_);
        if (level_ > 0) {
            char lv[32];
            std::snprintf(lv, sizeof lv, ":level-idc=%g", level_ / 30.0);
            params += lv;
        }
        params += highTier_ ? ":high-tier=1" : ":high-tier=0";  // (the sources' tier)
        if (hrdOn_) {
            char vbv[160];
            // (hrd=1: x265 then writes the timing information in the SPS that the footage's HRD is put in)
            std::snprintf(vbv, sizeof vbv, ":hrd=1:vbv-maxrate=%lld:vbv-bufsize=%lld:vbv-init=%.4f", static_cast<long long>(vbvRate / 1000),
                          static_cast<long long>(vbvSize / 1000), double(vbvInit) / double(vbvSize));
            params += vbv;
        }
        av_dict_set(&opts, "x265-params", params.c_str(), 0);
        if (!enc_.preset.empty()) av_dict_set(&opts, "preset", enc_.preset.c_str(), 0);
    } else if (codec_ == kAv1) {
        // AV1: the export's encoder at its quality (or rate) and speed, its closed GOPs (key frames).
        c->flags |= m->flags & AV_CODEC_FLAG_GLOBAL_HEADER;
        if (enc_.bitrate > 0) c->bit_rate = enc_.bitrate;
        else av_dict_set_int(&opts, "crf", enc_.crf, 0);
        if (!enc_.preset.empty() && std::string(m->codec->name) == "libsvtav1") av_dict_set(&opts, "preset", enc_.preset.c_str(), 0);
    } else if (codec_ == kVp9) {
        // VP9: libvpx as the export runs it (Exporter's settings), at its quality or rate. Each part starts on a key
        // frame, and libvpx's hidden frames come inside the packet of the frame shown after them.
        c->flags |= m->flags & AV_CODEC_FLAG_GLOBAL_HEADER;
        c->profile = m->profile;
        if (enc_.bitrate > 0) {
            c->bit_rate = enc_.bitrate;
        } else {
            av_dict_set_int(&opts, "crf", enc_.crf, 0);
            c->bit_rate = 0;
        }
        av_dict_set(&opts, "row-mt", "1", 0);
        av_dict_set(&opts, "deadline", enc_.preset.empty() ? "good" : enc_.preset.c_str(), 0);
        av_dict_set(&opts, "cpu-used", "4", 0);
    } else {
        // MPEG-2: the export's own rate control (XDCAM HD422's constant rate) and GOP.
        c->bit_rate = m->bit_rate;
        c->rc_min_rate = m->rc_min_rate;
        c->rc_max_rate = m->rc_max_rate;
        c->rc_buffer_size = m->rc_buffer_size;
        c->rc_initial_buffer_occupancy = m->rc_initial_buffer_occupancy;
        c->gop_size = enc_.gop > 0 ? enc_.gop : m->gop_size;
        c->max_b_frames = m->max_b_frames;
        // At a constant rate, the buffer as the copied GOP before left it (so the stream's buffer model runs on across
        // the splice), and the GOP headers' time codes where the part sits in the export.
        if (vbvLevel_ >= 0 && m->rc_buffer_size > 0) c->rc_initial_buffer_occupancy = int(std::clamp<int64_t>(vbvLevel_, 1, m->rc_buffer_size));
        if (hrdOn_) {
            // (its sequence headers state the stream's buffer again, and its vbv_delays are set, as it is joined)
            c->rc_buffer_size = int(std::min<int64_t>(vbvSize, INT_MAX));
            c->rc_initial_buffer_occupancy = int(std::min<int64_t>(vbvInit, INT_MAX));
        }
        if (hrdOn_ && !hrd_.cbr()) {
            // A variable rate: no more than the footage's stated maximum, at about what the GOPs copied either side
            // spend (never so much that a frame's worth is more than the buffer the part is given).
            const double fps = m->framerate.num > 0 ? av_q2d(m->framerate) : 25.0;
            c->rc_max_rate = vbvRate;
            c->rc_min_rate = 0;
            const int64_t most = std::min<int64_t>(int64_t(0.9 * double(vbvRate)), int64_t(0.5 * double(vbvSize) * fps));
            c->bit_rate = std::clamp<int64_t>(bitrate > 0 ? bitrate : m->bit_rate, std::min<int64_t>(100000, most), most);
        }
        int f[4] = {};
        av_dict_set(&opts, "gop_timecode", timecodeText(m->framerate, timecodeStart_, dropFrame_, partStart_, f).c_str(), 0);
        c->intra_dc_precision = m->intra_dc_precision;
        c->qmin = m->qmin;
        c->qmax = m->qmax;
        av_dict_set(&opts, "intra_vlc", "1", 0);
        av_dict_set(&opts, "non_linear_quant", "1", 0);
    }
    const int rc = avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        avcodec_free_context(&c);
        fail(error, "Cannot open the video encoder for the parts around edits: " + averr(rc));
        return nullptr;
    }
    return c;
}

bool LongGopCopier::flushPending(std::string* error) {
    if (!havePending_) return true;
    havePending_ = false;
    AVPacket* out = av_packet_alloc();
    if (!out || av_new_packet(out, int(pending_.size())) < 0) {
        av_packet_free(&out);
        return fail(error, "Out of memory");
    }
    std::memcpy(out->data, pending_.data(), pending_.size());
    if (pendingKey_) out->flags |= AV_PKT_FLAG_KEY;
    const bool ok = write(out, pendingPts_, true, error);
    av_packet_free(&out);
    return ok;
}

bool LongGopCopier::receive(std::string* error) {
    for (;;) {
        const int rc = avcodec_receive_packet(ctx_, pkt_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) return fail(error, "Video encoding failed: " + averr(rc));
        if (!flushPending(error)) {
            av_packet_unref(pkt_);
            return false;
        }
        pending_.clear();
        if (codec_ == kH264 || codec_ == kHevc) {
            // A part whose sets are not the configuration's gives them in its first picture (so a seek into it decodes),
            // as does one after such a part (whose sets the decoder would hold).
            if (codec_ == kH264 && firstOfPart_ && (partSets_ != encoderSets_ || activeEncoderSets_ != partSets_))
                pending_ = partSets_, activeEncoderSets_ = partSets_;
            if (!toLengthPrefixed(pkt_->data, size_t(pkt_->size), lengthSize_, pending_)) {
                av_packet_unref(pkt_);
                return fail(error, "Smart rendering: a coded picture is too large for the source's NAL unit lengths");
            }
            if (codec_ == kHevc) setsWithHrd(pending_);  // (its SPS in the video: the footage's HRD in it)
        } else {
            pending_.assign(pkt_->data, pkt_->data + pkt_->size);
        }
        firstOfPart_ = false;
        pendingPts_ = partStart_ + pkt_->pts;
        pendingKey_ = pkt_->flags & AV_PKT_FLAG_KEY;
        havePending_ = true;
        av_packet_unref(pkt_);
    }
}

bool LongGopCopier::startPart(FrameTime rel, std::string* error) {
    partStart_ = rel;
    partEnd_ = cursor_ < copies_.size() ? copies_[cursor_].lo : frames_;
    ctx_ = openEncoder(codec_ == kMpeg2 && !(hrdOn_ && !hrd_.cbr()) ? 0 : partBitrate(partStart_, partEnd_), error);
    if (!ctx_) return false;
    firstOfPart_ = true;
    partSets_.clear();
    if (codec_ == kH264) {
        for (const auto& [d, n] : annexBNals(ctx_->extradata, size_t(std::max(0, ctx_->extradata_size)))) {
            const int type = d[0] & 31;
            if (type == 7 || type == 8) putNal(partSets_, d, n, lengthSize_);
        }
        setsWithHrd(partSets_);
    }
    ++parts_;
    return true;
}

bool LongGopCopier::endPart(std::string* error) {
    if (!ctx_) return true;
    int rc = avcodec_send_frame(ctx_, nullptr);
    if (rc < 0) return fail(error, "Video encoding failed: " + averr(rc));
    if (!receive(error)) return false;
    avcodec_free_context(&ctx_);
    return flushPending(error);
}

bool LongGopCopier::encode(AVFrame* frame, FrameTime rel, std::string* error) {
    if (!ctx_ && !startPart(rel, error)) return false;
    frame->pts = rel - partStart_;
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    const int rc = avcodec_send_frame(ctx_, frame);
    if (rc < 0) return fail(error, "Video encoding failed: " + averr(rc));
    if (!receive(error)) return false;
    if (rel + 1 >= partEnd_) return endPart(error);
    return true;
}

bool LongGopCopier::finish(std::string* error) {
    if (!endPart(error)) return false;
    if (!hrdOn_ || !hrd_.holding()) return true;
    int64_t shown = 0;
    std::vector<uint8_t> last = hrd_.finish(&shown);
    return writeHeld(std::move(last), FrameTime(shown), error);
}

int LongGopCopier::copy(FrameTime rel, std::string* error) {
    while (cursor_ < copies_.size() && copies_[cursor_].hi < rel) ++cursor_;
    if (cursor_ >= copies_.size() || rel < copies_[cursor_].lo) return 0;
    if (rel == copies_[cursor_].lo) {
        if (!endPart(error)) return -1;  // (a part ends where a copy starts; done already unless frames were skipped)
        // The buffer model: a copy after a join decodes as its footage did only when the buffer stands at least where
        // its first picture asks (less what its pictures can spare); lower, its frames are encoded instead (a part
        // then starts there, its buffering period where the buffer stands). Higher, the picture before is given filler.
        const Copy& here = copies_[cursor_];
        const bool join = !(here.chained && lastCopiedHi_ + 1 == here.lo);
        bool declined = false;
        if (hrdOn_ && join && !hvc1_ && hrd_.holding()) {
            const Gop& g = runs_[size_t(here.run)].gops[here.gop];
            double margin = g.margin;
            for (size_t n = cursor_ + 1; n < copies_.size() && copies_[n].chained && copies_[n].lo == copies_[n - 1].hi + 1; ++n)
                margin = std::min(margin, runs_[size_t(copies_[n].run)].gops[copies_[n].gop].margin);
            declined = g.asked >= 0 && hrd_.nextDelay() < g.asked - 0.8 * std::clamp(margin, 0.0, g.asked);
        }
        std::vector<AVPacket*> gop;
        const int got = declined ? 0 : readGop(copies_[cursor_], gop, error);
        if (got < 0) return -1;
        if (got == 0 && hvc1_) {
            // (an encoded part's sets cannot go beside the source's out of band: the export is made again, encoded)
            fail(error, "Smart rendering: the HEVC footage was not found as it was planned (did the file change?)");
            return -2;
        }
        if (got == 0) {
            // Not found again as it was planned (a seek landing elsewhere, the file changed): its frames are encoded
            // instead, with the copies after it that decode only on from it.
            size_t n = 1;
            while (cursor_ + n < copies_.size() && copies_[cursor_ + n].chained && !copies_[cursor_ + n].standalone) ++n;
            copies_.erase(copies_.begin() + std::ptrdiff_t(cursor_), copies_.begin() + std::ptrdiff_t(cursor_ + n));
            return 0;
        }
        const bool ok = writeGop(copies_[cursor_], gop, error);
        for (AVPacket*& k : gop) av_packet_free(&k);
        if (!ok) return -1;
        lastCopiedHi_ = copies_[cursor_].hi;
    }
    return 1;
}

int LongGopCopier::readGop(const Copy& c, std::vector<AVPacket*>& out, std::string* error) {
    const Run& r = runs_[size_t(c.run)];
    const Gop& g = r.gops[c.gop];
    Source& src = *sources_[size_t(r.source)];
    auto lost = [&] {
        for (AVPacket*& k : out) av_packet_free(&k);
        out.clear();
        av_packet_unref(src.held);
        src.nextDts = AV_NOPTS_VALUE;
        return 0;
    };
    const int64_t want = r.packets[g.begin].dts;
    // To the GOP's first packet: read on when the reader is there, else seek (to it, or before it and read on).
    if (src.nextDts != want) {
        av_packet_unref(src.held);
        bool found = false;
        for (int attempt = 0; attempt < 3 && !found; ++attempt) {
            const int64_t target = attempt == 2 ? src.start : want - int64_t(attempt) * int64_t(gop_ + 4) * src.ticks;
            if (av_seek_frame(src.fmt, src.stream, target, AVSEEK_FLAG_BACKWARD) < 0) continue;
            for (;;) {
                av_packet_unref(src.held);
                if (av_read_frame(src.fmt, src.held) < 0) break;
                if (src.held->stream_index != src.stream) continue;
                if (src.held->dts == want) {
                    found = true;
                    break;
                }
                if (src.held->dts > want) break;  // (landed after it)
            }
        }
        if (!found) return lost();
    }
    // The whole GOP read and checked against the plan before any of it is written.
    for (size_t k = g.begin; k < g.end; ++k) {
        if (!src.held->data) {
            for (;;) {
                if (av_read_frame(src.fmt, src.held) < 0) return lost();
                if (src.held->stream_index == src.stream) break;
                av_packet_unref(src.held);
            }
        }
        const Packet& pk = r.packets[k];
        if (src.held->dts != pk.dts || src.held->size != pk.size) return lost();
        AVPacket* kept = av_packet_alloc();
        if (!kept) {
            lost();
            fail(error, "Out of memory");
            return -1;
        }
        av_packet_move_ref(kept, src.held);
        out.push_back(kept);
    }
    src.nextDts = g.end < r.packets.size() ? r.packets[g.end].dts : AV_NOPTS_VALUE;
    return 1;
}

bool LongGopCopier::writeGop(const Copy& c, const std::vector<AVPacket*>& gop, std::string* error) {
    const Run& r = runs_[size_t(c.run)];
    const Gop& g = r.gops[c.gop];
    const Source& src = *sources_[size_t(r.source)];
    bool first = true;
    std::vector<uint8_t> trimmed, renumbered;
    if (!c.chained || lastCopiedHi_ + 1 != c.lo) frameNumShift_ = -1;  // (after a join: its own numbers, unless made IDR)
    for (size_t k = g.begin; k < g.end; ++k) {
        AVPacket* in = gop[k - g.begin];
        const Packet& pk = r.packets[k];
        if (c.dropRasl && (pk.kind & kRasl)) continue;
        const FrameTime display = r.e0 + (pk.frame - r.s0);
        // What the export did not ask for left out (captions, HDR10+, AFD, Dolby Vision's units).
        const bool less = withoutExtras(codec_, in->data, size_t(in->size), lengthSize_, trimmed);
        const uint8_t* body = less ? trimmed.data() : in->data;
        size_t bodySize = less ? trimmed.size() : size_t(in->size);
        if (codec_ == kH264 && ((first && c.broken) || frameNumShift_ >= 0)) {
            // From a recovery point after pictures not its own: its I-picture made an IDR picture, the pictures after
            // it numbered from it (until the next IDR picture). The slice data is the source's, bit for bit.
            renumbered.clear();
            bool idr = false, bad = false, shiftSet = false;
            const bool makeIdr = first && c.broken;
            forEachNal(body, bodySize, lengthSize_, [&](const uint8_t* nal, size_t len) {
                const int type = nal[0] & 31;
                if (type == 5) idr = true;
                std::vector<uint8_t> v;
                if (type == 1 && makeIdr) {
                    if (!shiftSet) {
                        H264Slice sl;
                        const bool read = readH264Slice(nal, len, src.hsps, src.hpps, sl, false);
                        frameNumShift_ = read ? int64_t(sl.frameNum) : 0;
                        pocShift_ = read ? sl.pocLsb : 0;
                        sinceIdr_ = 0;
                        shiftSet = true;
                    }
                    v = h264SliceAsIdr(nal, len, src.hsps, src.hpps, idrPicId_);
                    bad = bad || v.empty();
                } else if (type == 1 && frameNumShift_ >= 0 && !idr) {
                    H264Renumber rn{uint64_t(frameNumShift_), pocShift_, sinceIdr_};
                    v = h264SliceRenumbered(nal, len, src.hsps, src.hpps, rn);
                    sinceIdr_ = rn.sinceIdr;
                    bad = bad || v.empty();
                }
                if (v.empty()) putNal(renumbered, nal, len, lengthSize_);
                else putNal(renumbered, v.data(), v.size(), lengthSize_);
            });
            if (bad) return fail(error, "Smart rendering: an H.264 slice from a recovery point could not be rewritten");
            if (makeIdr) idrPicId_ = idrPicId_ >= 0xffff ? 2 : idrPicId_ + 1;
            if (idr && !makeIdr) frameNumShift_ = -1;
            trimmed.swap(renumbered);
            body = trimmed.data();
            bodySize = trimmed.size();
        }
        // HEVC in the video (hev1): the source's parameter sets in front of every copied GOP, so it decodes whatever the
        // decoder held before (another source's, or an encoded part's under the same ids, as after a seek).
        const bool prefix = first && codec_ == kHevc && !hvc1_;
        const size_t lead = prefix ? src.sets.size() : 0;
        AVPacket* out = av_packet_alloc();
        if (!out || av_new_packet(out, int(lead + bodySize)) < 0) {
            av_packet_free(&out);
            return fail(error, "Out of memory");
        }
        if (lead) std::memcpy(out->data, src.sets.data(), lead);
        std::memcpy(out->data + lead, body, bodySize);
        out->flags = in->flags & (AV_PKT_FLAG_KEY | AV_PKT_FLAG_DISPOSABLE);
        if (first && c.broken && codec_ == kH264) out->flags |= AV_PKT_FLAG_KEY;
        if (first && c.broken && codec_ == kHevc) {
            // The CRA's slices as BLA_W_RADL (its RASL pictures are left out): only the NAL unit type changes.
            forEachNal(out->data, size_t(out->size), lengthSize_, [](const uint8_t* nal, size_t len) {
                if (len >= 2 && ((nal[0] >> 1) & 0x3f) == 21) const_cast<uint8_t*>(nal)[0] = uint8_t(17 << 1 | (nal[0] & 0x81));
            });
        }
        if (first && codec_ == kMpeg2) {
            // The GOP's time code where it now sits (its first frame shown), as the encoded parts' run.
            int f[4] = {};
            timecodeText(enc_.main->framerate, timecodeStart_, dropFrame_, c.lo, f);
            setGopTimecode(out->data, size_t(out->size), f[0], f[1], f[2], f[3]);
        }
        if (codec_ == kMpeg2) {
            // At a constant rate: the buffer after this picture, for an encoded part that may follow.
            const int64_t delay = vbvDelay(out->data, size_t(out->size));
            const AVCodecContext* m = enc_.main;
            if (delay >= 0 && m->rc_max_rate > 0 && m->rc_max_rate == m->rc_min_rate && m->framerate.num > 0)
                vbvLevel_ = delay * m->rc_max_rate / 90000 - int64_t(out->size) * 8 + int64_t(double(m->rc_max_rate) / av_q2d(m->framerate));
            else
                vbvLevel_ = -1;
        }
        const bool ok = write(out, display, false, error);
        av_packet_free(&out);
        if (!ok) return false;
        first = false;
        ++copied_;
    }
    return true;
}

}  // namespace montage
