// Montage — smart rendering of long-GOP exports (H.264, HEVC, MPEG-2 as XDCAM HD422 has it): where the export is coded
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
// Footage shown turned (a display matrix), with other pixels or another range is not copied. Every picture is shown
// once and decoded in order: the timestamps are rebuilt for the whole stream.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"

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
    // The export's encoder, opened: the size, pixel format, rate, colour tags, field order and (MPEG-2) rate control the
    // encoded parts are made with. Its codec is libx264, libx265 or mpeg2video.
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
    // is to be rendered and given to encode(), -1 on failure.
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
    bool writeGop(const Copy& c, std::string* error);
    bool startPart(FrameTime rel, std::string* error);
    bool endPart(std::string* error);
    bool receive(std::string* error);
    bool flushPending(std::string* error);
    bool write(AVPacket* pkt, FrameTime display, std::string* error);
    int64_t partBitrate(FrameTime a, FrameTime b) const;
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
    int gop_ = 12;
    int depth_ = 0;  // the stream's decode delay, in frames
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
};

}  // namespace montage
