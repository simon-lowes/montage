// Montage — Quality Check before delivery (the checks broadcasters' QC tools run, such as Baton, Vidchecker or the
// Harding test): the program is rendered small and listened to, and each problem gets a time span:
//  - flashing that can trigger photosensitive seizures (ITU-R BT.1702, Ofcom's guidance, WCAG 2.3.1): opposing
//    changes of 20 cd/m² or more (darker side under 160 cd/m²) or to or from saturated red, over a quarter of the
//    screen, more than three flashes in any second;
//  - picture levels outside EBU R103 (RGB -5 % to 105 %, luma -1 % to 103 %) on more than 1 % of the picture;
//  - black and frozen picture, silence and clipped sound held too long;
//  - integrated loudness away from the target and true peaks over the ceiling;
//  - spelling in captions (in each track's language) and titles (core/SpellCheck.h), the project's vocabulary allowed;
//  - linked clips out of sync with their picture (core/EditOps.h syncOffsets).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"

namespace montage {

struct QcSettings {
    bool flashing = true;
    bool levels = true;             // only for SDR sequences
    double blackSeconds = 1.0;      // 0 = not checked
    double freezeSeconds = 5.0;     // 0 = not checked
    double silenceSeconds = 2.0;    // below -60 dBFS; 0 = not checked
    bool clipping = true;           // samples at full scale
    double loudnessTarget = 0;      // LUFS, within 1 LU; 0 = not checked
    double peakCeiling = -1;        // dBTP, checked with the loudness
    int analysisWidth = 160;        // the picture is checked at this width
    bool spelling = true;           // captions and titles
    std::string titleLanguage = "en-US";  // the dictionary titles are checked with
    bool sync = true;               // linked sound or picture out of sync
};

enum class QcKind { Flashing, RedFlashing, Levels, Black, Freeze, Silence, Clipping, Loudness, TruePeak, Spelling, OutOfSync };

struct QcIssue {
    QcKind kind = QcKind::Flashing;
    FrameTime start = 0, end = 0;  // sequence frames, [start, end)
    std::string text;
};

const char* qcKindName(QcKind k);

// Counts flashes the way the guidelines do, a frame at a time: each frame's change in relative luminance (and in
// saturated red) over the part of the screen that changed together, accumulated while it keeps going one way;
// a transition is 0.1 of relative luminance (20 cd/m² on the 200 cd/m² display assumed) with the darker side under
// 0.8, and a flash is two opposing transitions.
class FlashDetector {
public:
    explicit FlashDetector(double fps) : fps_(fps) {}
    // Linear-light RGB, `w` x `h` interleaved (3 floats a pixel). Returns the transitions found this frame:
    // bit 0 luminance, bit 1 red.
    int add(const float* rgb, int w, int h);
    // Transitions in the second (fps frames) ending with the last frame added, and the frames (counted from the
    // first added) of the first and last of them.
    int transitionsInLastSecond(bool red = false) const;
    std::pair<int, int> lastSecondSpan(bool red = false) const;

private:
    struct Track {
        double acc = 0;      // the run's accumulated change
        double darker = 1;   // the darkest level the run started from or reached
        bool counted = false;
        std::vector<int> frames;  // the frames transitions happened on
    };
    void step(Track& t, double change, double before, double after, double threshold, double darkLimit, bool& hit);
    double fps_;
    int frame_ = 0;
    std::vector<float> prevY_, prevRed_;
    Track lum_, red_;
};

// Runs the checks over [in, out) (out < 0: to the end), in time order.
std::vector<QcIssue> qualityCheck(const Project& p, const Sequence& s, FrameTime in, FrameTime out, const QcSettings& settings,
                                  const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Red markers spanning each issue, named "QC: <kind>", in place of earlier QC markers. Returns how many.
int addQcMarkers(Sequence& s, const std::vector<QcIssue>& issues);

// Makes a picture broadcast safe in place (EBU R103 or strict 0-100 %): over-range colours are pulled towards their
// own luma (keeping hue) until every channel fits, then luma is limited; a soft knee keeps gradations. With
// `highlight`, the pixels that needed it are striped instead. Returns how many pixels were out of range.
// Values are the signal (0 = black, 1 = white).
int broadcastSafe(float* rgba, int w, int h, bool strict, double knee = 0.05, bool highlight = false);

}  // namespace montage
