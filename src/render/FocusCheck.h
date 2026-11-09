// Montage — focus checks: focus peaking over the monitors' pictures, and soft shots found frame by frame for Quality
// Check (a sequence's shots) and for dailies (media items).
//
// Everything is measured on luma (0 black, 1 white) through Gaussians. The fine band (σ 0.6 px less σ 1.5 px) holds
// the detail defocus takes first; the broad band (σ 0.6 less σ 4) the contrast at the scales around it. A frame's
// sharpness is the fine band's energy over the broad band's, summed over its busiest tiles (those with the most
// broad-band energy), so it does not change with exposure or the picture's own contrast: a dark or flat shot is not
// called soft, and every extra blur only lowers it. Motion blur is told from defocus by structure tensors (the sums of
// each gradient's outer product) of two bands a step coarser, σ 1 less σ 2.5 set against σ 2.5 less σ 6, through the
// pair's generalised eigenvalues: alike when detail was lost alike every way, one well under the other when the
// picture was smeared one way. (Coarser than the fine band, so that grain does not hide a smear.)
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

struct FocusMeasure {
    double sharpness = 0;   // the fine band's energy over the broad band's over the busiest tiles, 0 to 1
    double contrast = 0;    // the broad band's RMS over those tiles (luma)
    double anisotropy = 0;  // 0: detail lost alike every way; towards 1: lost one way only (motion blur)
    double blurAngle = 0;   // degrees, the way detail was smeared (0 across, 90 up and down), where anisotropy says
    bool textured = false;  // enough detail to judge: a flat, black or nearly empty frame is not
    bool operator==(const FocusMeasure&) const = default;
};

// `luma` is w x h, row by row. `threads` 0 uses every core; the result is the same on any number.
FocusMeasure measureFocus(const float* luma, int w, int h, int threads = 0);
// An RGBA picture's (premultiplied or not: alpha is ignored) Rec.709 luma, clamped to 0..1.
std::vector<float> lumaOf(const Image& img);

// Focus peaking: 1 where the fine band's magnitude reaches `threshold` (luma), else 0.
std::vector<uint8_t> focusPeaking(const float* luma, int w, int h, double threshold, int threads = 0);
// The threshold for a sensitivity: 0 low (only the crispest edges), 1 medium, 2 high.
double peakingThreshold(int sensitivity);

struct FocusOptions {
    int width = 960;                // pictures are measured at this width (never above their own)
    double softShare = 0.6;         // frames under this share of their shot's best are soft
    double angleShare = 0.5;        // shots under this share of the sharpest other angle of their scene are soft
    double motionAnisotropy = 0.2;  // frames past this have motion blur (a sharp or defocused picture reads about 0.05)
    double minSeconds = 0.2;        // shorter stretches are not reported
    int threads = 0;                // for the measuring (0 every core); results are the same on any number
};

// A shot and its frames' measures (the frames from `start`, in the sequence's or the media's frames).
struct FocusShot {
    FrameTime start = 0, end = 0;  // [start, end)
    std::string scene, angle;      // "" none: a shot with a scene is compared with the scene's other angles
    std::string sceneName, angleName;
    std::vector<FocusMeasure> frames;
    // What judgeFocus found: the best (90th percentile) and typical (median) sharpness of its frames without motion blur.
    double best = 0, typical = 0;
};

enum class FocusFinding { Soft, SoftAngle, MotionBlur };

struct FocusIssue {
    FocusFinding kind = FocusFinding::Soft;
    FrameTime start = 0, end = 0;  // [start, end), in the shots' frames
    size_t shot = 0;               // its index in the shots
    double share = 0;              // its sharpness as a share of what it is compared with (Soft, SoftAngle)
    double anisotropy = 0, angle = 0;  // MotionBlur: the stretch's median
    std::string text;
};

// The shots judged: stretches of a shot well under its own best, shots well under the sharpest other angle of their
// scene, and stretches of motion blur (reported as that, not as focus). In time order.
std::vector<FocusIssue> judgeFocus(std::vector<FocusShot>& shots, const FocusOptions& o, double fps);

// The sequence's shots over [in, out) (out < 0: to the end), each a run of frames whose topmost picture is one clip
// (clips of one media following on in its time are one shot), measured as rendered, HDR as shown on an SDR display.
// A shot's scene and angle are its Group Shots groups (core/ColorGroups.h), else its media's scene and camera metadata.
// Empty when cancelled.
std::vector<FocusIssue> checkSequenceFocus(const Project& p, const Sequence& s, FrameTime in, FrameTime out, const FocusOptions& o,
                                           const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                           std::vector<FocusShot>* shots = nullptr);

// Dailies: each picture media item checked frame by frame as a shot of its own, its scene and angle from its scene and
// camera (else shot) metadata. `summary` is what the media bin's Focus column shows ("Sharp", or each finding at its
// timecodes); `checked` false for media that could not be read.
struct MediaFocus {
    Id media = 0;
    bool checked = false;
    double typical = 0;
    std::vector<FocusIssue> issues;  // in the media's frames from 0
    std::string summary;
};
std::vector<MediaFocus> checkMediaFocus(const Project& p, const std::vector<Id>& media, const FocusOptions& o,
                                        const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
