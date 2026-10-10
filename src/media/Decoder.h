// Montage — FFmpeg-based media probing and decoding.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CameraRaw.h"
#include "Image.h"
#include "core/Interpretation.h"
#include "core/Model.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace montage {

// Fills `out` (kind, duration, dimensions, codecs...) from the file at `path`.
bool probeMedia(const std::string& path, MediaItem& out, std::string* error = nullptr);
// A project saved before Montage read stereo packing: its stereo video files (side-by-side or top-and-bottom metadata)
// are found from their headers and read again, so their items say so and give one eye's size, as they would imported
// now. Nothing happens once a project has been checked (Project::stereoChecked).
void checkStereoMedia(Project& p);

// Captions carried inside a video as CEA-608 (the A/53 caption data of H.264, HEVC and MPEG-2 frames, as broadcast
// and camera files carry them): caption channel 1, timed in frames at `fps` of the media's own time. The whole video
// is read. False (with `error`) if it holds none.
bool readEmbeddedCaptions(const std::string& path, Rational fps, std::vector<Caption>& out,
                          const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                          std::string* error = nullptr);

struct ImageSequence;

// Frame-accurate video decoder. Not thread safe: use one per thread
// (MediaPool hands them out).
class VectorDocument;

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    bool isOpen() const { return ctx_ != nullptr || vector_ != nullptr || raw_ != nullptr || rawSequence_ != nullptr; }

    // Frame displayed at media time `t` (seconds from the start of the file; of the conformed file when the path carries
    // an interpretation, core/Interpretation.h), converted to RGBA16 at exactly targetW x targetH (0 = native display size).
    Frame16Ptr frameAt(double t, int targetW = 0, int targetH = 0, bool highQuality = false);

    int displayWidth() const { return dispW_; }
    int displayHeight() const { return dispH_; }
    double fps() const { return fps_ / timeScale_; }
    double duration() const { return duration_ / timeScale_; }
    bool isStill() const { return still_; }
    // Media time of the most recently decoded frame (for pool scheduling).
    double position() const { return curPts_ / timeScale_; }
    // The hardware device decoding this stream ("videotoolbox", "d3d11va"...), or "" for software.
    const std::string& hardware() const { return hwName_; }
    const std::string& path() const { return path_; }

private:
    bool openCodec(bool tryHardware, std::string* error);
    void freeCodec();
    static int pickFormat(AVCodecContext* ctx, const int* formats);
    bool decodeNext(AVFrame* into);  // false at EOF / error
    bool seek(double t);
    Frame16Ptr convert(const AVFrame* f, double pts, int w, int h, bool hq);

    std::string path_;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* cur_ = nullptr;   // latest frame with pts <= requested time
    AVFrame* next_ = nullptr;  // look-ahead frame (pts > requested time)
    bool haveCur_ = false;
    bool haveNext_ = false;
    double curPts_ = -1;
    double nextPts_ = -1;
    bool eof_ = false;
    int stream_ = -1;
    double timeBase_ = 0;
    double origin_ = 0;      // file start time, seconds
    double fps_ = 25;
    double duration_ = 0;
    int rotation_ = 0;       // degrees clockwise to apply (0/90/180/270)
    int dispW_ = 0, dispH_ = 0;
    bool still_ = false;
    Frame16Ptr stillFrame_;
    SwsContext* sws_ = nullptr;
    int swsKey_[6] = {};  // the conversion sws_ was made for: sizes, source format, flags
    // Hardware decoding: the device's pixel format (-1 = software), the frame
    // hardware frames are copied into, and whether a copy failed (reopen in software).
    int hwPixFmt_ = -1;
    bool hwSlot_ = false;
    bool hwBroken_ = false;
    std::string hwName_;
    AVFrame* hwTransfer_ = nullptr;
    // Interpret Footage (core/Interpretation.h): file seconds per media second, pixel aspect (0 = the file's), how alpha
    // and fields are read.
    double timeScale_ = 1;
    double par_ = 0;
    std::string alpha_, fields_;
    Interpretation interp_;
    std::string stereo_;  // stereoscopic footage's packing ("sbs", "sbs_half", "tb", "tb_half"), "" = flat
    int eye_ = 0;         // the eye read (0 left, 1 right), after any swap
    std::shared_ptr<VectorDocument> vector_;  // a Lottie animation or SVG, rendered instead of decoded
    AVFrame* raw_ = nullptr;                  // a camera raw still, developed once (RGB48), or a CinemaDNG run's current frame
    std::unique_ptr<ImageSequence> rawSequence_;  // a run of camera raw frames (media/ImageSequence.h)
    int rawFrame_ = -1;                       // the run's frame in raw_
    RawSettings rawSettings_;                 // how raw pictures are developed (Interpret Footage)
    bool loadRaw(const RawImage& img, std::string* error);
};

struct AudioBuffer {
    int sampleRate = 48000;
    std::vector<float> samples;  // interleaved stereo
    int64_t frames() const { return int64_t(samples.size() / 2); }
};
using AudioBufferPtr = std::shared_ptr<const AudioBuffer>;

// Opens media for reading with FFmpeg: a file, or a numbered image sequence's path (media/ImageSequence.h).
int openMediaInput(struct AVFormatContext** fmt, const std::string& path);

// Decodes the whole first audio stream to stereo float at `sampleRate`,
// aligned so that sample 0 is media time 0 (same origin as VideoDecoder).
// With `channels` (indexes across every audio stream, Clip::channels), those
// channels instead: one in the centre, two as left and right, more summed
// alternately left and right.
AudioBufferPtr decodeAudio(const std::string& path, int sampleRate, std::string* error = nullptr,
                           const std::atomic<bool>* cancel = nullptr, const std::vector<int>& channels = {});

// Min/max envelope for waveform drawing: one (min, max) pair per bucket.
struct Peaks {
    int samplesPerBucket = 256;
    int sampleRate = 48000;
    std::vector<float> minmax;  // 2 floats per bucket (mono mix)
};
using PeaksPtr = std::shared_ptr<const Peaks>;
PeaksPtr computePeaks(const AudioBuffer& buf, int samplesPerBucket = 256);

// Rotates an RGBA16 frame clockwise by 90/180/270 degrees.
Frame16 rotateFrame(const Frame16& f, int degrees);

// Deinterlacing (as Premiere and Resolve do automatically): a frame flagged as
// interlaced keeps its first field, and each line of the other field is
// rebuilt from the lines either side only where it combs (lies outside them),
// so still detail stays sharp and motion loses its teeth. Works on planar and
// semi-planar YUV (8 to 16 bits) in place; returns whether it changed anything.
bool deinterlaceFrame(AVFrame* f);
// 0 progressive, 1 top field first, 2 bottom field first.
int fieldDominance(const AVFrame* f);
void setFieldDominance(AVFrame* f, int dominance);
// Interpret Footage's alpha on a decoded frame (straight alpha): "ignore" makes it opaque, "invert" turns it over,
// "premultiplied" divides the colour by it.
void interpretAlpha(Frame16& f, const std::string& mode);

}  // namespace montage
