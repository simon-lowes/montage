// Montage — the buffer models of H.264, HEVC and MPEG-2 (see HrdCheck.h).
#include "HrdCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "render/NalBits.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace montage {

namespace {

constexpr int kH264 = AV_CODEC_ID_H264, kHevc = AV_CODEC_ID_HEVC, kMpeg2 = AV_CODEC_ID_MPEG2VIDEO;
constexpr size_t kKeptIssues = 100;

std::string format(const char* f, double a = 0, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

// ---- NAL units of an access unit ----

using Nals = std::vector<std::vector<uint8_t>>;

// Length-prefixed units (lengthSize 1-4), or Annex B (lengthSize 0).
Nals splitNals(const uint8_t* d, size_t n, int lengthSize) {
    Nals out;
    if (lengthSize > 0) {
        size_t i = 0;
        while (i + size_t(lengthSize) <= n) {
            size_t len = 0;
            for (int k = 0; k < lengthSize; ++k) len = len << 8 | d[i + size_t(k)];
            i += size_t(lengthSize);
            if (len == 0 || i + len > n) break;
            out.emplace_back(d + i, d + i + len);
            i += len;
        }
        return out;
    }
    size_t i = 0, start = SIZE_MAX;
    auto close = [&](size_t end) {
        if (start == SIZE_MAX) return;
        while (end > start && d[end - 1] == 0) --end;
        if (end > start) out.emplace_back(d + start, d + end);
    };
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            close(i);
            i += 3;
            start = i;
            continue;
        }
        ++i;
    }
    close(n);
    return out;
}

std::vector<uint8_t> joinNals(const Nals& nals, int lengthSize) {
    std::vector<uint8_t> out;
    for (const auto& u : nals) {
        if (lengthSize > 0) {
            for (int k = lengthSize - 1; k >= 0; --k) out.push_back(uint8_t(u.size() >> (8 * k)));
        } else {
            out.insert(out.end(), {0, 0, 0, 1});
        }
        out.insert(out.end(), u.begin(), u.end());
    }
    return out;
}

int nalType(int codec, const std::vector<uint8_t>& u) {
    if (u.empty()) return -1;
    return codec == kH264 ? (u[0] & 31) : (u[0] >> 1) & 0x3f;
}
bool isVcl(int codec, int type) { return codec == kH264 ? (type >= 1 && type <= 5) : (type >= 0 && type <= 31); }
bool isParameterSet(int codec, int type) { return codec == kH264 ? (type == 7 || type == 8) : (type >= 32 && type <= 34); }
int headerBytes(int codec) { return codec == kH264 ? 1 : 2; }

// Bits of an access unit as a byte stream: a four-byte start code for each unit.
int64_t streamBits(const Nals& nals) {
    int64_t b = 0;
    for (const auto& u : nals) b += int64_t(u.size() + 4) * 8;
    return b;
}

// ---- Parameter sets ----

bool h264HighProfile(int p) {
    return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 || p == 139 ||
           p == 134 || p == 135;
}

void skipScalingList(nal::Reader& r, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size && r.ok; ++j) {
        if (next != 0) next = int((last + r.se() + 256) % 256);
        last = next == 0 ? last : next;
    }
}

bool h264Hrd(nal::Reader& r, hrd::Params& p) {
    p.present = true;
    const uint64_t count = r.ue() + 1;
    if (count > 32) return false;
    const int rateScale = int(r.u(4)), sizeScale = int(r.u(4));
    p.cpbs.clear();
    for (uint64_t i = 0; i < count; ++i) {
        hrd::Cpb c;
        c.bitRate = double(r.ue() + 1) * std::ldexp(1.0, 6 + rateScale);
        c.size = double(r.ue() + 1) * std::ldexp(1.0, 4 + sizeScale);
        c.cbr = r.flag();
        p.cpbs.push_back(c);
    }
    p.initialDelayLength = int(r.u(5)) + 1;
    p.cpbDelayLength = int(r.u(5)) + 1;
    p.dpbDelayLength = int(r.u(5)) + 1;
    p.timeOffsetLength = int(r.u(5));
    return r.ok;
}

// HEVC profile_tier_level(1, maxSubLayersMinus1).
void hevcPtl(nal::Reader& r, int maxSub, hrd::Sps* s) {
    r.u(2);
    const bool tier = r.flag();
    const int profile = int(r.u(5));
    r.skip(32 + 4 + 43 + 1);
    const int level = int(r.u(8));
    if (s) s->profile = profile, s->highTier = tier, s->level = level;
    std::vector<int> prof(size_t(std::max(0, maxSub))), lev(size_t(std::max(0, maxSub)));
    for (int i = 0; i < maxSub; ++i) prof[size_t(i)] = r.bit(), lev[size_t(i)] = r.bit();
    if (maxSub > 0)
        for (int i = maxSub; i < 8; ++i) r.u(2);
    for (int i = 0; i < maxSub; ++i) {
        if (prof[size_t(i)]) r.skip(88);
        if (lev[size_t(i)]) r.skip(8);
    }
}

void hevcSubLayerHrd(nal::Reader& r, int count, bool subPic, hrd::Params& p, int rateScale, int sizeScale) {
    p.cpbs.clear();
    for (int i = 0; i < count; ++i) {
        hrd::Cpb c;
        c.bitRate = double(r.ue() + 1) * std::ldexp(1.0, 6 + rateScale);
        c.size = double(r.ue() + 1) * std::ldexp(1.0, 4 + sizeScale);
        if (subPic) r.ue(), r.ue();
        c.cbr = r.flag();
        p.cpbs.push_back(c);
    }
}

// HEVC hrd_parameters(commonInf, maxSub): the highest sub-layer's into `nal` and `vcl`.
bool hevcHrd(nal::Reader& r, bool commonInf, int maxSub, hrd::Params& nalP, hrd::Params& vclP) {
    bool nalPresent = false, vclPresent = false, subPic = false;
    int rateScale = 0, sizeScale = 0, initLen = 24, cpbLen = 24, dpbLen = 24;
    if (commonInf) {
        nalPresent = r.flag();
        vclPresent = r.flag();
        if (nalPresent || vclPresent) {
            subPic = r.flag();
            if (subPic) r.u(8), r.u(5), r.u(1), r.u(5);
            rateScale = int(r.u(4));
            sizeScale = int(r.u(4));
            if (subPic) r.u(4);
            initLen = int(r.u(5)) + 1;
            cpbLen = int(r.u(5)) + 1;
            dpbLen = int(r.u(5)) + 1;
        }
    }
    for (int i = 0; i <= maxSub && r.ok; ++i) {
        const bool general = r.flag();
        bool withinCvs = true;
        if (!general) withinCvs = r.flag();
        bool lowDelay = false;
        if (withinCvs) r.ue();
        else lowDelay = r.flag();
        int count = 1;
        if (!lowDelay) count = int(r.ue()) + 1;
        if (count > 32) return false;
        hrd::Params n, v;
        if (nalPresent) hevcSubLayerHrd(r, count, subPic, n, rateScale, sizeScale);
        if (vclPresent) hevcSubLayerHrd(r, count, subPic, v, rateScale, sizeScale);
        if (i == maxSub) {
            auto fill = [&](hrd::Params& dst, const hrd::Params& src, bool present) {
                dst = src;
                dst.present = present;
                dst.initialDelayLength = initLen, dst.cpbDelayLength = cpbLen, dst.dpbDelayLength = dpbLen;
                dst.lowDelay = lowDelay;
                dst.subPic = subPic;
            };
            fill(nalP, n, nalPresent);
            fill(vclP, v, vclPresent);
        }
    }
    return r.ok;
}

// HEVC st_ref_pic_set(idx): returns NumDeltaPocs.
int hevcStRps(nal::Reader& r, int idx, int count, const std::vector<int>& deltas) {
    bool inter = false;
    if (idx != 0) inter = r.flag();
    if (inter) {
        int delta = 1;
        if (idx == count) delta = int(r.ue()) + 1;
        r.flag();
        r.ue();
        const int ref = idx - delta;
        if (ref < 0 || ref >= int(deltas.size())) {
            r.ok = false;
            return 0;
        }
        int n = 0;
        for (int j = 0; j <= deltas[size_t(ref)] && r.ok; ++j) {
            const bool used = r.flag();
            bool useDelta = true;
            if (!used) useDelta = r.flag();
            if (used || useDelta) ++n;
        }
        return n;
    }
    const int neg = int(r.ue()), pos = int(r.ue());
    if (neg > 32 || pos > 32) {
        r.ok = false;
        return 0;
    }
    for (int i = 0; i < neg + pos; ++i) r.ue(), r.flag();
    return neg + pos;
}

// ---- SEI ----

struct SeiMessage {
    int type = 0;
    std::vector<uint8_t> payload;
};

bool parseSei(const std::vector<uint8_t>& rbsp, int header, std::vector<SeiMessage>& out) {
    size_t i = size_t(header);
    while (i < rbsp.size()) {
        if (rbsp[i] == 0x80 && i + 1 == rbsp.size()) return true;
        int type = 0, size = 0;
        while (i < rbsp.size() && rbsp[i] == 0xff) type += 255, ++i;
        if (i >= rbsp.size()) return false;
        type += rbsp[i++];
        while (i < rbsp.size() && rbsp[i] == 0xff) size += 255, ++i;
        if (i >= rbsp.size()) return false;
        size += rbsp[i++];
        if (i + size_t(size) > rbsp.size()) return false;
        out.push_back({type, std::vector<uint8_t>(rbsp.begin() + std::ptrdiff_t(i), rbsp.begin() + std::ptrdiff_t(i + size_t(size)))});
        i += size_t(size);
    }
    return true;
}

std::vector<uint8_t> buildSei(const uint8_t* header, int headerSize, const std::vector<SeiMessage>& messages) {
    std::vector<uint8_t> rbsp;
    for (const SeiMessage& m : messages) {
        int t = m.type, s = int(m.payload.size());
        while (t >= 255) rbsp.push_back(0xff), t -= 255;
        rbsp.push_back(uint8_t(t));
        while (s >= 255) rbsp.push_back(0xff), s -= 255;
        rbsp.push_back(uint8_t(s));
        rbsp.insert(rbsp.end(), m.payload.begin(), m.payload.end());
    }
    rbsp.push_back(0x80);
    return nal::escape(header, size_t(headerSize), rbsp);
}

bool readBp(int codec, const std::vector<uint8_t>& payload, const std::map<int, hrd::Sps>& sps, hrd::BufferingPeriod& bp) {
    nal::Reader r(payload);
    bp = {};
    bp.spsId = int(r.ue());
    auto it = sps.find(bp.spsId);
    if (it == sps.end() || !r.ok) return false;
    const hrd::Sps& s = it->second;
    if (codec == kHevc) {
        const hrd::Params& any = s.nal.present ? s.nal : s.vcl;
        if (!any.subPic) bp.irapCpbParams = r.flag();
        if (bp.irapCpbParams) {
            bp.cpbDelayOffset = int64_t(r.u(any.cpbDelayLength));
            bp.dpbDelayOffset = int64_t(r.u(any.dpbDelayLength));
        }
        bp.concatenation = r.flag();
        bp.auCpbRemovalDelayDeltaMinus1 = int64_t(r.u(any.cpbDelayLength));
    }
    for (int set = 0; set < 2; ++set) {
        const hrd::Params& p = set == 0 ? s.nal : s.vcl;
        if (!p.present) continue;
        auto& delay = set == 0 ? bp.nalDelay : bp.vclDelay;
        auto& offset = set == 0 ? bp.nalOffset : bp.vclOffset;
        auto& altDelay = set == 0 ? bp.nalAltDelay : bp.vclAltDelay;
        auto& altOffset = set == 0 ? bp.nalAltOffset : bp.vclAltOffset;
        for (size_t i = 0; i < p.cpbs.size(); ++i) {
            delay.push_back(int64_t(r.u(p.initialDelayLength)));
            offset.push_back(int64_t(r.u(p.initialDelayLength)));
            if (codec == kHevc && (p.subPic || bp.irapCpbParams)) {
                altDelay.push_back(int64_t(r.u(p.initialDelayLength)));
                altOffset.push_back(int64_t(r.u(p.initialDelayLength)));
            }
        }
    }
    bp.present = r.ok;
    return r.ok;
}

std::vector<uint8_t> writeBp(int codec, const hrd::BufferingPeriod& bp, const hrd::Sps& s) {
    nal::Writer w;
    w.ue(uint64_t(std::max(0, bp.spsId)));
    if (codec == kHevc) {
        const hrd::Params& any = s.nal.present ? s.nal : s.vcl;
        if (!any.subPic) w.flag(bp.irapCpbParams);
        if (bp.irapCpbParams) {
            w.u(uint64_t(bp.cpbDelayOffset), any.cpbDelayLength);
            w.u(uint64_t(bp.dpbDelayOffset), any.dpbDelayLength);
        }
        w.flag(bp.concatenation);
        w.u(uint64_t(bp.auCpbRemovalDelayDeltaMinus1), any.cpbDelayLength);
    }
    for (int set = 0; set < 2; ++set) {
        const hrd::Params& p = set == 0 ? s.nal : s.vcl;
        if (!p.present) continue;
        const auto& delay = set == 0 ? bp.nalDelay : bp.vclDelay;
        const auto& offset = set == 0 ? bp.nalOffset : bp.vclOffset;
        const auto& altDelay = set == 0 ? bp.nalAltDelay : bp.vclAltDelay;
        const auto& altOffset = set == 0 ? bp.nalAltOffset : bp.vclAltOffset;
        const uint64_t top = (uint64_t(1) << p.initialDelayLength) - 1;
        for (size_t i = 0; i < p.cpbs.size(); ++i) {
            auto at = [&](const std::vector<int64_t>& v) { return uint64_t(std::clamp<int64_t>(i < v.size() ? v[i] : v.empty() ? 0 : v.back(), 0, int64_t(top))); };
            w.u(at(delay), p.initialDelayLength);
            w.u(at(offset), p.initialDelayLength);
            if (codec == kHevc && (p.subPic || bp.irapCpbParams)) {
                w.u(at(altDelay.empty() ? delay : altDelay), p.initialDelayLength);
                w.u(at(altOffset.empty() ? offset : altOffset), p.initialDelayLength);
            }
        }
    }
    w.payloadAlign();
    return w.out;
}

// The picture timing message: its delays, and (H.264) where the rest after them begins and ends, in bits.
bool readPt(int codec, const std::vector<uint8_t>& payload, const hrd::Sps& s, hrd::PictureTiming& pt, size_t* restFrom = nullptr,
            size_t* restTo = nullptr) {
    nal::Reader r(payload);
    pt = {};
    const hrd::Params* p = s.active();
    if (codec == kHevc) {
        if (s.picStruct) pt.picStruct = int(r.u(4)), r.u(2), r.u(1);
        if (p) {
            if (p->subPic) return false;
            pt.cpbRemovalDelay = int64_t(r.u(p->cpbDelayLength)) + 1;
            pt.dpbOutputDelay = int64_t(r.u(p->dpbDelayLength));
        }
        pt.present = r.ok;
        return r.ok;
    }
    if (p) {
        pt.cpbRemovalDelay = int64_t(r.u(p->cpbDelayLength));
        pt.dpbOutputDelay = int64_t(r.u(p->dpbDelayLength));
    }
    if (restFrom) *restFrom = r.pos;
    if (s.picStruct) {
        pt.picStruct = int(r.u(4));
        static const int clocks[16] = {1, 1, 1, 2, 2, 3, 3, 2, 3, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < clocks[pt.picStruct & 15]; ++i) {
            if (!r.flag()) continue;  // clock_timestamp_flag
            r.u(2), r.u(1), r.u(5);
            const bool full = r.flag();
            r.u(1), r.u(1), r.u(8);
            if (full) {
                r.u(6), r.u(6), r.u(5);
            } else if (r.flag()) {
                r.u(6);
                if (r.flag()) {
                    r.u(6);
                    if (r.flag()) r.u(5);
                }
            }
            const int tol = s.nal.present ? s.nal.timeOffsetLength : s.vcl.present ? s.vcl.timeOffsetLength : 24;
            if (tol > 0) r.u(tol);
        }
    }
    if (restTo) *restTo = r.pos;
    pt.present = r.ok;
    return r.ok;
}

std::vector<uint8_t> writePt(int codec, const hrd::PictureTiming& pt, const hrd::Sps& s, const std::vector<uint8_t>* original, size_t restFrom,
                             size_t restTo) {
    nal::Writer w;
    const hrd::Params* p = s.active();
    if (codec == kHevc) {
        if (s.picStruct) {
            if (original) w.copy(*original, 0, 7);
            else w.u(0, 4), w.u(1, 2), w.u(0, 1);  // a frame, progressive, not a duplicate
        }
        if (p) {
            w.u(uint64_t(std::max<int64_t>(0, pt.cpbRemovalDelay - 1)) & ((uint64_t(1) << p->cpbDelayLength) - 1), p->cpbDelayLength);
            w.u(uint64_t(std::max<int64_t>(0, pt.dpbOutputDelay)) & ((uint64_t(1) << p->dpbDelayLength) - 1), p->dpbDelayLength);
        }
    } else {
        if (p) {
            w.u(uint64_t(std::max<int64_t>(0, pt.cpbRemovalDelay)) & ((uint64_t(1) << p->cpbDelayLength) - 1), p->cpbDelayLength);
            w.u(uint64_t(std::max<int64_t>(0, pt.dpbOutputDelay)) & ((uint64_t(1) << p->dpbDelayLength) - 1), p->dpbDelayLength);
        }
        if (s.picStruct) {
            if (original) w.copy(*original, restFrom, restTo);
            else w.u(0, 4), w.u(0, 1);  // a frame, no clock timestamp
        }
    }
    w.payloadAlign();
    return w.out;
}

// ---- Level limits (no HRD stated) ----

// H.264 Table A-1: MaxBR and MaxCPB (thousands) by level_idc (9 = 1b).
bool h264LevelLimits(int level, bool constraint3, double& br, double& cpb) {
    struct L { int idc; double br, cpb; };
    static const L t[] = {{10, 64, 175},       {9, 128, 350},        {11, 192, 500},       {12, 384, 1000},     {13, 768, 2000},
                          {20, 2000, 2000},    {21, 4000, 4000},     {22, 4000, 4000},     {30, 10000, 10000},  {31, 14000, 14000},
                          {32, 20000, 20000},  {40, 20000, 25000},   {41, 50000, 62500},   {42, 50000, 62500},  {50, 135000, 135000},
                          {51, 240000, 240000}, {52, 240000, 240000}, {60, 240000, 240000}, {61, 480000, 480000}, {62, 800000, 800000}};
    if (level == 11 && constraint3) level = 9;
    for (const L& l : t)
        if (l.idc == level) {
            br = l.br, cpb = l.cpb;
            return true;
        }
    return false;
}

double h264NalFactor(int profile) {
    switch (profile) {
    case 100: return 1500;
    case 110: return 3600;
    case 122: case 244: case 44: return 4800;
    default: return 1200;
    }
}

// H.265 Tables A.8 and A.9: MaxCPB and MaxBR (thousands) for the tier, by general_level_idc (30 x level).
bool hevcLevelLimits(int level, bool high, double& br, double& cpb) {
    struct L { int idc; double cpbMain, cpbHigh, brMain, brHigh; };
    static const L t[] = {{30, 350, 0, 128, 0},           {60, 1500, 0, 1500, 0},           {63, 3000, 0, 3000, 0},
                          {90, 6000, 0, 6000, 0},         {93, 10000, 0, 10000, 0},         {120, 12000, 30000, 12000, 30000},
                          {123, 20000, 50000, 20000, 50000}, {150, 25000, 100000, 25000, 100000}, {153, 40000, 160000, 40000, 160000},
                          {156, 60000, 240000, 60000, 240000}, {180, 60000, 240000, 60000, 240000}, {183, 120000, 480000, 120000, 480000},
                          {186, 240000, 800000, 240000, 800000}};
    for (const L& l : t)
        if (l.idc == level) {
            const bool h = high && l.cpbHigh > 0;
            br = h ? l.brHigh : l.brMain, cpb = h ? l.cpbHigh : l.cpbMain;
            return true;
        }
    return false;
}

// ---- MPEG-2 ----

struct Mpeg2Picture {
    size_t start = 0;  // byte where its data begins (its sequence and GOP headers, if any)
    size_t psc = 0;    // byte after its picture start code
    int64_t vbvDelay = -1;
    int structure = 3;  // 1 top field, 2 bottom, 3 frame
    bool repeatFirstField = false, topFieldFirst = false;
};

struct Mpeg2Stream {
    double bitRate = 0, buffer = 0;
    bool progressiveSequence = false, lowDelay = false;
};

// The pictures in `d` (offsets relative to it), and what sequence headers say.
void mpeg2Pictures(const uint8_t* d, size_t n, Mpeg2Stream& st, std::vector<Mpeg2Picture>& out, size_t base) {
    size_t dataStart = 0;
    bool haveStart = false;
    uint32_t rate = 0, vbv = 0;
    bool seqSeen = false;
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        const uint8_t code = d[i + 3];
        if (code == 0xb3 || code == 0xb8) {
            if (!haveStart) dataStart = i, haveStart = true;
            if (code == 0xb3 && i + 12 <= n) {
                nal::Reader r(d + i + 4, n - i - 4);
                r.u(12), r.u(12), r.u(4), r.u(4);
                rate = uint32_t(r.u(18));
                r.u(1);
                vbv = uint32_t(r.u(10));
                seqSeen = true;
            }
        } else if (code == 0x00 && i + 8 <= n) {
            Mpeg2Picture p;
            p.start = base + (haveStart ? dataStart : i);
            p.psc = base + i + 4;
            const uint32_t v = uint32_t(d[i + 4]) << 24 | uint32_t(d[i + 5]) << 16 | uint32_t(d[i + 6]) << 8 | d[i + 7];
            const int64_t delay = (v >> 3) & 0xffff;
            p.vbvDelay = delay == 0xffff ? -1 : delay;
            out.push_back(p);
            haveStart = false;
        } else if (code == 0xb5 && i + 9 <= n) {
            const int id = d[i + 4] >> 4;
            if (id == 1) {  // sequence extension
                nal::Reader r(d + i + 4, n - i - 4);
                r.u(4), r.u(8);
                st.progressiveSequence = r.flag();
                r.u(2), r.u(2), r.u(2);
                const uint32_t rateExt = uint32_t(r.u(12));
                r.u(1);
                const uint32_t vbvExt = uint32_t(r.u(8));
                st.lowDelay = r.flag();
                if (seqSeen) {
                    st.bitRate = 400.0 * double((rateExt << 18) | rate);
                    st.buffer = 16384.0 * double((vbvExt << 10) | vbv);
                }
            } else if (id == 8 && !out.empty()) {  // picture coding extension
                nal::Reader r(d + i + 4, n - i - 4);
                r.u(4), r.u(16);
                r.u(2);
                out.back().structure = int(r.u(2));
                out.back().topFieldFirst = r.flag();
                r.u(5);
                out.back().repeatFirstField = r.flag();
            }
        } else if (code > 0x00 && code <= 0xaf) {
            i += 3;  // (a slice: skipped to its data)
        }
    }
    if (seqSeen && st.bitRate <= 0) {  // (MPEG-1 style, no extension)
        st.bitRate = 400.0 * rate;
        st.buffer = 16384.0 * vbv;
    }
}

}  // namespace

// ---- Parsing (public) ----

namespace hrd {

Sps parseH264Sps(const uint8_t* d, size_t n) {
    Sps s;
    s.codec = kH264;
    if (n < 4) return s;
    const std::vector<uint8_t> rbsp = nal::unescape(d, n);
    nal::Reader r(rbsp, 8);
    s.profile = int(r.u(8));
    const int constraints = int(r.u(8));
    s.level = int(r.u(8));
    if (s.level == 11 && (constraints & 0x10)) s.level = 9;  // (level 1b)
    s.id = int(r.ue());
    if (h264HighProfile(s.profile)) {
        s.chromaFormat = int(r.ue());
        if (s.chromaFormat == 3) s.separateColourPlane = r.flag();
        r.ue(), r.ue(), r.flag();
        if (r.flag())
            for (int i = 0; i < (s.chromaFormat != 3 ? 8 : 12); ++i)
                if (r.flag()) skipScalingList(r, i < 6 ? 16 : 64);
    }
    s.log2MaxFrameNum = int(r.ue()) + 4;
    s.pocType = int(r.ue());
    if (s.pocType == 0) {
        s.log2MaxPocLsb = int(r.ue()) + 4;
    } else if (s.pocType == 1) {
        s.deltaPicOrderAlwaysZero = r.flag();
        r.se(), r.se();
        const uint64_t cycle = r.ue();
        if (cycle > 255) return s;
        for (uint64_t i = 0; i < cycle; ++i) r.se();
    }
    s.maxNumRefFrames = int(r.ue());
    s.gapsAllowed = r.flag();
    r.ue(), r.ue();
    s.frameMbsOnly = r.flag();
    if (!s.frameMbsOnly) r.flag();
    r.flag();
    if (r.flag()) r.ue(), r.ue(), r.ue(), r.ue();
    s.vui = r.flag();
    if (s.vui) {
        if (r.flag() && r.u(8) == 255) r.u(16), r.u(16);
        if (r.flag()) r.flag();
        if (r.flag()) {
            r.u(3), r.flag();
            if (r.flag()) r.u(8), r.u(8), r.u(8);
        }
        if (r.flag()) r.ue(), r.ue();
        s.timing = r.flag();
        if (s.timing) {
            s.numUnitsInTick = uint32_t(r.u(32));
            s.timeScale = uint32_t(r.u(32));
            s.fixedFrameRate = r.flag();
        }
        const size_t from = r.pos;
        const bool nalHrd = r.flag();
        if (nalHrd && !h264Hrd(r, s.nal)) return s;
        const bool vclHrd = r.flag();
        if (vclHrd && !h264Hrd(r, s.vcl)) return s;
        if (nalHrd || vclHrd) s.nal.lowDelay = s.vcl.lowDelay = r.flag();
        if (s.timing) s.hrdFrom = from, s.hrdTo = r.pos;
        s.picStruct = r.flag();
    }
    s.valid = r.ok;
    return s;
}

Pps parseH264Pps(const uint8_t* d, size_t n, const std::map<int, Sps>& sps) {
    Pps p;
    if (n < 2) return p;
    const std::vector<uint8_t> rbsp = nal::unescape(d, n);
    nal::Reader r(rbsp, 8);
    p.id = int(r.ue());
    p.spsId = int(r.ue());
    p.cabac = r.flag();
    p.bottomFieldPicOrder = r.flag();
    p.numSliceGroups = int(r.ue()) + 1;
    if (p.numSliceGroups > 1) {
        p.valid = false;  // (slice groups: Baseline's FMO, not read further)
        return p;
    }
    p.numRefIdxL0 = int(r.ue()) + 1;
    p.numRefIdxL1 = int(r.ue()) + 1;
    p.weightedPred = r.flag();
    p.weightedBipred = int(r.u(2));
    r.se(), r.se(), r.se();
    p.deblockingControl = r.flag();
    r.flag();
    p.redundantPicCnt = r.flag();
    (void)sps;
    p.valid = r.ok;
    return p;
}

Sps parseHevcSps(const uint8_t* d, size_t n) {
    Sps s;
    s.codec = kHevc;
    if (n < 4) return s;
    const std::vector<uint8_t> rbsp = nal::unescape(d, n);
    nal::Reader r(rbsp, 16);
    s.vpsId = int(r.u(4));
    s.maxSubLayersMinus1 = int(r.u(3));
    r.flag();
    hevcPtl(r, s.maxSubLayersMinus1, &s);
    s.id = int(r.ue());
    s.chromaFormat = int(r.ue());
    if (s.chromaFormat == 3) s.separateColourPlane = r.flag();
    r.ue(), r.ue();
    if (r.flag()) r.ue(), r.ue(), r.ue(), r.ue();
    r.ue(), r.ue();
    const int log2MaxPocLsb = int(r.ue()) + 4;
    s.log2MaxPocLsb = log2MaxPocLsb;
    const bool ordering = r.flag();
    for (int i = ordering ? 0 : s.maxSubLayersMinus1; i <= s.maxSubLayersMinus1; ++i) r.ue(), r.ue(), r.ue();
    r.ue(), r.ue(), r.ue(), r.ue(), r.ue(), r.ue();
    if (r.flag() && r.flag()) {
        for (int size = 0; size < 4; ++size)
            for (int matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
                if (!r.flag()) {
                    r.ue();
                } else {
                    const int coefs = std::min(64, 1 << (4 + (size << 1)));
                    if (size > 1) r.se();
                    for (int i = 0; i < coefs; ++i) r.se();
                }
            }
    }
    r.flag(), r.flag();
    if (r.flag()) r.u(4), r.u(4), r.ue(), r.ue(), r.flag();
    const int sets = int(r.ue());
    if (sets > 64) return s;
    std::vector<int> deltas;
    for (int i = 0; i < sets && r.ok; ++i) deltas.push_back(hevcStRps(r, i, sets, deltas));
    if (r.flag()) {
        const uint64_t lt = r.ue();
        if (lt > 32) return s;
        for (uint64_t i = 0; i < lt; ++i) r.u(log2MaxPocLsb), r.flag();
    }
    r.flag(), r.flag();
    s.vui = r.flag();
    if (s.vui) {
        if (r.flag() && r.u(8) == 255) r.u(16), r.u(16);
        if (r.flag()) r.flag();
        if (r.flag()) {
            r.u(3), r.flag();
            if (r.flag()) r.u(8), r.u(8), r.u(8);
        }
        if (r.flag()) r.ue(), r.ue();
        r.flag(), r.flag();
        s.picStruct = r.flag();
        if (r.flag()) r.ue(), r.ue(), r.ue(), r.ue();
        s.timing = r.flag();
        if (s.timing) {
            s.numUnitsInTick = uint32_t(r.u(32));
            s.timeScale = uint32_t(r.u(32));
            if (r.flag()) r.ue();
            const size_t from = r.pos;
            if (r.flag() && !hevcHrd(r, true, s.maxSubLayersMinus1, s.nal, s.vcl)) return s;
            s.hrdFrom = from, s.hrdTo = r.pos;
        }
    }
    s.valid = r.ok;
    return s;
}

Pps parseHevcPps(const uint8_t* d, size_t n) {
    Pps p;
    if (n < 3) return p;
    const std::vector<uint8_t> rbsp = nal::unescape(d, n);
    nal::Reader r(rbsp, 16);
    p.id = int(r.ue());
    p.spsId = int(r.ue());
    p.dependentSliceSegments = r.flag();
    p.outputFlagPresent = r.flag();
    p.numExtraSliceHeaderBits = int(r.u(3));
    p.valid = r.ok;
    return p;
}

bool parseHevcVpsHrd(const uint8_t* d, size_t n, Sps& into) {
    const std::vector<uint8_t> rbsp = nal::unescape(d, n);
    nal::Reader r(rbsp, 16);
    r.u(4), r.u(1), r.u(1), r.u(6);
    const int maxSub = int(r.u(3));
    r.flag(), r.u(16);
    hevcPtl(r, maxSub, nullptr);
    const bool ordering = r.flag();
    for (int i = ordering ? 0 : maxSub; i <= maxSub; ++i) r.ue(), r.ue(), r.ue();
    const int maxLayerId = int(r.u(6));
    const uint64_t layerSets = r.ue() + 1;
    if (layerSets > 1024) return false;
    for (uint64_t i = 1; i < layerSets; ++i) r.skip(size_t(maxLayerId) + 1);
    if (!r.flag()) return false;
    into.numUnitsInTick = uint32_t(r.u(32));
    into.timeScale = uint32_t(r.u(32));
    into.timing = true;
    if (r.flag()) r.ue();
    const uint64_t hrds = r.ue();
    if (hrds == 0 || !r.ok) return false;
    r.ue();  // hrd_layer_set_idx[0]
    return hevcHrd(r, true, maxSub, into.nal, into.vcl) && (into.nal.present || into.vcl.present);
}

std::vector<uint8_t> spliceHrd(int codec, const uint8_t* target, size_t targetSize, const uint8_t* from, size_t fromSize) {
    const Sps t = codec == kH264 ? parseH264Sps(target, targetSize) : parseHevcSps(target, targetSize);
    const Sps f = codec == kH264 ? parseH264Sps(from, fromSize) : parseHevcSps(from, fromSize);
    if (!t.valid || !f.valid || !t.hrdTo || !f.hrdTo || (codec == kHevc && t.maxSubLayersMinus1 != f.maxSubLayersMinus1)) return {};
    const std::vector<uint8_t> tr = nal::unescape(target, targetSize), fr = nal::unescape(from, fromSize);
    const size_t stop = nal::stopBit(tr);
    if (stop < t.hrdTo) return {};
    const int h = headerBytes(codec);
    nal::Writer w;
    w.copy(tr, size_t(h) * 8, t.hrdFrom);
    w.copy(fr, f.hrdFrom, f.hrdTo);
    w.copy(tr, t.hrdTo, stop);
    w.trailing();
    return nal::escape(target, size_t(h), w.out);
}

bool readAccessUnit(int codec, const uint8_t* d, size_t n, int lengthSize, std::map<int, Sps>& sps, std::map<int, Pps>& pps,
                    AccessUnit& au, std::map<int, Sps>* vpsHrd) {
    au = {};
    const Nals nals = splitNals(d, n, lengthSize);
    if (nals.empty()) return false;
    au.bits = lengthSize > 0 ? streamBits(nals) : int64_t(n) * 8;
    std::vector<const std::vector<uint8_t>*> seis;
    bool firstSlice = true;
    for (const auto& u : nals) {
        const int type = nalType(codec, u);
        if (codec == kH264) {
            if (type == 7) {
                Sps s = parseH264Sps(u.data(), u.size());
                if (s.valid) sps[s.id] = s;
            } else if (type == 8) {
                Pps p = parseH264Pps(u.data(), u.size(), sps);
                if (p.id >= 0) pps[p.id] = p;
            } else if (type == 6) {
                seis.push_back(&u);
            } else if (type == 12) {
                au.vclBits += int(u.size() * 8);  // (filler data: counted in a Type I stream, as its VCL units are)
            } else if (type >= 1 && type <= 5) {
                au.vclBits += int(u.size() * 8);
                if (firstSlice) {
                    firstSlice = false;
                    au.idr = au.irap = type == 5;
                    const std::vector<uint8_t> rb = nal::unescape(u.data(), std::min<size_t>(u.size(), 32));
                    nal::Reader r(rb, 8);
                    r.ue(), r.ue();
                    const int id = int(r.ue());
                    if (auto it = pps.find(id); it != pps.end()) au.spsId = it->second.spsId;
                }
            }
        } else {
            if (type == 32 && vpsHrd) {
                Sps v;
                if (parseHevcVpsHrd(u.data(), u.size(), v)) (*vpsHrd)[(u.size() > 2 ? u[2] >> 4 : 0)] = v;
            } else if (type == 33) {
                Sps s = parseHevcSps(u.data(), u.size());
                if (s.valid) sps[s.id] = s;
            } else if (type == 34) {
                Pps p = parseHevcPps(u.data(), u.size());
                if (p.valid) pps[p.id] = p;
            } else if (type == 39) {
                seis.push_back(&u);
            } else if (type == 38) {
                au.vclBits += int(u.size() * 8);  // (filler data: counted in a Type I stream, as its VCL units are)
            } else if (type <= 31) {
                au.vclBits += int(u.size() * 8);
                if (firstSlice && u.size() > 2) {
                    firstSlice = false;
                    au.irap = type >= 16 && type <= 23;
                    au.idr = type == 19 || type == 20;
                    const std::vector<uint8_t> rb = nal::unescape(u.data(), std::min<size_t>(u.size(), 32));
                    nal::Reader r(rb, 16);
                    r.flag();
                    if (au.irap) r.flag();
                    const int id = int(r.ue());
                    if (auto it = pps.find(id); it != pps.end()) au.spsId = it->second.spsId;
                }
            }
        }
    }
    const Sps* active = nullptr;
    if (auto it = sps.find(au.spsId); it != sps.end()) active = &it->second;
    for (const auto* u : seis) {
        std::vector<SeiMessage> messages;
        if (!parseSei(nal::unescape(u->data(), u->size()), headerBytes(codec), messages)) continue;
        for (const SeiMessage& m : messages) {
            if (m.type == 0 && !au.bp.present) readBp(codec, m.payload, sps, au.bp);
            else if (m.type == 1 && active && !au.pt.present) readPt(codec, m.payload, *active, au.pt);
        }
    }
    return true;
}

std::vector<std::vector<uint8_t>> parameterSets(int codec, const uint8_t* x, size_t xn, int& lengthSize) {
    std::vector<std::vector<uint8_t>> out;
    lengthSize = 0;
    if (!x) return out;
    auto take = [&](size_t& i) {
        const size_t len = size_t(x[i]) << 8 | x[i + 1];
        if (i + 2 + len > xn) return false;
        out.emplace_back(x + i + 2, x + i + 2 + len);
        i += 2 + len;
        return true;
    };
    if (codec == kH264 && xn >= 7 && x[0] == 1) {
        lengthSize = (x[4] & 3) + 1;
        size_t i = 6;
        for (int k = 0, c = x[5] & 31; k < c && i + 2 <= xn; ++k)
            if (!take(i)) return out;
        if (i < xn)
            for (int k = 0, c = x[i++]; k < c && i + 2 <= xn; ++k)
                if (!take(i)) return out;
    } else if (codec == kHevc && xn >= 23 && x[0] == 1) {
        lengthSize = (x[21] & 3) + 1;
        size_t i = 23;
        for (int a = 0, arrays = x[22]; a < arrays && i + 3 <= xn; ++a) {
            const int count = x[i + 1] << 8 | x[i + 2];
            i += 3;
            for (int k = 0; k < count && i + 2 <= xn; ++k)
                if (!take(i)) return out;
        }
    } else if (xn > 4) {
        out = splitNals(x, xn, 0);
    }
    return out;
}

bool mpeg2Rate(const uint8_t* d, size_t n, double& bitRate, double& bufferBits) {
    Mpeg2Stream st;
    std::vector<Mpeg2Picture> pics;
    mpeg2Pictures(d, n, st, pics, 0);
    bitRate = st.bitRate, bufferBits = st.buffer;
    return bitRate > 0 && bufferBits > 0;
}

}  // namespace hrd

// ---- The report ----

std::string HrdReport::summary() const {
    if (!error.empty()) return "Not checked: " + error;
    if (!checked) return "Not checked: " + (note.empty() ? std::string("no buffer model") : note);
    std::string s = codec + " " + model + ", " + (cbr ? "CBR " : "VBR ") + format("%.3g Mb/s, %.2f s buffer", bitRate / 1e6, bitRate > 0 ? bufferBits / bitRate : 0);
    if (conforms()) return s + format(": conforms (%g frames)", double(frames));
    s += format(": %g problem", double(issueCount)) + (issueCount == 1 ? "" : "s");
    if (!issues.empty()) s += format(", the first at frame %g", double(issues.front().frame)) + " (" + issues.front().kind + ")";
    return s;
}

namespace {

struct Unit {
    hrd::AccessUnit au;
    int64_t pts = AV_NOPTS_VALUE, dts = AV_NOPTS_VALUE;
    int64_t shown = 0;
    int temporalId = 0, type = -1;
};

void addIssue(HrdReport& r, int64_t frame, int64_t shown, const char* kind, const std::string& text) {
    ++r.issueCount;
    if (r.issues.size() < kKeptIssues) r.issues.push_back({frame, shown, kind, text});
}

// Annex C over access units whose removal times are known: arrivals, then fullness just before each removal.
// A buffering period whose initial delay is not where the buffer stands (C.3 of H.264, C.4 of H.265) is checked the way
// it matters: a decoder that starts there (a channel change, a cut into the stream) waits what it says, which shifts
// every later arrival against the removals by the difference; it is a problem when that shift runs the buffer dry or
// over somewhere after it. Encoders' own periods are often a few milliseconds off (an encoding library's
// identification SEI written into the first access unit after its encoder counted it, a rate control's rounding).
void runCpb(HrdReport& r, const std::vector<double>& removal, const std::vector<int64_t>& bits, const std::vector<int64_t>& shown,
            const std::vector<bool>& bpStart, const std::vector<double>& initDelay, const std::vector<double>& initSum, double rate, double size,
            bool cbr, bool lowDelay) {
    const size_t n = removal.size();
    std::vector<double> ai(n), af(n), gap(n, 0);
    for (size_t k = 0; k < n; ++k) {
        if (k == 0) {
            ai[k] = 0;
        } else if (cbr) {
            ai[k] = af[k - 1];
        } else {
            const double earliest = removal[k] - (bpStart[k] ? initDelay[k] : initSum[k]);
            ai[k] = std::max(af[k - 1], earliest);
        }
        af[k] = ai[k] + double(bits[k]) / rate;
        if (k > 0) gap[k] = 90000.0 * (removal[k] - af[k - 1]);
        if (af[k] > removal[k] + 1e-7 && !lowDelay)
            addIssue(r, int64_t(k), shown[k], "underflow",
                     format("the picture's last bits arrive %.2f ms after it is to be decoded (%.0f bits)", (af[k] - removal[k]) * 1000, double(bits[k])));
    }
    // Fullness just before each removal: what has arrived of this and later access units.
    std::vector<double> fullness(n);
    for (size_t k = 0; k < n; ++k) {
        double full = 0;
        for (size_t m = k; m < n && ai[m] < removal[k]; ++m) full += std::min(double(bits[m]), (removal[k] - ai[m]) * rate);
        fullness[k] = full;
        if (full > size * (1 + 1e-9) + 8)
            addIssue(r, int64_t(k), shown[k], "overflow", format("the buffer would hold %.0f bits of its %.0f", full, size));
        r.highest = std::max(r.highest, full / size);
        if (k + 1 < n) r.lowest = std::min(r.lowest, std::max(0.0, full - double(bits[k])) / size);  // (the last leaves it empty)
    }
    // From each access unit on: the least time any picture's bits are early, and the least room left.
    std::vector<double> early(n + 1, 1e30), room(n + 1, 1e30);
    for (size_t k = n; k-- > 0;) {
        early[k] = std::min(early[k + 1], removal[k] - af[k]);
        room[k] = std::min(room[k + 1], size - fullness[k]);
    }
    double worst = 0;
    for (size_t k = 1; k < n; ++k) {
        if (!bpStart[k]) continue;
        const double said = initDelay[k] * 90000.0, g = gap[k];
        const bool exact = cbr ? (said >= std::floor(g + 1e-6) - 0.5 && said <= std::ceil(g - 1e-6) + 0.5) : said <= std::ceil(g - 1e-6) + 0.5;
        if (exact) continue;
        worst = std::max(worst, std::fabs(said - g) / 90.0);
        const double shift = (said - g) / 90000.0;  // > 0: a decoder starting here has the bits earlier
        if (shift < 0 && -shift > early[k] + 1e-7)
            addIssue(r, int64_t(k), shown[k], "initial delay",
                     format("its buffering period says %.0f (90 kHz) where the buffer gives %.1f: a decoder starting here runs dry", said, g));
        else if (shift > 0 && shift * rate > room[k] + 8)
            addIssue(r, int64_t(k), shown[k], "initial delay",
                     format("its buffering period says %.0f (90 kHz) where the buffer gives %.1f: a decoder starting here overflows", said, g));
    }
    if (worst > 0) {
        if (!r.note.empty()) r.note += "; ";
        r.note += format("buffering periods differ from where the buffer stands by up to %.1f ms", worst);
    }
}

void checkNal(int codec, AVFormatContext* fmt, int v, HrdReport& r, const std::atomic<bool>* cancel) {
    const AVCodecParameters* par = fmt->streams[v]->codecpar;
    std::map<int, hrd::Sps> sps, vpsHrd;
    std::map<int, hrd::Pps> pps;
    int lengthSize = 0;
    for (const auto& u : hrd::parameterSets(codec, par->extradata, size_t(std::max(0, par->extradata_size)), lengthSize)) {
        hrd::AccessUnit dummy;
        std::vector<uint8_t> one{0, 0, 0, 1};
        one.insert(one.end(), u.begin(), u.end());
        hrd::readAccessUnit(codec, one.data(), one.size(), 0, sps, pps, dummy, &vpsHrd);
    }
    std::vector<Unit> units;
    AVPacket* pkt = av_packet_alloc();
    while (pkt && av_read_frame(fmt, pkt) >= 0) {
        if (cancel && cancel->load()) break;
        if (pkt->stream_index == v) {
            Unit u;
            hrd::readAccessUnit(codec, pkt->data, size_t(pkt->size), lengthSize, sps, pps, u.au, &vpsHrd);
            u.pts = pkt->pts, u.dts = pkt->dts;
            const Nals nals = splitNals(pkt->data, size_t(pkt->size), lengthSize);
            for (const auto& nu : nals)
                if (isVcl(codec, nalType(codec, nu))) {
                    u.type = nalType(codec, nu);
                    if (codec == kHevc && nu.size() > 1) u.temporalId = (nu[1] & 7) - 1;
                    break;
                }
            units.push_back(std::move(u));
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (cancel && cancel->load()) {
        r.note = "cancelled";
        return;
    }
    if (units.empty()) {
        r.note = "no pictures";
        return;
    }
    // Display order.
    {
        std::vector<std::pair<int64_t, size_t>> order;
        for (size_t k = 0; k < units.size(); ++k) order.push_back({units[k].pts == AV_NOPTS_VALUE ? int64_t(k) : units[k].pts, k});
        std::stable_sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size(); ++i) units[order[i].second].shown = int64_t(i);
    }
    // The HRD: the first access unit's SPS (its VPS's when the SPS has none).
    const hrd::Sps* first = nullptr;
    for (const Unit& u : units)
        if (auto it = sps.find(u.au.spsId); it != sps.end()) {
            first = &it->second;
            break;
        }
    if (!first) {
        r.note = "no sequence parameter set";
        return;
    }
    hrd::Sps model = *first;
    if (codec == kHevc && !model.active())
        if (auto it = vpsHrd.find(model.vpsId); it != vpsHrd.end()) {
            model.nal = it->second.nal, model.vcl = it->second.vcl;
            if (!model.timing) model.timing = true, model.numUnitsInTick = it->second.numUnitsInTick, model.timeScale = it->second.timeScale;
        }
    const AVStream* st = fmt->streams[v];
    const double tb = av_q2d(st->time_base);
    std::vector<double> removal(units.size()), initDelay(units.size(), 0), initSum(units.size(), 0);
    std::vector<int64_t> bits(units.size()), shown(units.size());
    std::vector<bool> bpStart(units.size(), false);
    for (size_t k = 0; k < units.size(); ++k) bits[k] = units[k].au.bits, shown[k] = units[k].shown;
    const hrd::Params* p = model.active();
    bool haveBp = false, havePt = true;
    for (const Unit& u : units) haveBp = haveBp || u.au.bp.present, havePt = havePt && u.au.pt.present;
    if (!p) {
        // No HRD stated: the level's limits, VBR, the buffer full before the first picture, removals at the container's times.
        double br = 0, cpb = 0, factor = 0;
        bool ok = false;
        if (codec == kH264) ok = h264LevelLimits(model.level, false, br, cpb), factor = h264NalFactor(model.profile);
        else ok = hevcLevelLimits(model.level, model.highTier, br, cpb), factor = model.profile <= 2 ? 1100 : 2200;
        if (!ok) {
            r.note = "no HRD stated, and level " + std::to_string(model.level) + " is not one whose limits are known";
            return;
        }
        r.signalled = false;
        r.model = codec == kH264 ? format("level %.1f limits", model.level == 9 ? 1.1 : model.level / 10.0)
                                 : format("level %.1f limits", model.level / 30.0);
        r.note = "no HRD is stated: checked against the level's highest bit rate and largest buffer, VBR, the buffer full before the first picture";
        r.bitRate = br * factor;
        r.bufferBits = cpb * factor;
        r.cbr = false;
        const double delay = r.bufferBits / r.bitRate;
        const int64_t d0 = units[0].dts != AV_NOPTS_VALUE ? units[0].dts : 0;
        for (size_t k = 0; k < units.size(); ++k) {
            const int64_t d = units[k].dts != AV_NOPTS_VALUE ? units[k].dts : d0;
            removal[k] = delay + double(d - d0) * tb;
            initSum[k] = delay;
            initDelay[k] = delay;
        }
        bpStart[0] = true;
        r.initialDelay = delay;
        runCpb(r, removal, bits, shown, bpStart, initDelay, initSum, r.bitRate, r.bufferBits, false, false);
        r.frames = int64_t(units.size());
        r.checked = true;
        return;
    }
    r.signalled = true;
    r.model = p == &model.nal ? "NAL HRD" : "VCL HRD";
    const hrd::Cpb& c = p->cpbs.front();
    r.bitRate = c.bitRate;
    r.bufferBits = c.size;
    r.cbr = c.cbr;
    if (p->subPic) r.note = "sub-picture parameters are not modelled: checked by access unit";
    if (p == &model.vcl)
        for (size_t k = 0; k < units.size(); ++k) bits[k] = units[k].au.vclBits;  // (Type I: the VCL and filler data units)
    // Start at the first buffering period.
    size_t start = 0;
    while (start < units.size() && !units[start].au.bp.present) ++start;
    if (!haveBp || start >= units.size()) {
        r.note = "the HRD is stated but no buffering period SEI is: checked with the buffer full before the first picture";
        start = 0;
    }
    if (start > 0) {
        units.erase(units.begin(), units.begin() + std::ptrdiff_t(start));
        removal.resize(units.size()), initDelay.resize(units.size()), initSum.resize(units.size()), bits.erase(bits.begin(), bits.begin() + std::ptrdiff_t(start));
        shown.erase(shown.begin(), shown.begin() + std::ptrdiff_t(start)), bpStart.resize(units.size());
        r.note = "the stream starts before its first buffering period: checked from it";
    }
    const bool usePt = havePt && model.tick() > 0;
    if (!usePt && r.note.empty()) r.note = "no picture timing on every picture: removals at the container's times";
    auto delaysOf = [&](const hrd::BufferingPeriod& bp, double& delay, double& sum) {
        const bool nalSet = p == &model.nal;
        const auto& d = nalSet ? bp.nalDelay : bp.vclDelay;
        const auto& o = nalSet ? bp.nalOffset : bp.vclOffset;
        if (d.empty()) return false;
        delay = double(d[0]) / 90000.0;
        sum = double(d[0] + (o.empty() ? 0 : o[0])) / 90000.0;
        return true;
    };
    double curDelay = c.size / c.bitRate, curSum = curDelay;
    double bpRemoval = 0, prevNonDiscardable = 0;
    const int64_t d0 = units[0].dts != AV_NOPTS_VALUE ? units[0].dts : 0;
    for (size_t k = 0; k < units.size(); ++k) {
        const Unit& u = units[k];
        const hrd::Sps* s = &model;
        if (auto it = sps.find(u.au.spsId); it != sps.end()) s = &it->second;
        const double tc = s->tick() > 0 ? s->tick() : model.tick();
        const bool bp = u.au.bp.present || (k == 0);
        double delay = curDelay, sum = curSum;
        if (u.au.bp.present) delaysOf(u.au.bp, delay, sum);
        if (k == 0) {
            removal[0] = delay;
            r.initialDelay = delay;
        } else if (usePt) {
            const double base = bp && codec == kHevc && u.au.bp.concatenation ? 0 : bpRemoval;
            int64_t ticks = u.au.pt.cpbRemovalDelay;
            const hrd::Params* sp = s->active() ? s->active() : p;
            const double wrap = std::ldexp(1.0, sp->cpbDelayLength);
            double t = base + tc * double(ticks);
            if (bp && codec == kHevc && u.au.bp.concatenation) {
                // (C-10 with concatenation_flag: from the last picture that is not discardable, or as the buffer allows)
                const double t1 = prevNonDiscardable + tc * double(u.au.bp.auCpbRemovalDelayDeltaMinus1 + 1);
                t = t1;
                (void)wrap;
            } else {
                // (cpb_removal_delay counts modulo 2^length: the next time after the access unit before)
                while (t <= removal[k - 1] + 1e-9 && wrap * tc < 1e6) t += tc * wrap;
            }
            removal[k] = t;
        } else {
            const int64_t d = u.dts != AV_NOPTS_VALUE ? u.dts : d0;
            removal[k] = removal[0] + double(d - d0) * tb;
        }
        if (bp) {
            bpRemoval = removal[k];
            curDelay = delay, curSum = sum;
        }
        bpStart[k] = bp;
        initDelay[k] = delay;
        initSum[k] = curSum;
        if (codec == kHevc && u.temporalId == 0 && !(u.type >= 6 && u.type <= 9) && !(u.type <= 14 && u.type % 2 == 0)) prevNonDiscardable = removal[k];
        if (codec == kH264) prevNonDiscardable = removal[k];
    }
    runCpb(r, removal, bits, shown, bpStart, initDelay, initSum, c.bitRate, c.size, c.cbr, p->lowDelay);
    r.frames = int64_t(units.size());
    r.checked = true;
}

void checkMpeg2(AVFormatContext* fmt, int v, HrdReport& r, const std::atomic<bool>* cancel) {
    Mpeg2Stream st;
    std::vector<Mpeg2Picture> pics;
    std::vector<int64_t> pts;
    size_t pos = 0;
    AVPacket* pkt = av_packet_alloc();
    while (pkt && av_read_frame(fmt, pkt) >= 0) {
        if (cancel && cancel->load()) break;
        if (pkt->stream_index == v) {
            const size_t before = pics.size();
            mpeg2Pictures(pkt->data, size_t(pkt->size), st, pics, pos);
            for (size_t i = before; i < pics.size(); ++i) pts.push_back(pkt->pts);
            pos += size_t(pkt->size);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (pics.empty() || st.bitRate <= 0 || st.buffer <= 0) {
        r.note = pics.empty() ? "no pictures" : "no sequence header with a bit rate and buffer size";
        return;
    }
    r.model = "VBV";
    r.signalled = true;
    r.bitRate = st.bitRate;
    r.bufferBits = st.buffer;
    bool cbr = true;
    for (const Mpeg2Picture& p : pics) cbr = cbr && p.vbvDelay >= 0;
    r.cbr = cbr;
    const AVStream* s = fmt->streams[v];
    const AVRational rate = s->avg_frame_rate.num > 0 ? s->avg_frame_rate : s->r_frame_rate;
    const double frame = rate.num > 0 ? av_q2d(av_inv_q(rate)) : 0.04;
    std::vector<int64_t> shown(pics.size());
    {
        std::vector<std::pair<int64_t, size_t>> order;
        for (size_t k = 0; k < pics.size(); ++k) order.push_back({pts[k] == AV_NOPTS_VALUE ? int64_t(k) : pts[k], k});
        std::stable_sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size(); ++i) shown[order[i].second] = int64_t(i);
    }
    // Removal times one picture's display apart, in decoding order (a field picture half a frame; a repeated field half again).
    const size_t n = pics.size();
    std::vector<double> removal(n);
    auto lasts = [&](const Mpeg2Picture& p) {
        if (p.structure != 3) return frame / 2;
        if (!p.repeatFirstField) return frame;
        return st.progressiveSequence ? frame * (p.topFieldFirst ? 3 : 2) : frame * 1.5;
    };
    const double R = st.bitRate, B = st.buffer;
    std::vector<double> end(n);  // bits up to the end of each picture's data
    for (size_t k = 0; k < n; ++k) end[k] = k + 1 < n ? double(pics[k + 1].start) * 8 : double(pos) * 8;
    if (cbr) {
        // The delays say when each start code arrives; between them the bits come at the rate they imply, which must not
        // pass the stated one.
        std::vector<double> at(n);
        removal[0] = double(pics[0].psc) * 8 / R + double(pics[0].vbvDelay) / 90000;
        for (size_t k = 1; k < n; ++k) removal[k] = removal[k - 1] + lasts(pics[k - 1]);
        for (size_t k = 0; k < n; ++k) at[k] = removal[k] - double(pics[k].vbvDelay) / 90000;
        r.initialDelay = double(pics[0].vbvDelay) / 90000;
        std::vector<double> segRate(n, R);
        for (size_t k = 0; k + 1 < n; ++k) {
            const double bitsBetween = double(pics[k + 1].psc - pics[k].psc) * 8;
            const double dt = at[k + 1] - at[k];
            if (dt <= 0) {
                addIssue(r, int64_t(k + 1), shown[k + 1], "timing", "its vbv_delay puts its start code before the picture before it");
                segRate[k] = R;
                continue;
            }
            segRate[k] = bitsBetween / dt;
            if (dt < bitsBetween / R - 1.5 / 90000)
                addIssue(r, int64_t(k + 1), shown[k + 1], "rate",
                         format("the vbv_delays have the bits before it come at %.4g Mb/s, over the stated %.4g Mb/s", segRate[k] / 1e6, R / 1e6));
        }
        // Bits arrived by time t.
        auto arrived = [&](double t) {
            if (t <= at[0]) return std::max(0.0, double(pics[0].psc) * 8 - (at[0] - t) * R);
            size_t lo = 0, hi = n - 1;
            if (t >= at[n - 1]) return double(pics[n - 1].psc) * 8 + (t - at[n - 1]) * R;
            while (hi - lo > 1) {
                const size_t mid = (lo + hi) / 2;
                (at[mid] <= t ? lo : hi) = mid;
            }
            return double(pics[lo].psc) * 8 + (t - at[lo]) * segRate[lo];
        };
        for (size_t k = 0; k < n; ++k) {
            const double startBits = double(pics[k].start) * 8;
            const double have = std::min(arrived(removal[k]), double(pos) * 8);
            if (have + 1 < end[k])
                addIssue(r, int64_t(k), shown[k], "underflow",
                         format("%.0f of the picture's bits have not arrived when it is to be decoded", end[k] - have));
            const double full = have - startBits;
            if (full > B + 16)
                addIssue(r, int64_t(k), shown[k], "overflow", format("the buffer would hold %.0f bits of its %.0f", full, B));
            r.highest = std::max(r.highest, full / B);
            if (k + 1 < n) r.lowest = std::min(r.lowest, std::max(0.0, have - end[k]) / B);
        }
    } else {
        // Variable rate: filled at the stated rate until full; the first picture decoded once it is full (or all is in).
        r.note = "variable rate (vbv_delay 0xFFFF): the buffer filled at the stated rate until full before the first picture";
        double t = std::min(B, double(pos) * 8) / R, arrived = std::min(B, double(pos) * 8), removed = 0;
        r.initialDelay = t;
        for (size_t k = 0; k < n; ++k) {
            if (k > 0) {
                const double dt = lasts(pics[k - 1]);
                t += dt;
                arrived = std::min({arrived + dt * R, removed + B, double(pos) * 8});
            }
            if (arrived + 1 < end[k])
                addIssue(r, int64_t(k), shown[k], "underflow",
                         format("%.0f of the picture's bits have not arrived when it is to be decoded", end[k] - arrived));
            r.highest = std::max(r.highest, (arrived - double(pics[k].start) * 8) / B);
            removed = end[k];
            if (k + 1 < n) r.lowest = std::min(r.lowest, std::max(0.0, arrived - removed) / B);
        }
    }
    r.frames = int64_t(n);
    r.checked = true;
}

}  // namespace

HrdReport checkHrd(const std::string& path, const std::atomic<bool>* cancel) {
    HrdReport r;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        r.error = "cannot open " + path;
        return r;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        r.error = "cannot read " + path;
        return r;
    }
    const int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v < 0) {
        avformat_close_input(&fmt);
        r.note = "no video";
        return r;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != v) fmt->streams[i]->discard = AVDISCARD_ALL;
    const int codec = fmt->streams[v]->codecpar->codec_id;
    if (codec == kH264 || codec == kHevc) {
        r.codec = codec == kH264 ? "H.264" : "HEVC";
        checkNal(codec, fmt, v, r, cancel);
    } else if (codec == kMpeg2) {
        r.codec = "MPEG-2";
        checkMpeg2(fmt, v, r, cancel);
    } else {
        r.codec = avcodec_get_name(AVCodecID(codec));
        r.note = r.codec + " has no buffer model to check here (H.264, HEVC and MPEG-2 do)";
    }
    avformat_close_input(&fmt);
    std::stable_sort(r.issues.begin(), r.issues.end(), [](const HrdIssue& a, const HrdIssue& b) { return a.frame < b.frame; });
    return r;
}

// ---- Joining ----

bool HrdSplicer::start(int codec, const std::vector<std::vector<uint8_t>>& sets, int lengthSize, double fps, int depth, double mpeg2Rate,
                       double mpeg2Buffer) {
    codec_ = codec;
    lengthSize_ = lengthSize;
    fps_ = fps > 0 ? fps : 25;
    frame_ = 1.0 / fps_;
    depth_ = depth;
    active_ = false;
    if (codec == kMpeg2) {
        if (mpeg2Rate <= 0 || mpeg2Buffer <= 0) return false;
        rate_ = mpeg2Rate, size_ = mpeg2Buffer, cbr_ = true;
        active_ = true;
        return true;
    }
    for (const auto& s : sets) addParameterSet(s.data(), s.size());
    return active_;
}

void HrdSplicer::addParameterSet(const uint8_t* d, size_t n) {
    if (n < 2) return;
    const int type = codec_ == kH264 ? (d[0] & 31) : (d[0] >> 1) & 0x3f;
    if (codec_ == kH264 && type == 7) {
        hrd::Sps s = hrd::parseH264Sps(d, n);
        if (s.valid) sps_[s.id] = s;
    } else if (codec_ == kH264 && type == 8) {
        hrd::Pps p = hrd::parseH264Pps(d, n, sps_);
        if (p.id >= 0) pps_[p.id] = p;
    } else if (codec_ == kHevc && type == 33) {
        hrd::Sps s = hrd::parseHevcSps(d, n);
        if (s.valid) sps_[s.id] = s;
    } else if (codec_ == kHevc && type == 34) {
        hrd::Pps p = hrd::parseHevcPps(d, n);
        if (p.valid) pps_[p.id] = p;
    } else {
        return;
    }
    if (active_ || type != (codec_ == kH264 ? 7 : 33)) return;
    for (const auto& [id, sp] : sps_) {
        const hrd::Params* p = sp.active();
        if (!p || p->cpbs.empty() || sp.tick() <= 0) continue;
        rate_ = p->cpbs[0].bitRate;
        size_ = p->cpbs[0].size;
        cbr_ = p->cpbs[0].cbr;
        sum90_ = int64_t(std::floor(size_ / rate_ * 90000));
        hrdSps_.assign(d, d + n);
        active_ = true;
        break;
    }
}

int64_t HrdSplicer::bitsOf(const std::vector<uint8_t>& au) const {
    if (codec_ == kMpeg2) return int64_t(au.size()) * 8;
    return lengthSize_ > 0 ? streamBits(splitNals(au.data(), au.size(), lengthSize_)) : int64_t(au.size()) * 8;
}


double HrdSplicer::askedDelay(const uint8_t* d, size_t n) const {
    if (codec_ == kMpeg2) {
        Mpeg2Stream st;
        std::vector<Mpeg2Picture> pics;
        mpeg2Pictures(d, n, st, pics, 0);
        return pics.empty() || pics[0].vbvDelay < 0 ? -1 : double(pics[0].vbvDelay) / 90000;
    }
    std::map<int, hrd::Sps> sps = sps_;
    std::map<int, hrd::Pps> pps = pps_;
    hrd::AccessUnit au;
    if (!hrd::readAccessUnit(codec_, d, n, lengthSize_, sps, pps, au) || !au.bp.present) return -1;
    const auto& delay = !au.bp.nalDelay.empty() ? au.bp.nalDelay : au.bp.vclDelay;
    return delay.empty() ? -1 : double(delay[0]) / 90000;
}

namespace {

// MPEG-2: bits from a picture's data start to the end of its picture start code (what arrives before its vbv_delay runs).
double mpeg2Lead(const std::vector<uint8_t>& au) {
    Mpeg2Stream st;
    std::vector<Mpeg2Picture> pics;
    mpeg2Pictures(au.data(), au.size(), st, pics, 0);
    return pics.empty() ? 0 : double(pics[0].psc) * 8;
}

}  // namespace

double HrdSplicer::nextDelay() const {
    if (!havePending_) return firstDelay();
    const double nextRemoval = t0_ + double(count_ + 1) * frame_;
    const double end = pending_.arrival + double(bitsOf(pending_.au)) / rate_;
    return nextRemoval - end;
}

void HrdSplicer::pad(Held& h, int64_t bytes) {
    if (bytes <= 0) return;
    if (codec_ == kMpeg2) {
        h.au.insert(h.au.end(), size_t(bytes), 0);  // (zero bytes before the next start code)
        return;
    }
    // Filler data NAL units (H.264 type 12, HEVC type 38) after the picture's own: start code, header, 0xFF..., 0x80.
    const int hb = headerBytes(codec_);
    const int64_t overhead = 4 + hb + 1;
    // (fewer bytes than a filler unit's own: none, a few bytes short, rather than a whole unit's more than asked, which
    // would overflow the buffer, or arrive after the picture's removal)
    if (bytes < overhead) return;
    int64_t payload = bytes - overhead;
    Nals nals = splitNals(h.au.data(), h.au.size(), lengthSize_);
    int tid = 1;
    for (const auto& u : nals)
        if (codec_ == kHevc && u.size() > 1 && isVcl(codec_, nalType(codec_, u))) {
            tid = u[1] & 7;
            break;
        }
    const int64_t most = lengthSize_ == 1 ? 250 : lengthSize_ == 2 ? 65000 : lengthSize_ == 3 ? (int64_t(1) << 24) - 8 : int64_t(1) << 26;
    while (payload >= 0) {
        const int64_t take = std::min(payload, most);
        std::vector<uint8_t> f;
        if (codec_ == kH264) f.push_back(12);
        else f.push_back(uint8_t(38 << 1)), f.push_back(uint8_t(tid));
        f.insert(f.end(), size_t(take), 0xff);
        f.push_back(0x80);
        nals.push_back(std::move(f));
        payload -= take;
        if (payload <= 0) break;
        payload -= overhead;
    }
    h.au = joinNals(nals, lengthSize_);
}

void HrdSplicer::finalize(Held& h, double nextRemoval, double nextAsked) {
    double bits = double(bitsOf(h.au));
    if (cbr_ && h.mayPad) {
        // Where the next access unit's removal finds the buffer (its bits there): no more than it holds, and no more than
        // the next part's own buffering period asks for.
        double end = h.arrival + bits / rate_;
        // (two 90 kHz ticks short of full, so that delays rounded to the tick never put it over)
        double want = size_ / rate_ - 2.0 / 90000;
        if (nextAsked >= 0) want = std::min(want, nextAsked);
        if (codec_ == kMpeg2 && nextAsked >= 0) want = nextAsked;  // (MPEG-2: vbv_delay runs from the start code)
        const double over = nextRemoval - end - want;
        // (never so much that this one's own bits arrive after it is removed)
        const double room = h.removal - end;
        const double padSeconds = std::min(over, room);
        if (padSeconds > 0) {
            pad(h, int64_t(std::floor(padSeconds * rate_ / 8)));
            bits = double(bitsOf(h.au));
        }
    }
    // (a picture too large to arrive by its removal underflows here: nothing can be done about it, and the written
    // stream's check, checkHrd, reports it)
    lastFinal_ = h.arrival + bits / rate_;
    if (codec_ == kMpeg2) mpegBits_ += bits;
    ++count_;
}

std::vector<uint8_t> HrdSplicer::push(std::vector<uint8_t> au, int64_t display, bool ours, bool mayPad, int64_t* prevDisplay) {
    std::vector<uint8_t> out;
    const int64_t k = count_ + (havePending_ ? 1 : 0);
    const double removal = t0_ + double(k) * frame_;
    if (havePending_) {
        double asked = -1;
        if (!ours) asked = askedDelay(au.data(), au.size());
        if (codec_ == kMpeg2 && asked >= 0) {
            // (the next start code's own lead: the delay runs from it)
            asked += mpeg2Lead(au) / rate_;
        }
        finalize(pending_, removal, asked);
        out = std::move(pending_.au);
        if (prevDisplay) *prevDisplay = pending_.display;
        havePending_ = false;
    }
    Held h;
    h.au = std::move(au);
    h.display = display;
    h.ours = ours;
    h.mayPad = mayPad;
    h.removal = removal;
    rewrite(h, k);
    // Its bits begin arriving after the one before's (CBR), or no earlier than the period allows (VBR).
    h.arrival = k == 0 ? 0 : lastFinal_;
    if (codec_ == kMpeg2 && k == 0) h.removal = t0_;
    if (!cbr_ && codec_ != kMpeg2 && k > 0) {
        // (C.1.2: a buffering period's first access unit no earlier than its initial delay before its removal; the
        // others no earlier than the delay and offset together)
        const double earliest = h.removal - double(h.bp ? init90_ : init90_ + offset90_) / 90000;
        h.arrival = std::max(h.arrival, earliest);
    }
    pending_ = std::move(h);
    havePending_ = true;
    return out;
}

std::vector<uint8_t> HrdSplicer::finish(int64_t* prevDisplay) {
    if (!havePending_) return {};
    finalize(pending_, t0_ + double(count_ + 1) * frame_, -1);
    havePending_ = false;
    if (prevDisplay) *prevDisplay = pending_.display;
    return std::move(pending_.au);
}

bool HrdSplicer::rewrite(Held& h, int64_t k) {
    if (codec_ == kMpeg2) {
        // Every picture's vbv_delay as the model has it (the first's sets the model, ours the delay asked for); ours'
        // sequence headers state the buffer the stream has.
        Mpeg2Stream st;
        std::vector<Mpeg2Picture> pics;
        mpeg2Pictures(h.au.data(), h.au.size(), st, pics, 0);
        if (pics.empty()) return false;
        const double psc = (mpegBits_ + double(pics[0].psc) * 8) / rate_;
        if (k == 0) {
            const double d = h.ours || pics[0].vbvDelay < 0 ? firstDelay() : double(pics[0].vbvDelay) / 90000;
            t0_ = psc + d;
            h.removal = t0_;
        }
        const int64_t want = std::clamp<int64_t>(int64_t(std::llround((h.removal - psc) * 90000)), 0, 0xfffe);
        if (h.ours || pics[0].vbvDelay < 0 || pics[0].vbvDelay != want) {
            uint8_t* p = h.au.data() + pics[0].psc;  // (the temporal reference's 10 bits, the type's 3, then the delay's 16)
            uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
            v = (v & ~(uint32_t(0xffff) << 3)) | (uint32_t(want) << 3);
            p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
        }
        if (h.ours) {
            // (sequence header: vbv_buffer_size_value's 10 bits after 12+12+4+4+18+1; its extension's 8 after 4+8+1+2+2+2+12+1)
            const uint32_t units = uint32_t(std::llround(size_ / 16384));
            for (size_t i = 0; i + 12 <= h.au.size(); ++i) {
                if (h.au[i] || h.au[i + 1] || h.au[i + 2] != 1) continue;
                if (h.au[i + 3] == 0xb3) {
                    uint8_t* q = h.au.data() + i + 4;
                    // bits 51..60 from q
                    for (int b = 0; b < 10; ++b) {
                        const size_t bit = 51 + size_t(b);
                        const int val = (units >> (9 - b)) & 1;
                        q[bit / 8] = uint8_t((q[bit / 8] & ~(0x80 >> (bit % 8))) | (val ? 0x80 >> (bit % 8) : 0));
                    }
                } else if (h.au[i + 3] == 0xb5 && (h.au[i + 4] >> 4) == 1) {
                    uint8_t* q = h.au.data() + i + 4;
                    for (int b = 0; b < 8; ++b) {
                        const size_t bit = 32 + size_t(b);
                        const int val = (units >> (17 - b)) & 1;
                        q[bit / 8] = uint8_t((q[bit / 8] & ~(0x80 >> (bit % 8))) | (val ? 0x80 >> (bit % 8) : 0));
                    }
                }
            }
        }
        return true;
    }
    Nals nals = splitNals(h.au.data(), h.au.size(), lengthSize_);
    // Parameter sets carried in the access unit, then its SPS (from its first slice).
    int spsId = -1, firstVcl = -1, firstSei = -1, tid = 1;
    bool irap = false;
    for (size_t i = 0; i < nals.size(); ++i) {
        const auto& u = nals[i];
        const int type = nalType(codec_, u);
        if (isParameterSet(codec_, type)) addParameterSet(u.data(), u.size());
        if ((codec_ == kH264 ? type == 6 : type == 39) && firstSei < 0 && firstVcl < 0) firstSei = int(i);
        if (isVcl(codec_, type) && firstVcl < 0) {
            firstVcl = int(i);
            const std::vector<uint8_t> rb = nal::unescape(u.data(), std::min<size_t>(u.size(), 32));
            nal::Reader r(rb, size_t(headerBytes(codec_)) * 8);
            if (codec_ == kH264) {
                irap = type == 5;
                r.ue(), r.ue();
            } else {
                irap = type >= 16 && type <= 23;
                tid = u.size() > 1 ? u[1] & 7 : 1;
                r.flag();
                if (irap) r.flag();
            }
            if (auto it = pps_.find(int(r.ue())); it != pps_.end()) spsId = it->second.spsId;
        }
    }
    auto it = sps_.find(spsId);
    if (it == sps_.end() || !it->second.active() || firstVcl < 0) return false;  // (its parameter sets state no HRD: left as it is)
    const hrd::Sps& s = it->second;
    const double tc = s.tick();
    const int64_t perFrame = std::max<int64_t>(1, std::llround(frame_ / tc));
    if (h.ours) {
        // An encoded part's own buffering periods and picture timing (an encoder's HRD, whose field lengths the stream's
        // SPS no longer states) go: the stream's are written in their place.
        Nals kept;
        for (size_t i = 0; i < nals.size(); ++i) {
            const int type = nalType(codec_, nals[i]);
            std::vector<SeiMessage> m;
            if ((codec_ == kH264 ? type == 6 : type == 39) && parseSei(nal::unescape(nals[i].data(), nals[i].size()), headerBytes(codec_), m)) {
                const size_t before = m.size();
                std::erase_if(m, [](const SeiMessage& x) { return x.type == 0 || x.type == 1; });
                if (m.size() != before) {
                    if (!m.empty()) kept.push_back(buildSei(nals[i].data(), headerBytes(codec_), m));
                    continue;
                }
            }
            kept.push_back(std::move(nals[i]));
        }
        nals = std::move(kept);
        firstVcl = firstSei = -1;
        for (size_t i = 0; i < nals.size(); ++i) {
            const int type = nalType(codec_, nals[i]);
            if ((codec_ == kH264 ? type == 6 : type == 39) && firstSei < 0 && firstVcl < 0) firstSei = int(i);
            if (isVcl(codec_, type) && firstVcl < 0) firstVcl = int(i);
        }
    }
    // Its buffering period and picture timing, as they are.
    struct Found {
        int nal = -1;
        size_t message = 0;
    } bpAt, ptAt;
    std::vector<std::vector<SeiMessage>> seis(nals.size());
    hrd::BufferingPeriod bp;
    hrd::PictureTiming pt;
    size_t restFrom = 0, restTo = 0;
    for (size_t i = 0; i < nals.size(); ++i) {
        const int type = nalType(codec_, nals[i]);
        if (codec_ == kH264 ? type != 6 : type != 39) continue;
        if (!parseSei(nal::unescape(nals[i].data(), nals[i].size()), headerBytes(codec_), seis[i])) continue;
        for (size_t m = 0; m < seis[i].size(); ++m) {
            if (seis[i][m].type == 0 && bpAt.nal < 0 && readBp(codec_, seis[i][m].payload, sps_, bp)) bpAt = {int(i), m};
            if (seis[i][m].type == 1 && ptAt.nal < 0 && readPt(codec_, seis[i][m].payload, s, pt, &restFrom, &restTo)) ptAt = {int(i), m};
        }
    }
    const bool isBp = bpAt.nal >= 0 || (h.ours && (irap || k == 0));
    // The buffering period: the first sets the model; later ones are given where the buffer stands.
    hrd::BufferingPeriod nbp = bp;
    if (isBp) {
        if (bpAt.nal < 0) {
            nbp = {};
            nbp.present = true;
            nbp.spsId = s.id;
        }
        int64_t delay = 0;
        const auto& old = !bp.nalDelay.empty() ? bp.nalDelay : bp.vclDelay;
        const int64_t had = old.empty() ? -1 : old[0];
        const auto& oldOff = !bp.nalOffset.empty() ? bp.nalOffset : bp.vclOffset;
        const int64_t sum = had >= 0 ? had + (oldOff.empty() ? 0 : oldOff[0]) : sum90_;
        if (k == 0) {
            delay = had >= 0 && !h.ours ? had : int64_t(std::llround(firstDelay() * 90000));
            t0_ = double(delay) / 90000;
            h.removal = t0_;
        } else {
            const double g = 90000.0 * (h.removal - lastFinal_);
            if (had >= 0 && !h.ours && (cbr_ ? (had >= std::floor(g + 1e-6) && had <= std::ceil(g - 1e-6)) : had <= std::ceil(g - 1e-6)))
                delay = had;
            else
                delay = std::max<int64_t>(1, int64_t(std::floor(g + 1e-6)));
        }
        delay = std::min(delay, sum > 0 ? sum : delay);
        const int64_t offset = std::max<int64_t>(0, sum - delay);
        for (auto* v : {&nbp.nalDelay, &nbp.vclDelay}) {
            const bool nalSet = v == &nbp.nalDelay;
            const hrd::Params& p = nalSet ? s.nal : s.vcl;
            if (!p.present) {
                v->clear();
                continue;
            }
            v->assign(p.cpbs.size(), delay);
            (nalSet ? nbp.nalOffset : nbp.vclOffset).assign(p.cpbs.size(), offset);
        }
        if (codec_ == kHevc) {
            nbp.concatenation = false;
            nbp.irapCpbParams = false;
            nbp.nalAltDelay.clear(), nbp.nalAltOffset.clear(), nbp.vclAltDelay.clear(), nbp.vclAltOffset.clear();
        }
        init90_ = delay, offset90_ = offset;
        h.bp = true;
    }
    // Picture timing: removal one frame after the one before; output as its timestamps place it.
    hrd::PictureTiming npt = pt;
    npt.present = true;
    npt.cpbRemovalDelay = k == 0 ? (codec_ == kHevc ? 1 : 0) : (k - bpIndex_) * perFrame;
    // (one output delay for the whole stream, the most any picture of it is decoded ahead of where it is shown, so
    // output times keep display order across every join)
    npt.dpbOutputDelay = std::max<int64_t>(0, (h.display - k + depth_) * perFrame);
    if (isBp) bpIndex_ = k;
    // Written back only where something changed.
    bool changed = false;
    std::vector<uint8_t> newBp, newPt;
    if (isBp) {
        newBp = writeBp(codec_, nbp, s);
        changed = bpAt.nal < 0 || newBp != seis[size_t(bpAt.nal)][bpAt.message].payload;
    }
    const std::vector<uint8_t>* oldPt = ptAt.nal >= 0 ? &seis[size_t(ptAt.nal)][ptAt.message].payload : nullptr;
    const bool ptChanged = !oldPt || pt.cpbRemovalDelay % (int64_t(1) << s.active()->cpbDelayLength) !=
                                         npt.cpbRemovalDelay % (int64_t(1) << s.active()->cpbDelayLength) ||
                           pt.dpbOutputDelay != npt.dpbOutputDelay;
    if (ptChanged) newPt = writePt(codec_, npt, s, oldPt, restFrom, restTo);
    if (!changed && !ptChanged) return true;
    uint8_t header[2] = {uint8_t(codec_ == kH264 ? 6 : 39 << 1), uint8_t(tid)};
    if (bpAt.nal >= 0 || ptAt.nal >= 0) {
        // In place, in the SEI units that held them.
        if (isBp && bpAt.nal >= 0) seis[size_t(bpAt.nal)][bpAt.message].payload = newBp;
        if (ptChanged && ptAt.nal >= 0) seis[size_t(ptAt.nal)][ptAt.message].payload = newPt;
        for (int i : {bpAt.nal, ptAt.nal})
            if (i >= 0) {
                const uint8_t* hd = nals[size_t(i)].data();
                nals[size_t(i)] = buildSei(hd, headerBytes(codec_), seis[size_t(i)]);
            }
        std::vector<SeiMessage> extra;
        if (isBp && bpAt.nal < 0) extra.push_back({0, newBp});
        if (ptChanged && ptAt.nal < 0) extra.push_back({1, newPt});
        if (!extra.empty()) nals.insert(nals.begin() + (firstSei >= 0 ? firstSei : firstVcl), buildSei(header, headerBytes(codec_), extra));
    } else {
        // Ours: one SEI unit, the buffering period first, before any other SEI and the pictures.
        std::vector<SeiMessage> m;
        if (isBp) m.push_back({0, newBp});
        m.push_back({1, newPt});
        nals.insert(nals.begin() + (firstSei >= 0 ? firstSei : firstVcl), buildSei(header, headerBytes(codec_), m));
    }
    h.au = joinNals(nals, lengthSize_);
    return true;
}

}  // namespace montage
