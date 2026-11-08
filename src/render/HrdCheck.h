// Montage — the buffer models broadcast ingest checks a long-GOP file against, and the tools that keep them whole when
// a stream is joined from separate encodes.
//  - H.264 and HEVC (ITU-T H.264 and H.265 Annex C, the hypothetical reference decoder): the HRD parameters in the
//    sequence parameter set's VUI (NAL HRD, else VCL HRD; HEVC's in the VPS when the SPS has none), the buffering
//    period SEI that starts each period and the picture timing SEI of every access unit. Each access unit's bits
//    arrive at the stated bit rate (CBR: one after another; VBR: no earlier than the initial delay and offset allow),
//    and are removed at the time its picture timing says. Underflow (a picture not all there when it is to be decoded)
//    and overflow (more bits than the buffer holds) are reported by frame, and so is a buffering period whose initial
//    delay does not agree with where the buffer stands (C.3 of H.264, C.4 of H.265).
//  - A stream with no HRD stated is checked against its level's limits (H.264 Table A-1, H.265 Table A.8): VBR at the
//    level's highest bit rate into its largest buffer, the buffer full before the first picture, the pictures removed
//    at the container's times.
//  - MPEG-2 (ISO/IEC 13818-2 Annex C, the video buffering verifier): the sequence header's bit rate and buffer size,
//    every picture's vbv_delay. At a constant rate (vbv_delay given) the delays set when each picture's start code
//    arrives; the rate between pictures they imply must not pass the stated rate, and the buffer must neither run dry
//    nor overflow. At a variable rate (vbv_delay 0xFFFF) the buffer fills at the stated rate until full and must not
//    run dry.
// Sizes are counted as a byte stream: each NAL unit of an MP4, MOV or Matroska file with a four-byte start code (as
// x264 and x265 count them), Annex B files (MPEG-TS, raw streams) as they are. Sub-picture HRD parameters are not modelled.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace montage {

struct HrdIssue {
    int64_t frame = 0;  // the access unit, in decoding order (0 = the first)
    int64_t shown = 0;  // the frame it shows, in display order (counted from the file's first)
    std::string kind;   // "underflow", "overflow", "initial delay", "rate", "timing"
    std::string text;
};

struct HrdReport {
    bool checked = false;  // a buffer model was found and every picture run through it
    std::string codec;     // "H.264", "HEVC", "MPEG-2"
    std::string model;     // "NAL HRD", "VCL HRD", "VBV", "level 4.1 limits"
    bool cbr = false;
    bool signalled = false;   // stated in the stream (not the level's limits)
    double bitRate = 0;       // bits/s
    double bufferBits = 0;
    double initialDelay = 0;  // seconds the first picture waits in the buffer
    int64_t frames = 0;
    double lowest = 1, highest = 0;  // the buffer's fullness, as a fraction of it: lowest just after a removal, highest just before
    std::vector<HrdIssue> issues;    // the first 100
    int64_t issueCount = 0;
    std::string note;   // what was assumed, or why nothing was checked
    std::string error;  // the file could not be read
    bool conforms() const { return checked && issueCount == 0; }
    // One line: the model and what it found ("H.264 NAL HRD, CBR 4 Mb/s, 1.00 s buffer: conforms (60 frames)").
    std::string summary() const;
};

// Checks the first video stream of `path` (H.264, HEVC or MPEG-2 in any container FFmpeg reads).
HrdReport checkHrd(const std::string& path, const std::atomic<bool>* cancel = nullptr);

namespace hrd {

// One CPB's schedule.
struct Cpb {
    double bitRate = 0;   // bits/s
    double size = 0;      // bits
    bool cbr = false;
};

// An HRD parameter set (H.264 hrd_parameters(), H.265's for the highest sub-layer).
struct Params {
    bool present = false;
    std::vector<Cpb> cpbs;
    int initialDelayLength = 24, cpbDelayLength = 24, dpbDelayLength = 24, timeOffsetLength = 24;
    bool lowDelay = false;
    bool subPic = false;  // HEVC: sub-picture parameters (not modelled)
};

struct Sps {
    bool valid = false;
    int codec = 0;  // AVCodecID
    int id = -1, vpsId = -1;
    int profile = 0, level = 0;
    bool highTier = false;
    int maxSubLayersMinus1 = 0;
    bool vui = false, timing = false;
    uint32_t numUnitsInTick = 0, timeScale = 0;
    bool fixedFrameRate = false;
    Params nal, vcl;
    bool picStruct = false;  // H.264 pic_struct_present_flag; HEVC frame_field_info_present_flag
    // Where the HRD sits in the RBSP (bits from the start of the NAL unit, its header included): H.264 from
    // nal_hrd_parameters_present_flag to after low_delay_hrd_flag; HEVC from vui_hrd_parameters_present_flag to the
    // end of hrd_parameters(). Both 0 without timing information (no HRD can go there).
    size_t hrdFrom = 0, hrdTo = 0;
    // H.264 slice header fields (render/LongGopCopy's recovery-point entry).
    int chromaFormat = 1;
    bool separateColourPlane = false;
    int log2MaxFrameNum = 4, pocType = 0, log2MaxPocLsb = 4;
    bool deltaPicOrderAlwaysZero = false, frameMbsOnly = true;
    int maxNumRefFrames = 0;
    bool gapsAllowed = false;
    const Params* active() const { return nal.present ? &nal : vcl.present ? &vcl : nullptr; }
    double tick() const { return timing && timeScale ? double(numUnitsInTick) / double(timeScale) : 0; }
};

struct Pps {
    bool valid = false;
    int id = -1, spsId = -1;
    // H.264
    bool cabac = false, bottomFieldPicOrder = false, weightedPred = false, deblockingControl = false, redundantPicCnt = false;
    int weightedBipred = 0, numRefIdxL0 = 1, numRefIdxL1 = 1, numSliceGroups = 1;
    // HEVC: what comes between the slice's PPS id and its type
    bool dependentSliceSegments = false, outputFlagPresent = false;
    int numExtraSliceHeaderBits = 0;
};

Sps parseH264Sps(const uint8_t* nal, size_t n);
Pps parseH264Pps(const uint8_t* nal, size_t n, const std::map<int, Sps>& sps);
Sps parseHevcSps(const uint8_t* nal, size_t n);
Pps parseHevcPps(const uint8_t* nal, size_t n);
// HEVC: a VPS's HRD for its highest sub-layer (the first hrd_parameters() it holds), in `into` (nal, vcl).
bool parseHevcVpsHrd(const uint8_t* nal, size_t n, Sps& into);

// The SPS NAL unit `target` with the HRD (and, for H.264, low_delay_hrd_flag) of `from` in place of its own: both
// must have timing information (and HEVC the same number of sub-layers). Empty when that cannot be done.
std::vector<uint8_t> spliceHrd(int codec, const uint8_t* target, size_t targetSize, const uint8_t* from, size_t fromSize);

// A buffering period's initial delays (90 kHz) for each CPB, NAL then VCL.
struct BufferingPeriod {
    bool present = false;
    int spsId = -1;
    std::vector<int64_t> nalDelay, nalOffset, vclDelay, vclOffset;
    // HEVC
    bool irapCpbParams = false, concatenation = false;
    int64_t cpbDelayOffset = 0, dpbDelayOffset = 0, auCpbRemovalDelayDeltaMinus1 = 0;
    std::vector<int64_t> nalAltDelay, nalAltOffset, vclAltDelay, vclAltOffset;
};

struct PictureTiming {
    bool present = false;
    int64_t cpbRemovalDelay = 0;  // H.264 cpb_removal_delay; HEVC au_cpb_removal_delay_minus1 + 1
    int64_t dpbOutputDelay = 0;
    int picStruct = -1;
};

// What an access unit (its NAL units length-prefixed by `lengthSize` bytes, or Annex B with lengthSize 0) says about
// the buffer, with the parameter sets in it remembered in `sps` and `pps`.
struct AccessUnit {
    BufferingPeriod bp;
    PictureTiming pt;
    int spsId = -1;     // the active SPS, from the first slice's PPS
    bool irap = false;  // H.264 IDR; HEVC IRAP
    bool idr = false;
    int64_t bits = 0;   // counted as a byte stream
    int vclBits = 0;
};
bool readAccessUnit(int codec, const uint8_t* d, size_t n, int lengthSize, std::map<int, Sps>& sps, std::map<int, Pps>& pps,
                    AccessUnit& au, std::map<int, Sps>* vpsHrd = nullptr);

// The parameter sets (whole NAL units) of a stream's decoder configuration: avcC, hvcC, or Annex B; `lengthSize`
// gets its NAL units' length size (0: Annex B).
std::vector<std::vector<uint8_t>> parameterSets(int codec, const uint8_t* extradata, size_t n, int& lengthSize);
// MPEG-2: the bit rate (bits/s) and VBV buffer (bits) of the first sequence header (and its extension) in `d`.
bool mpeg2Rate(const uint8_t* d, size_t n, double& bitRate, double& bufferBits);

}  // namespace hrd

// Keeps one stream's buffer model whole while access units from separate encodes are written one after another (smart
// rendering's copied GOPs and encoded parts, a farm's segments): every access unit's picture timing rewritten for its
// place (removals one frame apart in decoding order, output when its timestamps say), each buffering period's initial
// delay set to where the buffer stands, and, at a constant rate, filler data (H.264 and HEVC filler NAL units, MPEG-2
// zero bytes before the next start code) added to the access unit before a join so the buffer stands where the next
// part expects it, and where it would otherwise overflow. Parts encoded here have no SEI of their own: they get
// buffering periods at every IRAP picture and picture timing on every access unit.
class HrdSplicer {
public:
    // H.264 and HEVC: the output's parameter sets (each NAL unit whole, without length or start code) and the length
    // size of its access units; the model comes from the first SPS with an HRD. MPEG-2: `mpeg2Rate` and
    // `mpeg2Buffer` (bits/s and bits, a constant rate). `depth`: the frames the output decodes ahead of what it shows.
    // False when there is no HRD to keep.
    bool start(int codec, const std::vector<std::vector<uint8_t>>& sets, int lengthSize, double fps, int depth, double mpeg2Rate = 0,
               double mpeg2Buffer = 0);
    bool active() const { return active_; }
    bool cbr() const { return cbr_; }
    double bitRate() const { return rate_; }
    double bufferBits() const { return size_; }
    int64_t written() const { return count_; }
    // A parameter set the output carries out of band (H.264's avcC) or will see in band.
    void addParameterSet(const uint8_t* nal, size_t n);
    // Seconds the next access unit's removal comes after the one held (or the first's initial delay) finished arriving,
    // were nothing padded: where the buffer stands for what comes next (times the bit rate, its bits).
    double nextDelay() const;
    // The next access unit's buffering period, when it brings its own (H.264 and HEVC: the first CPB's initial delay
    // in seconds; MPEG-2: its first picture's vbv_delay in seconds), so the one held is padded to meet it.
    // An access unit in decoding order, showing frame `display`; `ours`: encoded here (it gets its SEI, or its
    // vbv_delay set) and may be padded. Returns the access unit before it, final (empty for the first); `prevDisplay`
    // gets what it shows.
    std::vector<uint8_t> push(std::vector<uint8_t> au, int64_t display, bool ours, bool mayPad, int64_t* prevDisplay);
    // The access unit held, final.
    std::vector<uint8_t> finish(int64_t* prevDisplay);
    bool holding() const { return havePending_; }
    // The initial delay (seconds) the first access unit gets when it is ours (default: 90 % of the buffer).
    void setFirstDelay(double seconds) { firstDelay_ = seconds; }
    // The initial delay (seconds) an access unit (an IRAP picture copied from footage) asks for, from its own SEI or
    // vbv_delay: -1 when it says none.
    double askedDelay(const uint8_t* d, size_t n) const;
    const std::vector<HrdIssue>& issues() const { return issues_; }
    int64_t fillerBytes() const { return filler_; }

private:
    struct Held {
        std::vector<uint8_t> au;
        int64_t display = 0;
        bool ours = false, mayPad = false;
        double removal = 0;  // its nominal removal time
        double arrival = 0;  // when its first bit arrives (before padding)
    };
    void finalize(Held& h, double nextRemoval, double nextAsked);
    bool rewrite(Held& h, int64_t k);
    void pad(Held& h, int64_t bytes);
    int64_t bitsOf(const std::vector<uint8_t>& au) const;
    void note(int64_t frame, int64_t shown, const char* kind, const std::string& text);

    bool active_ = false;
    int codec_ = 0;
    int lengthSize_ = 4;
    double fps_ = 25, frame_ = 0.04;
    int depth_ = 0;
    bool cbr_ = false;
    double rate_ = 0, size_ = 0;
    int64_t sum90_ = 0;        // a new buffering period's initial delay and offset together
    std::map<int, hrd::Sps> sps_;
    std::map<int, hrd::Pps> pps_;
    std::vector<uint8_t> hrdSps_;  // the SPS whose HRD parts are given
    double firstDelay_ = -1;
    // The model.
    int64_t count_ = 0;        // access units final
    double t0_ = 0;            // the first access unit's removal
    double lastFinal_ = 0;     // when the last final one finished arriving
    double bpRemoval_ = 0;     // the latest buffering period's first access unit's removal
    int64_t bpIndex_ = 0;
    int64_t init90_ = 0, offset90_ = 0;
    int64_t dpbFrames_ = -1;   // output delay, in frames, after the removal less the display order (learned)
    double mpegBits_ = 0;      // MPEG-2: bits written
    Held pending_;
    bool havePending_ = false;
    std::vector<HrdIssue> issues_;
    int64_t filler_ = 0;
};

}  // namespace montage
