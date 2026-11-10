// Montage — Compare Encode: a delivered file set against the picture it was made from (the sequence's own render, or
// any reference file), frame by frame, the way encoders are judged:
//  - PSNR of luma (and of each plane) as FFmpeg's psnr filter measures it, and of R′G′B′;
//  - SSIM as FFmpeg's ssim filter measures it (8x8 windows a 4-pixel step apart, made of 4x4 sums);
//  - MS-SSIM (Wang, Simoncelli and Bovik 2003: an 11-tap Gaussian window of σ 1.5 over five scales);
//  - ΔICtCp, the colour difference of ITU-R BT.2124 (ΔE_ITP = 720 √(ΔI² + (ΔCt/2)² + ΔCp²), 1 the smallest
//    difference seen), on the light each picture shows: PQ as coded, HLG on a 1000-nit display, SDR with white at
//    100 nits;
//  - VMAF through libvmaf (built with Montage, MONTAGE_WITH_VMAF): Netflix's v1 model for SDR, built into libvmaf, and
//    its HDR v1 model for PQ and HLG where libvmaf has it built in or it is in the VMAF models folder (libvmaf v3.2.1
//    has none, so HDR encodes are compared without VMAF until then).
// The two are aligned first by a search over ±N frames on small pictures. The encode is compared in its own format:
// the reference is converted to it (size, sampling, bit depth and matrix) where it differs.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

// A picture as the metrics see it: Y′CbCr (or R′G′B′) planes of 8 to 16 significant bits, one sample a uint16_t.
struct QualityPicture {
    int width = 0, height = 0;    // the first plane's
    int bits = 8;
    int log2ChromaW = 1, log2ChromaH = 1;
    bool rgb = false;             // planes G, B, R (as FFmpeg's planar RGB) instead of Y, Cb, Cr
    bool fullRange = false;
    bool chromaLeft = true;       // subsampled chroma sited with the left luma sample (as MPEG's default), else centred
    int chromaLoc = 0;            // the file's chroma site as tagged (FFmpeg's AVChromaLocation; 0 untagged)
    double kr = 0.2126, kb = 0.0722;  // the matrix's weights
    // What the code values mean, for ΔICtCp: "bt1886" (SDR), "pq" or "hlg"; "bt709", "bt2020" or "p3" primaries.
    std::string transfer = "bt1886", primaries = "bt709";
    double frameRate = 0;         // of the file it came from (frames a second), when known
    double time = std::numeric_limits<double>::quiet_NaN();  // when it is shown (seconds, as its file stamps it), when known
    bool clockMayJump = false;    // its file's clock can start again part way (MPEG-TS and the like): a jump is not a gap
    bool damaged = false;         // the decoder reported an error in it (or in data lost just before it)
    std::vector<uint16_t> plane[3];
    int planeWidth(int i) const { return i == 0 || rgb ? width : (width + (1 << log2ChromaW) - 1) >> log2ChromaW; }
    int planeHeight(int i) const { return i == 0 || rgb ? height : (height + (1 << log2ChromaH) - 1) >> log2ChromaH; }
    int peak() const { return (1 << bits) - 1; }
    bool sameLayout(const QualityPicture& o) const {
        return width == o.width && height == o.height && bits == o.bits && log2ChromaW == o.log2ChromaW &&
               log2ChromaH == o.log2ChromaH && rgb == o.rgb;
    }
};

// One plane's mean squared error and PSNR (10 log10(peak² / MSE), infinite when equal), as FFmpeg's psnr filter.
double planeMse(const QualityPicture& a, const QualityPicture& b, int plane);
double psnrFromMse(double mse, int peak);
// The mean squared error over R′G′B′ (each picture converted with its own matrix and range, values 0..1), and its
// PSNR (peak 1).
double rgbMse(const QualityPicture& a, const QualityPicture& b);
double rgbPsnr(const QualityPicture& a, const QualityPicture& b);
// One plane's SSIM exactly as FFmpeg's ssim filter computes it (its integer sums, its float formula).
double planeSsim(const QualityPicture& a, const QualityPicture& b, int plane);
// MS-SSIM of the first plane (luma) over five scales (fewer when the picture is too small for five), each half the
// last (odd sizes rounded down).
double msSsim(const QualityPicture& a, const QualityPicture& b);
// BT.2124's ΔE_ITP between the two pictures, pixel by pixel at the luma plane's size (each pixel's chroma from the
// sample sited with it, or the nearest before it): the mean, and the largest.
void deltaItp(const QualityPicture& a, const QualityPicture& b, double& mean, double& largest);
// One pixel's ICtCp (BT.2100, through PQ) from linear BT.2020 RGB in cd/m².
void ictcpFromNits(const double rgbNits[3], double ictcp[3]);
// The light a pixel of the picture shows, as linear BT.2020 RGB in cd/m², from its R′G′B′ (0..1).
void nitsOfSignal(const QualityPicture& like, const double rgb[3], double nits[3]);
// R′G′B′ (0..1) of the sample at plane coordinates (x, y), chroma taken from its co-sited sample.
void signalAt(const QualityPicture& p, int x, int y, double rgb[3]);

enum QualityMetric : unsigned {
    MetricPsnr = 1, MetricSsim = 2, MetricMsSsim = 4, MetricDeltaItp = 8, MetricVmaf = 16,
    MetricsAll = MetricPsnr | MetricSsim | MetricMsSsim | MetricDeltaItp | MetricVmaf
};
// "psnr", "ssim", "ms_ssim", "delta_ictcp", "vmaf"; 0 for anything else.
unsigned qualityMetricFromName(const std::string& name);
// Whether Montage was built with libvmaf (it is unless configured without MONTAGE_WITH_VMAF), its version
// ("v3.2.1"), and whether it has this model built in, by its version ("vmaf_v1.0.16_3d0h").
bool vmafAvailable();
std::string vmafVersion();
bool vmafModelBuiltIn(const std::string& version);
// Whether VMAF can measure a picture of this layout: Y′CbCr 4:2:0, 4:2:2 or 4:4:4.
bool vmafMeasures(const QualityPicture& p);

// VMAF through libvmaf: pairs of pictures (of one layout) fed in order, each pair's score read at the end. Also used
// by the per-shot streaming ladders' trial encodes (render/StreamingPackage).
class VmafMeter {
public:
    VmafMeter();
    ~VmafMeter();
    VmafMeter(const VmafMeter&) = delete;
    VmafMeter& operator=(const VmafMeter&) = delete;
    // `model`: a model's JSON file, or one libvmaf has built in, by its version (as vmafModelFor gives).
    bool open(const std::string& model, int threads, std::string* error = nullptr);
    bool add(const QualityPicture& reference, const QualityPicture& encode);
    bool finish(std::vector<double>& scores);  // a score a pair, in the order they were added
    unsigned pictures() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

constexpr double kNoScore = std::numeric_limits<double>::quiet_NaN();

// (For an R′G′B′ encode the luma figures, PSNR, its MSE and SSIM, are over all three planes, and U and V are not
// measured.)
struct FrameQuality {
    FrameTime frame = 0;         // in the reference's frames (the sequence's when it is the reference)
    double psnrY = kNoScore;     // dB, infinite when the luma is identical
    double psnrU = kNoScore, psnrV = kNoScore;
    double psnrRgb = kNoScore;
    double mseY = 0, mseU = 0, mseV = 0, mseRgb = 0;  // (the means' PSNR is from the mean MSE, as FFmpeg's)
    double ssim = kNoScore;      // luma
    double ssimAll = kNoScore;   // the planes weighted by their sizes, as FFmpeg's "All"
    double msSsim = kNoScore;
    double deltaItp = kNoScore, deltaItpMax = kNoScore;
    double vmaf = kNoScore;
};

struct CompareOptions {
    unsigned metrics = MetricsAll;  // (VMAF is left out where Montage has no libvmaf)
    int search = 12;         // the encode may start up to this many frames before or after the reference
    int alignFrames = 48;    // frames compared for the alignment
    int threads = 0;         // 0 = as many as the computer has; results are the same whatever this is
    int worstCount = 5;      // the worst frames listed (by R′G′B′ PSNR, else SSIM, else VMAF)
    // A model's JSON file, or a built-in model's version; "" = the v1 model built into libvmaf (for PQ and HLG the
    // HDR v1 model, built in or from vmafModelDir; for pictures whose chroma is under 80 samples either way, which the
    // v1 model's chroma feature cannot measure, the v0.6.1 model).
    std::string vmafModel;
    // Where HDR VMAF's model file may be: MONTAGE_VMAF_MODELS, else the vmaf-models folder beside the program.
    std::string vmafModelDir;
    // What the files' code values mean when their tags do not say (Y4M, untagged streams): "" = from the tags,
    // else as QualityPicture's transfer and primaries.
    std::string assumeTransfer, assumePrimaries;
    int maxFrames = 0;       // 0 = all
};

struct EncodeComparison {
    bool ok = false;
    std::string error;
    int offset = 0;              // encode frame i shows reference frame i - offset
    double alignCost = 0;        // the mean squared difference of the small pictures at that offset
    int width = 0, height = 0, bits = 8;
    std::string format;          // the encode's: "4:2:0 10-bit", "R′G′B′ 8-bit"...
    std::vector<FrameQuality> frames;
    // Over every frame: PSNR from the mean squared error (as FFmpeg's average), the means of the rest, and the worst.
    FrameQuality mean, worst;
    std::vector<FrameTime> worstFrames;  // the worst first
    bool vmafScored = false;
    std::string vmafModel;       // the model used
    // How many frames each has after the alignment's offset is taken off (-1: not known), frames the decoder reported
    // damaged in the encode, and what is wrong but did not stop the comparison (frames missing or extra, damage).
    int64_t referenceFrames = -1, encodeFrames = -1;
    int damagedFrames = 0;
    // Matched by the encode's timestamps: reference frames the encode does not have (dropped), and encode frames
    // shown again (repeated) or out of step, which were left out.
    int64_t droppedFrames = 0, repeatedFrames = 0;
    std::vector<std::string> warnings;
};

// The pictures of a source, one after another; false at its end.
using PictureSource = std::function<bool(QualityPicture&)>;

// Compares `encode` with `reference`, both opened again for each pass (alignment, then the comparison): `open` makes a
// fresh source. Frames are compared in parallel; each frame's figures come from that frame alone.
// `referenceLength`, when the reference's length is known (a sequence's range), saves reading it to its end to count.
EncodeComparison compareSources(const std::function<PictureSource()>& openReference,
                                const std::function<PictureSource()>& openEncode, const CompareOptions& o,
                                const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                int64_t referenceLength = -1);

// How many frames a file has (its stream's count, or its duration at its rate), -1 if it cannot say.
int64_t pictureFileLength(const std::string& path);
// A file's pictures as decoded, or converted to `like`'s layout (size, sampling, bit depth, matrix, range) when given.
PictureSource openPictureFile(const std::string& path, const QualityPicture* like, const CompareOptions& o,
                              std::string* error = nullptr);
// The first picture of a file, as decoded (its layout and tags).
bool probePictureFile(const std::string& path, QualityPicture& out, const CompareOptions& o, std::string* error = nullptr);
// The VMAF model a comparison in `like`'s layout and tags uses (o.vmafModel when given); "" when there is none for it
// (HDR with no HDR model).
std::string vmafModelFor(const QualityPicture& like, const CompareOptions& o);

// Two files.
EncodeComparison compareEncodeFiles(const std::string& reference, const std::string& encode, const CompareOptions& o,
                                    const std::function<void(double)>& progress = {},
                                    const std::atomic<bool>* cancel = nullptr);
// A file against the sequence's own render over [in, out) (out < 0: to the end), the render converted to the file's
// layout with the file's matrix and range; frames are the sequence's. The file is taken to start at `in`, unless
// `encodeStartsAt` says it starts at another sequence frame (a whole delivery checked over a range: 0).
EncodeComparison compareEncodeWithSequence(const Project& p, const Sequence& s, FrameTime in, FrameTime out,
                                           const std::string& encode, const CompareOptions& o,
                                           const std::function<void(double)>& progress = {},
                                           const std::atomic<bool>* cancel = nullptr, FrameTime encodeStartsAt = -1);

// Limits a comparison is held to (Quality Check presets); 0 = not checked.
struct QualityLimits {
    double minPsnr = 0;      // dB, the luma of the worst frame
    double minVmaf = 0;
    double maxDeltaItp = 0;  // a frame's mean ΔE_ITP
};
// Stretches of frames past a limit ([first, end), joined when next to each other), with what is wrong.
struct QualitySpan {
    FrameTime first = 0, end = 0;
    std::string text;
};
std::vector<QualitySpan> qualityOverLimits(const EncodeComparison& c, const QualityLimits& l);

// The comparison as CSV ('#' lines with the summary, then a row a frame) and as a PDF page or more (the summary, a
// graph of the scores over time, the worst frames).
bool writeQualityCsv(const std::string& path, const EncodeComparison& c, const Sequence* s, std::string* error = nullptr);
bool writeQualityPdf(const std::string& path, const EncodeComparison& c, const std::string& title, const QualityLimits& l,
                     const Sequence* s, std::string* error = nullptr);

}  // namespace montage
