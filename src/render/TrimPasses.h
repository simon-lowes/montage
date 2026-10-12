// Montage — HDR and SDR trim passes (Resolve's MultiMaster Trim, the per-shot trims of Dolby Vision and HDR10+, kept
// as open metadata of Montage's own): a cut graded once (usually in HDR) is delivered to several displays, SDR Rec.709
// at 100 nits, HDR10 at 1000, 600 or 300 nits, HLG, each a trim target of the sequence (Sequence::trimTargets). A
// target maps the master to its space and peak (HDR into SDR as Montage tone maps it; PQ above the target's peak rolled
// off with BT.2390's EETF), then each shot is trimmed for that display: lift, gamma, gain, saturation, a highlight
// roll-off and mid-tone detail, on its code values (Clip::trims, keyframable). A shot is the top picture clip on screen;
// its trim takes in the whole frame, as Dolby Vision's do (titles over it with it). The Program monitor and reference
// output show a target (RenderOptions::trimTarget), and Export delivers one (ExportSettings::trimTarget).
#pragma once

#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"
#include "media/Image.h"

namespace montage {

struct ColorSpace;

// The usual targets: "sdr" (Rec.709, 100 nits), "hdr10-1000", "hdr10-600", "hdr10-300" (PQ) and "hlg".
const std::vector<TrimTarget>& trimTargetPresets();
const TrimTarget* findTrimTarget(const Sequence& s, const std::string& id);
// The space and peak a target's pictures are in (the sequence's own for "" or an unknown target).
const ColorSpace& trimTargetSpace(const Sequence& s, const std::string& id);
double trimTargetPeak(const Sequence& s, const std::string& id);

// A trim's controls: lift, gamma, gain, saturation, highlights, mid_detail, each with its default (no change).
struct TrimControl {
    const char* name;
    const char* label;
    double def, min, max;
};
const std::vector<TrimControl>& trimControls();
Effect makeTrim();  // the defaults
bool isNeutralTrim(const Effect& trim, FrameTime t);

// The shot whose trim a frame takes: the top enabled picture clip on a shown track at t (not an adjustment layer).
const Clip* trimShot(const Sequence& s, FrameTime t);
// The program frame at t, in the sequence's space (premultiplied), as `target` shows it: in its space and trimmed.
void applyTrimPass(Image& img, const Sequence& s, FrameTime t, const TrimTarget& target);
// BT.2390's EETF on PQ code values: the brightest channel rolled off from `fromNits` to `toNits`, hue kept.
void rollOffPq(Image& img, double fromNits, double toNits);

namespace edit {
// Adds a target (a preset's id, or a space and peak of its own); its id is made unique.
Result addTrimTarget(Sequence& s, TrimTarget target);
// Removes one and every clip's trim for it.
Result removeTrimTarget(Sequence& s, const std::string& id);
// Sets controls of a clip's trim for a target (made if need be): at `at` (timeline frame) when the control has
// keyframes, else its value. Unknown controls fail.
Result setTrim(Sequence& s, Id clip, const std::string& target, const std::map<std::string, double>& values, FrameTime at);
// The clips' trims for a target back to none (all targets for "").
Result resetTrims(Sequence& s, const std::vector<Id>& clips, const std::string& target);
// A clip's trim for a target on other picture clips (in place of theirs).
Result copyTrim(Sequence& s, Id from, const std::vector<Id>& to, const std::string& target);
}  // namespace edit

}  // namespace montage
