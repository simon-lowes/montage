// Montage — smart rendering of long-GOP exports (H.264, HEVC, MPEG-2 as XDCAM HD422 has it, AV1, VP9): where the export is coded
// as its footage is (codec, size, sampling, bit depth, rate, field order), untouched pictures are copied from the source
// a GOP at a time, and only the GOPs around edits, effects, transitions and titles are encoded again, closed, by x264,
// x265 or FFmpeg's MPEG-2 encoder set to the source's profile and level (and, for H.264 and HEVC, a bit rate taken from
// the GOPs copied either side).
//  - H.264: a GOP runs from one IDR picture to the next. The encoder's parameter sets take an id the source's do not
//    use, so both sit in the file's one decoder configuration (avcC) and every copied packet is the source's, byte for
//    byte. Sources whose parameter sets clash (another picture under the same id) are not copied.
//  - HEVC: from one IRAP picture (IDR, CRA, BLA) to the next, parameter sets in the video (hev1): the source's go in
//    front of every copied GOP, so each decodes after a seek. A copied GOP that starts on a CRA after pictures not its
//    own leaves out its RASL pictures (they refer to a GOP not copied: their frames are encoded with the part before),
//    and its CRA is marked a broken link (BLA, the NAL unit type its splices are given), so the decoder starts afresh.
//  - MPEG-2: from one sequence and GOP header to the next. A closed GOP is copied as it is. An open one's leading
//    B-pictures refer to the picture before it, so it is copied only where that frame is shown just before it, encoded
//    (an encoded part always ends on an anchor picture): those B-pictures then decode from the encoded picture, near the
//    source's but not exactly it.
//  - AV1 and VP9: from one shown key frame to the next (AV1's with its sequence header). VP9's key frames set every
//    reference and every probability context anew and carry their own profile, depth and size, so its GOPs need
//    nothing in the file's description; libvpx's hidden frames ride inside the packet of the frame shown after them.
// Footage shown turned (a display matrix), with other pixels or another range is not copied; nor is footage coded at
// well over what the export's own settings would spend on it, or at a profile, tier or level above the one the export's
// encoder states (or one its decoders need not decode). Every picture is shown
// once and decoded in order: the timestamps are rebuilt for the whole stream.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/HrdCheck.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVStream;
struct AVFrame;
struct AVPacket;

namespace montage {

struct ColorSpace;

// The one clip seen at sequence frame `f` when nothing changes its pictures (no effect, grade, colour group, transition,
// speed change, blend mode or motion, and no other clip in view); null otherwise (render/Exporter.cpp).
const Clip* untouchedClipAt(const Sequence& seq, FrameTime f);

struct LongGopEncoder {
    std::string preset;      // x264's and x265's
    std::string x265Params;  // the export's own (HDR10's), which the encoded parts keep
    int crf = 23;            // x264's and x265's quality, held under the rate of the GOPs copied either side
    int64_t bitrate = 0;     // or the export's bit rate, when it has one
    int gop = 0;             // the export's keyframe interval for the encoded parts (0: the footage's)
    // HEVC: parameter sets in the video (hev1), needed where encoded parts or more than one source meet copied GOPs
    // under the same ids. Apple's players refuse hev1, so without it such an export is encoded whole; copies that need
    // only one source's sets are written hvc1.
    bool inBandHevc = false;
    std::string startTimecode;  // MPEG-2: the GOP headers' time codes run from it (hh:mm:ss:ff, ';' drop frame; empty: 0)
    // A GOP is read whole and checked before it is written: one coded in more bytes than this (a screen recording's
    // single IDR picture, an action camera's GOPs of several seconds) is encoded rather than held to be copied.
    int64_t maxGopBytes = int64_t(64) << 20;
    // The export's encoder, opened: the size, pixel format, rate, colour tags, field order and (MPEG-2) rate control the
    // encoded parts are made with. Its codec is libx264, libx265, mpeg2video, libsvtav1, libaom-av1 or libvpx-vp9.
    const AVCodecContext* main = nullptr;
};

class LongGopCopier {
public:
    ~LongGopCopier();
    // Plans an export of sequence frames [in, out) into `oc`'s video stream `vst` (MP4 or MOV for H.264 and HEVC; MXF
    // or MOV for MPEG-2): null, with `why`, when no GOP of any source would be copied (the export is then encoded whole).
    static std::unique_ptr<LongGopCopier> plan(const Project& p, const Sequence& seq, FrameTime in, FrameTime out, const ColorSpace& seqSpace,
                                               const LongGopEncoder& encoder, AVFormatContext* oc, AVStream* vst, std::string* why = nullptr);
    // Before the file's header: the stream's decoder configuration (H.264: the sources' and the encoder's parameter
    // sets; HEVC: the first copied source's, as hev1).
    bool prepareStream(std::string* error);
    // Frame `rel` of the export (0 = `in`): 1 when it is copied (the whole GOP is written at its first frame), 0 when it
    // is to be rendered and given to encode(), -1 on failure, -2 when the export is to be made again encoded whole
    // (HEVC written hvc1 whose footage was not found as planned: no encoded part can sit beside its sets).
    int copy(FrameTime rel, std::string* error);
    // A rendered frame `rel`, in the main encoder's format, for the encoded part it falls in.
    bool encode(AVFrame* frame, FrameTime rel, std::string* error);
    // The last encoded part finished; before the file's trailer.
    bool finish(std::string* error);
    int copiedFrames() const { return copied_; }
    int encodedParts() const { return parts_; }

    struct Packet;
    struct Gop;
    struct Source;
    struct Run;
    struct Copy;

private:
    LongGopCopier() = default;
    // A copy's packets read from its source (1), or not found as planned (0: its frames are then encoded), or -1.
    int readGop(const Copy& c, std::vector<AVPacket*>& out, std::string* error);
    bool writeGop(const Copy& c, const std::vector<AVPacket*>& gop, std::string* error);
    bool startPart(FrameTime rel, std::string* error);
    bool endPart(std::string* error);
    bool receive(std::string* error);
    bool flushPending(std::string* error);
    bool write(AVPacket* pkt, FrameTime display, bool ours, std::string* error);
    bool writeFinal(AVPacket* pkt, FrameTime display, std::string* error);
    bool writeHeld(std::vector<uint8_t> au, FrameTime display, std::string* error);
    // HRD: an SPS NAL unit (whole) with the footage's HRD in place of the encoder's; as it is when there is none.
    std::vector<uint8_t> withHrd(const uint8_t* sps, size_t n) const;
    void setsWithHrd(std::vector<uint8_t>& lengthPrefixed) const;
    int64_t partBitrate(FrameTime a, FrameTime b) const;
    double ceilingBitsPerFrame() const;
    AVCodecContext* openEncoder(int64_t bitrate, std::string* error) const;

    LongGopEncoder enc_;
    AVFormatContext* oc_ = nullptr;
    AVStream* vst_ = nullptr;
    int codec_ = 0;  // AVCodecID
    FrameTime frames_ = 0;
    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<Run> runs_;
    std::vector<Copy> copies_;
    size_t cursor_ = 0;
    int lengthSize_ = 4;
    int spsId_ = -1;                      // H.264: the encoder's parameter sets' id
    std::vector<uint8_t> encoderSets_;    // H.264: those sets as the decoder configuration holds them (length-prefixed)
    std::vector<uint8_t> activeEncoderSets_;
    std::vector<uint8_t> partSets_;       // H.264: the encoded part's (they match the configuration's unless x264 made them differently)
    int firstSource_ = -1;
    bool hvc1_ = false;                   // HEVC: every set out of band (one source's, no encoded part)
    int profile_ = 0, level_ = 0;         // the stream's: the highest of the sources' (and H.264's encoder's)
    bool highTier_ = false;               // HEVC: a source of the high tier
    int64_t vbvLevel_ = -1;               // MPEG-2 at a constant rate: the buffer's bits after the last copied GOP
    int64_t timecodeStart_ = 0;           // MPEG-2: the export's first frame's time code, as a frame count
    bool dropFrame_ = false;
    int gop_ = 12;
    int depth_ = 0;  // the stream's decode delay, in frames
    int joinBframes_ = 3;  // (H.264, HEVC) the joins' B-pictures, and whether they nest (so they reorder no deeper than the footage)
    bool joinPyramid_ = true;
    int64_t decoded_ = 0;
    // The encoded part in progress.
    AVCodecContext* ctx_ = nullptr;
    FrameTime partStart_ = 0, partEnd_ = 0;
    bool firstOfPart_ = false;
    AVPacket* pkt_ = nullptr;
    std::vector<uint8_t> pending_;
    FrameTime pendingPts_ = 0;
    bool pendingKey_ = false, havePending_ = false;
    int copied_ = 0, parts_ = 0;
    // The buffer model (render/HrdCheck.h).
    bool hrdOn_ = false;
    double hrdRate_ = 0, hrdBuffer_ = 0;
    std::vector<uint8_t> hrdSps_;  // H.264 and HEVC: the footage's SPS whose HRD the encoded parts are given
    HrdSplicer hrd_;
    int heldFlags_ = 0;
    FrameTime lastCopiedHi_ = -2;
    // H.264 entered at a recovery point: what its I-picture's frame_num was (taken off those after it until an IDR
    // picture), and the idr_pic_id the next IDR picture made takes.
    int64_t frameNumShift_ = -1;  // (H.264, after an I-picture made an IDR one: its frame_num, taken off those after; -1 none)
    uint64_t pocShift_ = 0, sinceIdr_ = 0;  // (and its pic_order_cnt_lsb; the latest frame_num since, not wrapped)
    // (2 to 65535: never x264's 0 and 1, so a made IDR picture right after an encoded one has another idr_pic_id)
    int idrPicId_ = 2;
};

}  // namespace montage
