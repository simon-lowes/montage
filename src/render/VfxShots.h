// Montage — VFX shots after the pull (render/VfxPull.h): each pulled shot kept with the sequence by name, the vendor's
// renders found in a returns folder by their names ("{shot}_comp_v003", frames or a movie) and brought in as versions
// (takes on the shot's clip, placed by their frame numbers so the frame numbered as the cut's first lands on the cut's
// first frame), checked against the pull, stepped up and down, with a status and a note; the shot list as CSV and
// OpenTimelineIO, and a compositing script (Nuke's .nk) for each shot that reads its plate and writes the next version.
#pragma once

#include <string>
#include <vector>

#include "VfxPull.h"
#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

enum VfxStatus { kVfxInProgress = 0, kVfxForReview = 1, kVfxApproved = 2, kVfxFinal = 3 };
const char* vfxStatusName(int status);  // "In Progress", "For Review", "Approved", "Final"

VfxShotRecord vfxShotRecord(const VfxShot& pulled);
// Keeps the pulled shots with the sequence: a shot pulled again (by name) keeps its versions, status and note.
void recordVfxShots(Sequence& s, const std::vector<VfxShot>& shots);
VfxShotRecord* findVfxShot(Sequence& s, const std::string& name);
const VfxShotRecord* findVfxShot(const Sequence& s, const std::string& name);
VfxShotRecord* vfxShotOfClip(Sequence& s, Id clip);
// The version the shot's clip shows (0 the plate), from what it plays, however it came to (a take picked by hand too).
int vfxShownVersion(const Sequence& s, const VfxShotRecord& r);
// Where the shot's compositing script goes in `folder` (its name made safe for a file).
std::string vfxScriptFile(const VfxShotRecord& r, const std::string& folder);
const VfxShotRecord* vfxShotOfClip(const Sequence& s, Id clip);

// A returned version's frames as a pattern ("<returns>/<v>/<v>.####.exr", v the pattern's name for it: "{shot}" the
// shot's, "v###" its number padded to as many digits, "" = "{shot}_comp_v###"); `format` exr, dpx or tiff.
std::string vfxVersionName(const std::string& pattern, const std::string& shot, int version);
std::string vfxVersionPath(const std::string& returns, const std::string& pattern, const VfxShotRecord& r, int version, const std::string& format);

// What is wrong with a version against the pull: frames missing or short of the pull's range, another size, and (for
// OpenEXR) chromaticities other than the plate's colour space's. `numbers`: the frame numbers found (frames; empty for
// a movie, whose length is `movieFrames`).
std::vector<std::string> vfxVersionProblems(const VfxShotRecord& r, const std::vector<int>& numbers, int width, int height,
                                            const std::string& firstFile, int movieFrames = -1);

struct VfxScan {
    std::vector<std::string> added;     // "<shot> v<n>"
    std::vector<std::string> problems;  // "<shot> v<n>: <problem>"
};
// Looks through `folder` (and the folders in it) for versions of the sequence's shots named by `pattern` ("" = the
// sequence's, else "{shot}_comp_v###"), as numbered frames (OpenEXR, DPX, TIFF, PNG) or movies; each new one becomes a
// media item and a take on its shot's clip. A version already there is checked again (its frames may have been
// written since). False with `error` when the folder cannot be read; finding nothing new is not an error.
bool scanVfxReturns(Project& p, Sequence& s, const std::string& folder, const std::string& pattern, VfxScan* out = nullptr,
                    std::string* error = nullptr);

namespace edit {

// Shows version `version` of the shot on `clip` (0: the plate; -1: the latest), the take it was brought in as (the clips
// linked to it, its sound, left as they are).
Result setVfxVersion(Project& p, Sequence& s, Id clip, int version);
// The next (+1) or previous (-1) version, stopping at the plate and the latest.
Result stepVfxVersion(Project& p, Sequence& s, Id clip, int step);
Result setVfxStatus(Sequence& s, const std::string& shot, int status, const std::string* note = nullptr);

}  // namespace edit

// The shot list: a row a shot (its clip's place in the cut, status, version shown, versions, note, frames, plate,
// colour space, and the shown version's problems), and the same as an OpenTimelineIO timeline of the shots' plates
// at their places in the cut, each with its VFX details in its metadata.
std::string vfxShotListCsv(const Sequence& s);
std::string vfxShotListOtio(const Sequence& s);

// A Nuke script for the shot: its plate read untouched (raw, its colour space noted) over its frame range and written,
// raw too, to `writePath` (frames as "####").
std::string nukeScript(const VfxShotRecord& r, const std::string& writePath);

}  // namespace montage
