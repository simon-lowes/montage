// Montage — AV1 sequence and frame headers read, and film grain parameters written into them (see Av1Headers.h).
#include "render/Av1Headers.h"

#include <algorithm>
#include <cstring>

namespace montage {

namespace {

enum { kObuSequence = 1, kObuDelimiter = 2, kObuFrameHeader = 3, kObuTileGroup = 4, kObuFrame = 6, kObuRedundantHeader = 7 };
enum { kKeyFrame = 0, kInterFrame = 1, kIntraOnlyFrame = 2, kSwitchFrame = 3 };

// Bits read most significant first. Reading past the end gives zeros and sets `bad`.
struct Bits {
    const uint8_t* p = nullptr;
    size_t n = 0;  // bytes
    size_t at = 0; // bits
    bool bad = false;
    uint32_t f(int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i) {
            if (at >= n * 8) {
                bad = true;
                return 0;
            }
            v = (v << 1) | ((p[at >> 3] >> (7 - (at & 7))) & 1u);
            ++at;
        }
        return v;
    }
    bool flag() { return f(1) != 0; }
    int su(int bits) {
        int v = int(f(bits));
        const int sign = 1 << (bits - 1);
        if (v & sign) v -= 2 * sign;
        return v;
    }
    // A number below `count`, in as few bits as hold it.
    uint32_t ns(uint32_t count) {
        if (count <= 1) return 0;
        int w = 0;
        for (uint32_t x = count; x; x >>= 1) ++w;
        const uint32_t m = (1u << w) - count, v = f(w - 1);
        if (v < m) return v;
        return (v << 1) - m + f(1);
    }
    uint32_t uvlc() {
        int zeros = 0;
        while (!bad && !flag()) {
            if (++zeros >= 32) return 0xffffffffu;
        }
        return zeros ? f(zeros) + (1u << zeros) - 1 : 0;
    }
};

struct BitWriter {
    std::vector<uint8_t> bytes;
    size_t bits = 0;
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if ((bits & 7) == 0) bytes.push_back(0);
            if ((v >> i) & 1u) bytes.back() |= uint8_t(0x80 >> (bits & 7));
            ++bits;
        }
    }
    void copy(const uint8_t* p, size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) put((p[i >> 3] >> (7 - (i & 7))) & 1u, 1);
    }
    void align() {
        while (bits & 7) put(0, 1);
    }
};

bool readLeb128(const uint8_t* p, size_t n, size_t& at, uint64_t& v) {
    v = 0;
    for (int i = 0; i < 8; ++i) {
        if (at >= n) return false;
        const uint8_t b = p[at++];
        v |= uint64_t(b & 0x7f) << (i * 7);
        if (!(b & 0x80)) return true;
    }
    return false;
}

void writeLeb128(std::vector<uint8_t>& out, uint64_t v) {
    do {
        uint8_t b = uint8_t(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

int tileLog2(int blk, int target) {
    int k = 0;
    while ((int64_t(blk) << k) < target) ++k;
    return k;
}

bool no(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

// Where a codec configuration's OBUs begin: after an av1C box's four bytes, or at once.
size_t configObus(const uint8_t* data, size_t size) { return size >= 4 && data[0] == 0x81 ? 4 : 0; }

// A film grain's parameters as a sequence can carry them (no chroma points for a monochrome one, or for a 4:2:0 one
// with no luma points, or beside chroma scaled from the luma; in 4:2:0, a plane with no points beside one with them
// given the other's points scaled to nothing, so that both have points, as decoders require).
Av1FilmGrain grainFor(const Av1FilmGrain& in, const Av1Sequence& seq) {
    Av1FilmGrain g = in;
    if (seq.mono) g.chromaFromLuma = false;
    if (seq.mono || g.chromaFromLuma || (seq.subX && seq.subY && g.numY == 0)) g.numCb = g.numCr = 0;
    if (seq.subX && seq.subY && (g.numCb > 0) != (g.numCr > 0)) {
        if (g.numCr == 0) {
            g.numCr = g.numCb;
            for (int i = 0; i < g.numCr; ++i) g.crValue[i] = g.cbValue[i], g.crScaling[i] = 0;
            std::fill(std::begin(g.arCr), std::end(g.arCr), int8_t(0));
        } else {
            g.numCb = g.numCr;
            for (int i = 0; i < g.numCb; ++i) g.cbValue[i] = g.crValue[i], g.cbScaling[i] = 0;
            std::fill(std::begin(g.arCb), std::end(g.arCb), int8_t(0));
        }
    }
    return g;
}

}  // namespace

bool Av1HeaderReader::splitObus(const uint8_t* data, size_t size, std::vector<Obu>& out, std::string* error) {
    out.clear();
    size_t at = 0;
    while (at < size) {
        Obu o;
        o.start = at;
        const uint8_t head = data[at++];
        if (head & 0x80) return no(error, "The AV1 stream has an OBU with its forbidden bit set");
        o.type = (head >> 3) & 15;
        o.extension = head & 4, o.hasSize = head & 2;
        if (o.extension) {
            if (at >= size) return no(error, "The AV1 stream ends inside an OBU's header");
            o.temporalId = data[at] >> 5, o.spatialId = (data[at] >> 3) & 3;
            ++at;
        }
        uint64_t len = size - at;
        if (o.hasSize && !readLeb128(data, size, at, len)) return no(error, "The AV1 stream ends inside an OBU's size");
        if (len > size - at) return no(error, "An OBU of the AV1 stream is longer than its packet");
        o.payload = at, o.size = size_t(len);
        at += size_t(len);
        out.push_back(o);
    }
    return true;
}

bool Av1HeaderReader::readSequence(const uint8_t* p, size_t n, std::string* error) {
    Bits b{p, n};
    Av1Sequence s;
    s.profile = int(b.f(3));
    s.stillPicture = b.flag();
    s.reducedStill = b.flag();
    int bufferDelayBits = 0;
    bool displayDelay = false;
    if (s.reducedStill) {
        b.f(5);
        s.operatingPoints = 1;
    } else {
        if (b.flag()) {  // timing_info_present_flag
            b.f(32), b.f(32);
            s.equalPictureInterval = b.flag();
            if (s.equalPictureInterval) b.uvlc();
            s.decoderModel = b.flag();
            if (s.decoderModel) {
                bufferDelayBits = int(b.f(5)) + 1;
                b.f(32);
                s.bufferRemovalBits = int(b.f(5)) + 1;
                s.presentationBits = int(b.f(5)) + 1;
            }
        }
        displayDelay = b.flag();
        s.operatingPoints = int(b.f(5)) + 1;
        for (int i = 0; i < s.operatingPoints; ++i) {
            s.operatingPointIdc[i] = int(b.f(12));
            if (b.f(5) > 7) b.f(1);  // seq_level_idx, seq_tier
            if (s.decoderModel) {
                s.decoderModelForOp[i] = b.flag();
                if (s.decoderModelForOp[i]) b.f(bufferDelayBits), b.f(bufferDelayBits), b.f(1);
            }
            if (displayDelay && b.flag()) b.f(4);
        }
    }
    s.widthBits = int(b.f(4)) + 1, s.heightBits = int(b.f(4)) + 1;
    s.maxWidth = int(b.f(s.widthBits)) + 1, s.maxHeight = int(b.f(s.heightBits)) + 1;
    s.frameIds = s.reducedStill ? false : b.flag();
    if (s.frameIds) {
        s.deltaFrameIdBits = int(b.f(4)) + 2;
        s.frameIdBits = int(b.f(3)) + 1 + s.deltaFrameIdBits;
    }
    s.use128 = b.flag();
    b.f(2);  // enable_filter_intra, enable_intra_edge_filter
    if (!s.reducedStill) {
        b.f(1), b.f(1);  // enable_interintra_compound, enable_masked_compound
        s.enableWarpedMotion = b.flag();
        b.f(1);  // enable_dual_filter
        s.enableOrderHint = b.flag();
        if (s.enableOrderHint) {
            b.f(1);  // enable_jnt_comp
            s.enableRefFrameMvs = b.flag();
        }
        s.forceScreenContentTools = b.flag() ? 2 : int(b.f(1));
        if (s.forceScreenContentTools > 0) s.forceIntegerMv = b.flag() ? 2 : int(b.f(1));
        else s.forceIntegerMv = 2;
        if (s.enableOrderHint) s.orderHintBits = int(b.f(3)) + 1;
    }
    s.enableSuperres = b.flag();
    s.enableCdef = b.flag();
    s.enableRestoration = b.flag();
    // color_config
    const bool high = b.flag();
    if (s.profile == 2 && high) s.bitDepth = b.flag() ? 12 : 10;
    else s.bitDepth = high ? 10 : 8;
    s.mono = s.profile == 1 ? false : b.flag();
    int primaries = 2, transfer = 2;
    s.matrix = 2;
    if (b.flag()) primaries = int(b.f(8)), transfer = int(b.f(8)), s.matrix = int(b.f(8));
    if (s.mono) {
        s.fullRange = b.flag();
        s.subX = s.subY = true;
    } else if (primaries == 1 && transfer == 13 && s.matrix == 0) {
        s.fullRange = true;
        s.subX = s.subY = false;
    } else {
        s.fullRange = b.flag();
        if (s.profile == 0) s.subX = s.subY = true;
        else if (s.profile == 1) s.subX = s.subY = false;
        else if (s.bitDepth == 12) {
            s.subX = b.flag();
            s.subY = s.subX ? b.flag() : false;
        } else {
            s.subX = true, s.subY = false;
        }
        if (s.subX && s.subY) b.f(2);  // chroma_sample_position
    }
    if (!s.mono) s.separateUvDeltaQ = b.flag();
    s.filmGrainBit = b.at;
    s.filmGrainPresent = b.flag();
    if (b.bad) return no(error, "The AV1 sequence header is cut short");
    s.valid = true;
    seq_ = s;
    return true;
}

bool Av1HeaderReader::sequenceHeader(const uint8_t* p, size_t n, std::string* error) {
    // (the same again, as encoders send it before each key frame: nothing changes)
    if (seq_.valid && n == seqBytes_.size() && std::equal(p, p + n, seqBytes_.begin())) return true;
    // Another one begins a new coded video sequence: what the slots hold was made under the old one, so a shown key
    // frame must come next, and it cannot come between a frame's header and its tiles.
    if (seq_.valid && seenFrameHeader_) return no(error, "The AV1 sequence header changed inside a frame");
    const bool had = seq_.valid;
    if (!readSequence(p, n, error)) return false;
    seqBytes_.assign(p, p + n);
    if (had) keyFrameDue_ = true;
    return true;
}

int Av1HeaderReader::relativeDistance(int a, int b) const {
    if (!seq_.enableOrderHint || seq_.orderHintBits <= 0) return 0;
    const int m = 1 << (seq_.orderHintBits - 1);
    const int diff = a - b;
    return (diff & (m - 1)) - (diff & m);
}

bool Av1HeaderReader::readFrameHeader(const uint8_t* p, size_t n, const Obu& obu, Parsed& out, std::string* error) {
    const Av1Sequence& s = seq_;
    if (!s.valid) return no(error, "The AV1 stream has a frame before its sequence header");
    Bits b{p, n};
    out = Parsed{};
    Av1FrameHeader& h = out.h;
    h.temporalId = obu.temporalId, h.spatialId = obu.spatialId;
    constexpr int kAllFrames = 0xff, kNone = 7;
    bool intra = true, errorResilient = false;
    if (s.reducedStill) {
        h.frameType = kKeyFrame, h.showFrame = true;
    } else {
        h.showExisting = b.flag();
        if (h.showExisting) {
            h.showSlot = int(b.f(3));
            if (s.decoderModel && !s.equalPictureInterval) b.f(s.presentationBits);
            if (s.frameIds) b.f(s.frameIdBits);
            const Slot& shown = slots_[h.showSlot];
            if (b.bad) return no(error, "An AV1 frame header is cut short");
            if (!shown.valid) return no(error, "An AV1 frame shows a reference slot that holds no frame");
            if (keyFrameDue_) return no(error, "The AV1 sequence header changed without a key frame after it");
            h.frameType = shown.frameType;
            h.refreshFlags = h.frameType == kKeyFrame ? kAllFrames : 0;
            h.orderHint = shown.orderHint;
            h.width = shown.upscaledWidth, h.height = shown.frameHeight;
            if (s.filmGrainPresent) h.grain = shown.grain;
            h.grainBit = h.grainEnd = out.headerBits = b.at;
            pending_ = shown;
            return true;
        }
        h.frameType = int(b.f(2));
        intra = h.frameType == kIntraOnlyFrame || h.frameType == kKeyFrame;
        h.showFrame = b.flag();
        if (h.showFrame && s.decoderModel && !s.equalPictureInterval) b.f(s.presentationBits);
        h.showableFrame = h.showFrame ? h.frameType != kKeyFrame : b.flag();
        errorResilient = h.frameType == kSwitchFrame || (h.frameType == kKeyFrame && h.showFrame) ? true : b.flag();
    }
    if (keyFrameDue_ && !(h.frameType == kKeyFrame && h.showFrame)) return no(error, "The AV1 sequence header changed without a key frame after it");
    if (h.frameType == kKeyFrame && h.showFrame)
        for (Slot& slot : slots_) slot.valid = false, slot.orderHint = 0;
    const bool disableCdfUpdate = b.flag();
    const bool screenContent = s.forceScreenContentTools == 2 ? b.flag() : s.forceScreenContentTools != 0;
    bool integerMv = false;
    if (screenContent) integerMv = s.forceIntegerMv == 2 ? b.flag() : s.forceIntegerMv != 0;
    if (intra) integerMv = true;
    if (s.frameIds) b.f(s.frameIdBits);
    const bool sizeOverride = h.frameType == kSwitchFrame ? true : s.reducedStill ? false : b.flag();
    h.orderHint = int(b.f(s.orderHintBits));
    const int primaryRef = intra || errorResilient ? kNone : int(b.f(3));
    if (s.decoderModel && b.flag())
        for (int op = 0; op < s.operatingPoints; ++op)
            if (s.decoderModelForOp[op]) {
                const int idc = s.operatingPointIdc[op];
                if (idc == 0 || (((idc >> h.temporalId) & 1) && ((idc >> (h.spatialId + 8)) & 1))) b.f(s.bufferRemovalBits);
            }
    h.refreshFlags = h.frameType == kSwitchFrame || (h.frameType == kKeyFrame && h.showFrame) ? kAllFrames : int(b.f(8));
    if ((!intra || h.refreshFlags != kAllFrames) && errorResilient && s.enableOrderHint)
        for (Slot& slot : slots_) {
            const int hint = int(b.f(s.orderHintBits));
            // (a slot whose order hint is not the one given holds no frame: a decoder that lost it carries on without it)
            if (hint != slot.orderHint) slot = Slot{}, slot.orderHint = hint;
        }

    int frameWidth = 0, frameHeight = 0, upscaledWidth = 0, renderWidth = 0, renderHeight = 0;
    auto superres = [&] {
        int denominator = 8;
        if (s.enableSuperres && b.flag()) denominator = int(b.f(3)) + 9;
        upscaledWidth = frameWidth;
        frameWidth = (upscaledWidth * 8 + denominator / 2) / denominator;
    };
    auto frameSize = [&] {
        if (sizeOverride) frameWidth = int(b.f(s.widthBits)) + 1, frameHeight = int(b.f(s.heightBits)) + 1;
        else frameWidth = s.maxWidth, frameHeight = s.maxHeight;
        superres();
    };
    auto renderSize = [&] {
        if (b.flag()) renderWidth = int(b.f(16)) + 1, renderHeight = int(b.f(16)) + 1;
        else renderWidth = upscaledWidth, renderHeight = frameHeight;
    };
    int ref[7] = {};
    bool intrabc = false, highPrecisionMv = false;
    if (intra) {
        frameSize();
        renderSize();
        if (screenContent && upscaledWidth == frameWidth) intrabc = b.flag();
    } else {
        bool shortSignaling = false;
        if (s.enableOrderHint) {
            shortSignaling = b.flag();
            if (shortSignaling) {
                // The seven references from two of them and the slots' order hints (7.8).
                const int last = int(b.f(3)), gold = int(b.f(3));
                for (int& r : ref) r = -1;
                ref[0] = last, ref[3] = gold;
                bool used[8] = {};
                used[last] = used[gold] = true;
                const int cur = 1 << (s.orderHintBits - 1);
                int shifted[8];
                for (int i = 0; i < 8; ++i) shifted[i] = cur + relativeDistance(slots_[i].orderHint, h.orderHint);
                auto latestBackward = [&] {
                    int r = -1, best = 0;
                    for (int i = 0; i < 8; ++i)
                        if (!used[i] && shifted[i] >= cur && (r < 0 || shifted[i] >= best)) r = i, best = shifted[i];
                    return r;
                };
                auto earliestBackward = [&] {
                    int r = -1, best = 0;
                    for (int i = 0; i < 8; ++i)
                        if (!used[i] && shifted[i] >= cur && (r < 0 || shifted[i] < best)) r = i, best = shifted[i];
                    return r;
                };
                auto latestForward = [&] {
                    int r = -1, best = 0;
                    for (int i = 0; i < 8; ++i)
                        if (!used[i] && shifted[i] < cur && (r < 0 || shifted[i] >= best)) r = i, best = shifted[i];
                    return r;
                };
                if (const int r = latestBackward(); r >= 0) ref[6] = r, used[r] = true;    // ALTREF
                if (const int r = earliestBackward(); r >= 0) ref[4] = r, used[r] = true;  // BWDREF
                if (const int r = earliestBackward(); r >= 0) ref[5] = r, used[r] = true;  // ALTREF2
                for (const int k : {1, 2, 4, 5, 6})  // LAST2, LAST3, BWDREF, ALTREF2, ALTREF
                    if (ref[k] < 0)
                        if (const int r = latestForward(); r >= 0) ref[k] = r, used[r] = true;
                int earliest = -1, best = 0;
                for (int i = 0; i < 8; ++i)
                    if (earliest < 0 || shifted[i] < best) earliest = i, best = shifted[i];
                for (int& r : ref)
                    if (r < 0) r = earliest;
            }
        }
        for (int i = 0; i < 7; ++i) {
            if (!shortSignaling) ref[i] = int(b.f(3));
            if (s.frameIds) b.f(s.deltaFrameIdBits);
        }
        if (sizeOverride && !errorResilient) {
            bool found = false;
            for (int i = 0; i < 7 && !found; ++i) {
                found = b.flag();
                if (found) {
                    const Slot& r = slots_[ref[i]];
                    upscaledWidth = r.upscaledWidth, frameWidth = upscaledWidth, frameHeight = r.frameHeight;
                    renderWidth = r.renderWidth, renderHeight = r.renderHeight;
                }
            }
            if (found) {
                superres();
            } else {
                frameSize();
                renderSize();
            }
        } else {
            frameSize();
            renderSize();
        }
        highPrecisionMv = integerMv ? false : b.flag();
        if (!b.flag()) b.f(2);  // is_filter_switchable, interpolation_filter
        b.f(1);                 // is_motion_mode_switchable
        if (!errorResilient && s.enableRefFrameMvs) b.f(1);  // use_ref_frame_mvs
        for (int i = 0; i < 7; ++i) h.refSlot[i] = ref[i];
        // (every reference must hold a frame, as decoders require: one lost, or never made, is refused)
        for (const int r : ref)
            if (!slots_[r].valid) return no(error, "An AV1 frame refers to a reference slot that holds no frame");
    }
    if (!s.reducedStill && !disableCdfUpdate) b.f(1);  // disable_frame_end_update_cdf
    if (b.bad || frameWidth <= 0 || frameHeight <= 0) return no(error, "An AV1 frame header is cut short");

    // What the frame carries on from the frame it takes its probabilities from: the segments' features.
    Slot now;
    if (primaryRef != kNone) {
        const Slot& prev = slots_[ref[primaryRef]];
        std::memcpy(now.featureEnabled, prev.featureEnabled, sizeof now.featureEnabled);
        std::memcpy(now.featureData, prev.featureData, sizeof now.featureData);
    }

    // tile_info
    {
        const int miCols = 2 * ((frameWidth + 7) >> 3), miRows = 2 * ((frameHeight + 7) >> 3);
        const int sbCols = s.use128 ? (miCols + 31) >> 5 : (miCols + 15) >> 4, sbRows = s.use128 ? (miRows + 31) >> 5 : (miRows + 15) >> 4;
        const int sbSize = (s.use128 ? 5 : 4) + 2;
        const int maxTileWidthSb = 4096 >> sbSize;
        int maxTileAreaSb = (4096 * 2304) >> (2 * sbSize);
        const int minLog2Cols = tileLog2(maxTileWidthSb, sbCols), maxLog2Cols = tileLog2(1, std::min(sbCols, 64)),
                  maxLog2Rows = tileLog2(1, std::min(sbRows, 64));
        const int minLog2Tiles = std::max(minLog2Cols, tileLog2(maxTileAreaSb, sbRows * sbCols));
        if (b.flag()) {  // uniform_tile_spacing_flag
            out.tileColsLog2 = minLog2Cols;
            while (out.tileColsLog2 < maxLog2Cols && b.flag()) ++out.tileColsLog2;
            const int tileWidthSb = (sbCols + (1 << out.tileColsLog2) - 1) >> out.tileColsLog2;
            out.tileCols = tileWidthSb > 0 ? (sbCols + tileWidthSb - 1) / tileWidthSb : 1;
            out.tileRowsLog2 = std::max(minLog2Tiles - out.tileColsLog2, 0);
            while (out.tileRowsLog2 < maxLog2Rows && b.flag()) ++out.tileRowsLog2;
            const int tileHeightSb = (sbRows + (1 << out.tileRowsLog2) - 1) >> out.tileRowsLog2;
            out.tileRows = tileHeightSb > 0 ? (sbRows + tileHeightSb - 1) / tileHeightSb : 1;
        } else {
            int widest = 1, start = 0;
            out.tileCols = 0;
            for (; start < sbCols; ++out.tileCols) {
                const int size = int(b.ns(uint32_t(std::min(sbCols - start, maxTileWidthSb)))) + 1;
                widest = std::max(widest, size);
                start += size;
            }
            out.tileColsLog2 = tileLog2(1, out.tileCols);
            maxTileAreaSb = minLog2Tiles > 0 ? (sbRows * sbCols) >> (minLog2Tiles + 1) : sbRows * sbCols;
            const int maxTileHeightSb = std::max(maxTileAreaSb / widest, 1);
            start = 0;
            out.tileRows = 0;
            for (; start < sbRows; ++out.tileRows) start += int(b.ns(uint32_t(std::min(sbRows - start, maxTileHeightSb)))) + 1;
            out.tileRowsLog2 = tileLog2(1, out.tileRows);
        }
        if (out.tileColsLog2 > 0 || out.tileRowsLog2 > 0) b.f(out.tileRowsLog2 + out.tileColsLog2), b.f(2);
    }

    // quantization_params
    const int baseQ = int(b.f(8));
    auto deltaQ = [&] { return b.flag() ? b.su(7) : 0; };
    const int qYDc = deltaQ();
    int qUDc = 0, qUAc = 0, qVDc = 0, qVAc = 0;
    if (!s.mono) {
        const bool differ = s.separateUvDeltaQ ? b.flag() : false;
        qUDc = deltaQ(), qUAc = deltaQ();
        if (differ) qVDc = deltaQ(), qVAc = deltaQ();
        else qVDc = qUDc, qVAc = qUAc;
    }
    if (b.flag()) {  // using_qmatrix
        b.f(4), b.f(4);
        if (s.separateUvDeltaQ) b.f(4);
    }

    // segmentation_params
    const bool segmentation = b.flag();
    if (segmentation) {
        bool updateData = true;
        if (primaryRef != kNone) {
            if (b.flag()) b.f(1);  // segmentation_update_map, segmentation_temporal_update
            updateData = b.flag();
        }
        if (updateData) {
            static const int kBits[8] = {8, 6, 6, 6, 6, 3, 0, 0}, kMax[8] = {255, 63, 63, 63, 63, 7, 0, 0};
            static const bool kSigned[8] = {true, true, true, true, true, false, false, false};
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j) {
                    int v = 0;
                    const bool enabled = b.flag();
                    now.featureEnabled[i][j] = enabled;
                    if (enabled) v = kSigned[j] ? std::clamp(b.su(1 + kBits[j]), -kMax[j], kMax[j]) : std::clamp(int(b.f(kBits[j])), 0, kMax[j]);
                    now.featureData[i][j] = v;
                }
        }
    } else {
        std::memset(now.featureEnabled, 0, sizeof now.featureEnabled);
        std::memset(now.featureData, 0, sizeof now.featureData);
    }

    // delta_q_params, delta_lf_params
    const bool deltaQPresent = baseQ > 0 ? b.flag() : false;
    if (deltaQPresent) {
        b.f(2);
        if (!intrabc && b.flag()) b.f(2), b.f(1);
    }

    bool codedLossless = true;
    for (int seg = 0; seg < 8; ++seg) {
        int q = baseQ;
        if (segmentation && now.featureEnabled[seg][0]) q = std::clamp(baseQ + now.featureData[seg][0], 0, 255);
        if (!(q == 0 && qYDc == 0 && qUAc == 0 && qUDc == 0 && qVAc == 0 && qVDc == 0)) codedLossless = false;
    }
    const bool allLossless = codedLossless && frameWidth == upscaledWidth;

    // loop_filter_params
    if (!codedLossless && !intrabc) {
        const uint32_t l0 = b.f(6), l1 = b.f(6);
        if (!s.mono && (l0 || l1)) b.f(6), b.f(6);
        b.f(3);
        if (b.flag() && b.flag()) {  // loop_filter_delta_enabled, loop_filter_delta_update
            for (int i = 0; i < 8; ++i)
                if (b.flag()) b.su(7);
            for (int i = 0; i < 2; ++i)
                if (b.flag()) b.su(7);
        }
    }
    // cdef_params
    if (!codedLossless && !intrabc && s.enableCdef) {
        b.f(2);
        const int count = 1 << b.f(2);
        for (int i = 0; i < count; ++i) {
            b.f(4), b.f(2);
            if (!s.mono) b.f(4), b.f(2);
        }
    }
    // lr_params
    if (!allLossless && !intrabc && s.enableRestoration) {
        bool uses = false, usesChroma = false;
        for (int i = 0; i < (s.mono ? 1 : 3); ++i)
            if (b.f(2) != 0) {
                uses = true;
                if (i > 0) usesChroma = true;
            }
        if (uses) {
            if (s.use128) b.f(1);
            else if (b.flag()) b.f(1);
            if (s.subX && s.subY && usesChroma) b.f(1);
        }
    }
    if (!codedLossless) b.f(1);  // tx_mode_select
    const bool referenceSelect = intra ? false : b.flag();
    // skip_mode_params
    bool skipMode = false;
    if (!intra && referenceSelect && s.enableOrderHint) {
        int forward = -1, backward = -1, forwardHint = 0, backwardHint = 0;
        for (int i = 0; i < 7; ++i) {
            const int hint = slots_[ref[i]].orderHint;
            if (relativeDistance(hint, h.orderHint) < 0) {
                if (forward < 0 || relativeDistance(hint, forwardHint) > 0) forward = i, forwardHint = hint;
            } else if (relativeDistance(hint, h.orderHint) > 0) {
                if (backward < 0 || relativeDistance(hint, backwardHint) < 0) backward = i, backwardHint = hint;
            }
        }
        if (forward < 0) {
            skipMode = false;
        } else if (backward >= 0) {
            skipMode = true;
        } else {
            int second = -1, secondHint = 0;
            for (int i = 0; i < 7; ++i) {
                const int hint = slots_[ref[i]].orderHint;
                if (relativeDistance(hint, forwardHint) < 0 && (second < 0 || relativeDistance(hint, secondHint) > 0)) second = i, secondHint = hint;
            }
            skipMode = second >= 0;
        }
    }
    if (skipMode) b.f(1);                                             // skip_mode_present
    if (!intra && !errorResilient && s.enableWarpedMotion) b.f(1);    // allow_warped_motion
    b.f(1);                                                           // reduced_tx_set
    // global_motion_params: each parameter's bits do not depend on the reference's parameters, only its kind.
    if (!intra) {
        auto param = [&](int type, int idx) {
            int absBits = 12;
            if (idx < 2) absBits = type == 1 ? 9 - (highPrecisionMv ? 0 : 1) : 12;
            const uint32_t symbols = (2u << absBits) + 1;
            int i = 0;
            uint32_t mk = 0;
            for (;;) {
                const int b2 = i ? 3 + i - 1 : 3;
                const uint32_t a = 1u << b2;
                if (symbols <= mk + 3 * a) {
                    b.ns(symbols - mk);
                    return;
                }
                if (b.flag()) {
                    ++i, mk += a;
                } else {
                    b.f(b2);
                    return;
                }
                if (b.bad) return;
            }
        };
        for (int r = 1; r <= 7; ++r) {
            int type = 0;  // identity; 1 translation, 2 rotation and zoom, 3 affine
            if (b.flag()) type = b.flag() ? 2 : b.flag() ? 1 : 3;
            if (type >= 2) {
                param(type, 2), param(type, 3);
                if (type == 3) param(type, 4), param(type, 5);
            }
            if (type >= 1) param(type, 0), param(type, 1);
        }
    }

    // film_grain_params
    h.grainBit = b.at;
    if (s.filmGrainPresent && (h.showFrame || h.showableFrame)) {
        h.grainCoded = true;
        Av1FilmGrain g;
        g.apply = b.flag();
        if (g.apply) {
            g.seed = uint16_t(b.f(16));
            h.grainUpdate = h.frameType == kInterFrame ? b.flag() : true;
            if (!h.grainUpdate) {
                h.grainRefSlot = int(b.f(3));
                // (the parameters of a frame this one refers to: one of its own references)
                if (std::find(std::begin(h.refSlot), std::end(h.refSlot), h.grainRefSlot) == std::end(h.refSlot))
                    return no(error, "An AV1 frame's film grain is taken from a frame it does not refer to");
                const uint16_t seed = g.seed;
                g = slots_[h.grainRefSlot].grain;
                g.apply = true, g.seed = seed;
            } else {
                g.numY = int(b.f(4));
                if (g.numY > 14) return no(error, "An AV1 frame's film grain has too many luma points");
                for (int i = 0; i < g.numY; ++i) g.yValue[i] = uint8_t(b.f(8)), g.yScaling[i] = uint8_t(b.f(8));
                g.chromaFromLuma = s.mono ? false : b.flag();
                if (!(s.mono || g.chromaFromLuma || (s.subX && s.subY && g.numY == 0))) {
                    g.numCb = int(b.f(4));
                    if (g.numCb > 10) return no(error, "An AV1 frame's film grain has too many chroma points");
                    for (int i = 0; i < g.numCb; ++i) g.cbValue[i] = uint8_t(b.f(8)), g.cbScaling[i] = uint8_t(b.f(8));
                    g.numCr = int(b.f(4));
                    if (g.numCr > 10) return no(error, "An AV1 frame's film grain has too many chroma points");
                    for (int i = 0; i < g.numCr; ++i) g.crValue[i] = uint8_t(b.f(8)), g.crScaling[i] = uint8_t(b.f(8));
                    if (s.subX && s.subY && (g.numCb > 0) != (g.numCr > 0))
                        return no(error, "An AV1 frame's 4:2:0 film grain has points for one chroma plane and not the other");
                }
                g.scalingShift = int(b.f(2)) + 8;
                g.arLag = int(b.f(2));
                const int nLuma = g.lumaCoefficients(), nChroma = g.numY > 0 ? nLuma + 1 : nLuma;
                if (g.numY > 0)
                    for (int i = 0; i < nLuma; ++i) g.arY[i] = int8_t(int(b.f(8)) - 128);
                if (g.chromaFromLuma || g.numCb > 0)
                    for (int i = 0; i < nChroma; ++i) g.arCb[i] = int8_t(int(b.f(8)) - 128);
                if (g.chromaFromLuma || g.numCr > 0)
                    for (int i = 0; i < nChroma; ++i) g.arCr[i] = int8_t(int(b.f(8)) - 128);
                g.arShift = int(b.f(2)) + 6;
                g.grainScaleShift = int(b.f(2));
                if (g.numCb > 0) g.cbMult = int(b.f(8)), g.cbLumaMult = int(b.f(8)), g.cbOffset = int(b.f(9));
                if (g.numCr > 0) g.crMult = int(b.f(8)), g.crLumaMult = int(b.f(8)), g.crOffset = int(b.f(9));
                g.overlap = b.flag();
                g.clipRestricted = b.flag();
            }
            h.grain = g;
        }
    }
    h.grainEnd = out.headerBits = b.at;
    if (b.bad) return no(error, "An AV1 frame header is cut short");
    h.width = upscaledWidth, h.height = frameHeight;

    now.valid = true;
    now.frameType = h.frameType;
    now.upscaledWidth = upscaledWidth, now.frameWidth = frameWidth, now.frameHeight = frameHeight;
    now.renderWidth = renderWidth, now.renderHeight = renderHeight;
    now.orderHint = h.orderHint;
    now.showable = h.showableFrame;
    now.grain = h.grain;
    pending_ = now;
    keyFrameDue_ = false;
    return true;
}

void Av1HeaderReader::finishFrame(const Parsed& f) {
    for (int i = 0; i < 8; ++i)
        if (f.h.refreshFlags & (1 << i)) slots_[i] = pending_;
    seenFrameHeader_ = false;
}

bool Av1HeaderReader::configure(const uint8_t* data, size_t size, std::string* error) {
    if (!data || size == 0) return no(error, "The AV1 stream has no codec configuration");
    const size_t from = configObus(data, size);
    std::vector<Obu> obus;
    if (!splitObus(data + from, size - from, obus, error)) return false;
    for (const Obu& o : obus)
        if (o.type == kObuSequence) return sequenceHeader(data + from + o.payload, o.size, error);
    // (an av1C box may hold no OBUs: the sequence header then comes with the first packet)
    if (from > 0 && obus.empty()) return true;
    return no(error, "The AV1 codec configuration has no sequence header");
}

namespace {

// Whether the tile group at `p` is its frame's last.
bool lastTileGroup(const uint8_t* p, size_t n, int tileCols, int tileRows, int tileBits) {
    const int tiles = tileCols * tileRows;
    if (tiles <= 1) return true;
    Bits b{p, n};
    if (!b.flag()) return true;  // tile_start_and_end_present_flag
    b.f(tileBits);
    return int(b.f(tileBits)) == tiles - 1;
}

}  // namespace

bool Av1HeaderReader::read(const uint8_t* data, size_t size, std::vector<Av1FrameHeader>& frames, std::string* error) {
    frames.clear();
    std::vector<Obu> obus;
    if (!splitObus(data, size, obus, error)) return false;
    for (const Obu& o : obus) {
        const uint8_t* p = data + o.payload;
        if (o.type == kObuDelimiter) {
            seenFrameHeader_ = false;
        } else if (o.type == kObuSequence) {
            if (!sequenceHeader(p, o.size, error)) return false;
        } else if (o.type == kObuFrameHeader || o.type == kObuRedundantHeader || o.type == kObuFrame) {
            if (!seenFrameHeader_) {
                if (!readFrameHeader(p, o.size, o, current_, error)) return false;
                frames.push_back(current_.h);
                seenFrameHeader_ = true;
                if (current_.h.showExisting) finishFrame(current_);
            }
            if (seenFrameHeader_ && o.type == kObuFrame) {
                const size_t at = (current_.headerBits + 7) / 8;
                if (at > o.size) return no(error, "An AV1 frame is cut short");
                if (lastTileGroup(p + at, o.size - at, current_.tileCols, current_.tileRows, current_.tileColsLog2 + current_.tileRowsLog2)) finishFrame(current_);
            }
        } else if (o.type == kObuTileGroup && seenFrameHeader_) {
            if (lastTileGroup(p, o.size, current_.tileCols, current_.tileRows, current_.tileColsLog2 + current_.tileRowsLog2)) finishFrame(current_);
        }
    }
    return true;
}

void av1WriteFilmGrain(const Av1FilmGrain& in, const Av1Sequence& seq, bool interFrame, bool update, int refSlot, std::vector<bool>& bits) {
    const Av1FilmGrain g = grainFor(in, seq);
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bits.push_back((v >> i) & 1u);
    };
    put(g.apply ? 1 : 0, 1);
    if (!g.apply) return;
    put(g.seed, 16);
    if (interFrame) put(update ? 1 : 0, 1);
    else update = true;
    if (!update) {
        put(uint32_t(refSlot & 7), 3);
        return;
    }
    put(uint32_t(g.numY), 4);
    for (int i = 0; i < g.numY; ++i) put(g.yValue[i], 8), put(g.yScaling[i], 8);
    if (!seq.mono) put(g.chromaFromLuma ? 1 : 0, 1);
    if (!(seq.mono || g.chromaFromLuma || (seq.subX && seq.subY && g.numY == 0))) {
        put(uint32_t(g.numCb), 4);
        for (int i = 0; i < g.numCb; ++i) put(g.cbValue[i], 8), put(g.cbScaling[i], 8);
        put(uint32_t(g.numCr), 4);
        for (int i = 0; i < g.numCr; ++i) put(g.crValue[i], 8), put(g.crScaling[i], 8);
    }
    put(uint32_t(g.scalingShift - 8), 2);
    put(uint32_t(g.arLag), 2);
    const int nLuma = g.lumaCoefficients(), nChroma = g.numY > 0 ? nLuma + 1 : nLuma;
    if (g.numY > 0)
        for (int i = 0; i < nLuma; ++i) put(uint32_t(int(g.arY[i]) + 128), 8);
    if (g.chromaFromLuma || g.numCb > 0)
        for (int i = 0; i < nChroma; ++i) put(uint32_t(int(g.arCb[i]) + 128), 8);
    if (g.chromaFromLuma || g.numCr > 0)
        for (int i = 0; i < nChroma; ++i) put(uint32_t(int(g.arCr[i]) + 128), 8);
    put(uint32_t(g.arShift - 6), 2);
    put(uint32_t(g.grainScaleShift), 2);
    if (g.numCb > 0) put(uint32_t(g.cbMult), 8), put(uint32_t(g.cbLumaMult), 8), put(uint32_t(g.cbOffset), 9);
    if (g.numCr > 0) put(uint32_t(g.crMult), 8), put(uint32_t(g.crLumaMult), 8), put(uint32_t(g.crOffset), 9);
    put(g.overlap ? 1 : 0, 1);
    put(g.clipRestricted ? 1 : 0, 1);
}

bool Av1GrainWriter::configure(std::vector<uint8_t>& config, std::string* error) {
    if (config.empty()) return true;  // (none: the sequence header comes with the first packet)
    if (!reader_.configure(config.data(), config.size(), error)) return false;
    const size_t from = configObus(config.data(), config.size());
    std::vector<Av1HeaderReader::Obu> obus;
    if (!Av1HeaderReader::splitObus(config.data() + from, config.size() - from, obus, error)) return false;
    for (const auto& o : obus)
        if (o.type == kObuSequence) {
            const size_t bit = reader_.seq_.filmGrainBit;
            config[from + o.payload + bit / 8] |= uint8_t(0x80 >> (bit % 8));
        }
    return true;
}

bool Av1GrainWriter::rewrite(const uint8_t* data, size_t size, int64_t frame, const Source& grain, std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    std::vector<Av1HeaderReader::Obu> obus;
    if (!Av1HeaderReader::splitObus(data, size, obus, error)) return false;
    // The order hint of the frame this unit shows: frames it only keeps are shown that many frames later.
    int shownHint = -1;
    {
        Av1HeaderReader probe = reader_;
        std::vector<Av1FrameHeader> frames;
        if (!probe.read(data, size, frames, error)) return false;
        for (const Av1FrameHeader& h : frames)
            if (h.shown()) shownHint = h.orderHint;
    }
    Av1HeaderReader& r = reader_;
    auto emit = [&](const Av1HeaderReader::Obu& o, const uint8_t* payload, size_t n) {
        out.insert(out.end(), data + o.start, data + o.start + 1 + (o.extension ? 1 : 0));
        if (o.hasSize) writeLeb128(out, n);
        out.insert(out.end(), payload, payload + n);
    };
    // A header's bits and then what its OBU holds after them: the tile data from the next whole byte, or trailing bits.
    auto withHeader = [&](const Av1HeaderReader::Obu& o, const uint8_t* p, const BitWriter& header, size_t originalBits) {
        BitWriter w = header;
        if (o.type == kObuFrame) {
            w.align();
            const size_t from = (originalBits + 7) / 8;
            w.bytes.insert(w.bytes.end(), p + std::min(from, o.size), p + o.size);
        } else {
            w.put(1, 1);
            w.align();
        }
        emit(o, w.bytes.data(), w.bytes.size());
    };
    for (const auto& o : obus) {
        const uint8_t* p = data + o.payload;
        if (o.type == kObuDelimiter) {
            r.seenFrameHeader_ = false;
            emit(o, p, o.size);
        } else if (o.type == kObuSequence) {
            if (!r.sequenceHeader(p, o.size, error)) return false;
            std::vector<uint8_t> copy(p, p + o.size);
            const size_t bit = r.seq_.filmGrainBit;
            copy[bit / 8] |= uint8_t(0x80 >> (bit % 8));
            emit(o, copy.data(), copy.size());
        } else if (o.type == kObuFrameHeader || o.type == kObuRedundantHeader || o.type == kObuFrame) {
            if (r.seenFrameHeader_) {
                // (a copy of the frame's header: the same bits again)
                BitWriter w;
                w.copy(lastHeader_.data(), 0, lastHeaderBits_);
                withHeader(o, p, w, lastOriginalBits_);
            } else {
                Av1HeaderReader::Parsed& f = r.current_;
                if (!r.readFrameHeader(p, o.size, o, f, error)) return false;
                r.seenFrameHeader_ = true;
                const Av1FrameHeader& h = f.h;
                if (h.showExisting) {
                    if (h.frameType == kKeyFrame)
                        for (int i = 0; i < 8; ++i)
                            if (i != h.showSlot) written_[i] = written_[h.showSlot];
                    r.finishFrame(f);
                    emit(o, p, o.size);
                    continue;
                }
                BitWriter w;
                w.copy(p, 0, h.grainBit);
                Av1FilmGrain given;  // (none: what a frame that cannot be shown leaves in its slots)
                if (h.showFrame || h.showableFrame) {
                    int64_t at = frame;
                    if (!h.showFrame && shownHint >= 0) at += r.relativeDistance(h.orderHint, shownHint);
                    given = grainFor(grain ? grain(at) : Av1FilmGrain{}, r.seq_);
                    std::string why;
                    if (given.apply && !given.valid(&why, r.seq_.subX && r.seq_.subY)) return no(error, "The film grain parameters cannot be written");
                    const bool inter = h.frameType == kInterFrame;
                    int from = -1;
                    if (given.apply && inter)
                        for (int i = 0; i < 7 && from < 0; ++i)
                            if (h.refSlot[i] >= 0 && written_[h.refSlot[i]].apply && written_[h.refSlot[i]].sameParameters(given)) from = h.refSlot[i];
                    std::vector<bool> bits;
                    av1WriteFilmGrain(given, r.seq_, inter, from < 0, from, bits);
                    for (const bool bit : bits) w.put(bit ? 1 : 0, 1);
                    if (given.apply) {
                        ++framesWithGrain;
                        if (from >= 0) ++framesByReference;
                    } else {
                        given = Av1FilmGrain{};
                    }
                }
                for (int i = 0; i < 8; ++i)
                    if (h.refreshFlags & (1 << i)) written_[i] = given;
                lastHeader_ = w.bytes, lastHeaderBits_ = w.bits, lastOriginalBits_ = h.grainEnd;
                withHeader(o, p, w, h.grainEnd);
            }
            if (r.seenFrameHeader_ && o.type == kObuFrame) {
                const size_t at = (r.current_.headerBits + 7) / 8;
                if (at > o.size) return no(error, "An AV1 frame is cut short");
                if (lastTileGroup(p + at, o.size - at, r.current_.tileCols, r.current_.tileRows, r.current_.tileColsLog2 + r.current_.tileRowsLog2))
                    r.finishFrame(r.current_);
            }
        } else {
            if (o.type == kObuTileGroup && r.seenFrameHeader_ &&
                lastTileGroup(p, o.size, r.current_.tileCols, r.current_.tileRows, r.current_.tileColsLog2 + r.current_.tileRowsLog2))
                r.finishFrame(r.current_);
            emit(o, p, o.size);
        }
    }
    bytesAdded += int64_t(out.size()) - int64_t(size);
    return true;
}

}  // namespace montage
