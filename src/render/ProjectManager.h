// Montage — Project Manager (Premiere's Project Manager, Resolve's Media Management): a copy of the project in a new
// folder with the media it uses, for archiving or handing on. Collect copies each used file whole; Consolidate
// transcodes only the part of each video the sequences use, plus handles, and points the clips at the new files.
// Media nothing uses is left out, and sequences can be limited to some.
//
// For finishing: each file's used part in its own format, cut without coding again where its format allows (ProRes
// and DNxHR frame by frame, H.264 and HEVC a GOP at a time with only the GOPs at the ends coded again, image sequences
// their frames copied), or transcoded to movies or to image sequences (OpenEXR, DPX, TIFF) in each file's colour
// space, the working space or one chosen; handles in seconds, frames or a timecode duration, or the whole file; names
// from a pattern, the source's folders kept to a depth; shots nothing shows (clips disabled, or hidden under opaque
// clips above) left out and listed; the clips' grades, effects and speed baked in; and the choices kept as presets in
// the project, the user's library and a facility folder, the narrower overriding the wider.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct ConsolidateOptions {
    std::string folder;          // the project goes here, its media in folder/Media
    std::string name = "Project";  // the project file's name, without .montage
    std::vector<Id> sequences;   // the sequences to keep (and what they nest); empty = all
    bool trim = false;           // Consolidate: only the used parts of videos, transcoded
    double handles = 1.0;        // seconds kept either side of what is used, when trimming (handleUnit "seconds")
    std::string codec = "prores_ks";  // trimmed copies: prores_ks (ProRes 422 HQ, .mov) or libx264 (.mp4)
    bool keepUnused = false;     // keep media items nothing uses
    // What becomes of each used file: "" as `trim` says (copied whole, or its used part transcoded to `codec`);
    // "whole": copied whole; "trimmed": its used part in its own format, cut without coding again where it can be
    // (ProRes and DNxHR frame by frame; H.264 and HEVC a GOP at a time, only the GOPs at the ends coded again; an image
    // sequence's frames copied), other formats transcoded to ProRes 422 HQ; "movie": transcoded to `codec` (prores_ks,
    // dnxhd as DNxHR HQ, libx264, libx265 or ffv1; "" each video's own where it is one of those, else ProRes);
    // "frames": transcoded to image sequences in `frames`, its sound beside them as WAV.
    std::string output;
    std::string frames = "exr";  // exr (half float), dpx (10-bit) or tiff (16-bit)
    // The colour space transcodes are made in: "" each file's own (OpenEXR: its light), "working" the first kept
    // sequence's working space (OpenEXR: its light), or a ColorSpace.h id.
    std::string colorSpace;
    // The handles: "seconds" (`handles`), "frames" (`handleFrames`, each file's own), "timecode" (`handleTimecode`, a
    // duration as "00:00:00:12", at each file's rate), "none", or "whole" (all of a file however little of it is used).
    std::string handleUnit = "seconds";
    int handleFrames = 0;
    std::string handleTimecode;
    // The new files' names: "" each source's (trimmed copies with "_trim"); or a pattern of {sequence}, {shot} (010,
    // 020... by its first clip's place in that sequence's cut), {clip} (the media's name), {reel}, {tape}, {scene},
    // {take} (its metadata) and, for image sequences, {frame} (the number; without it, after the name: "name.0001.exr").
    // A file copied whole keeps its own name.
    std::string namePattern;
    int folderDepth = 0;         // this many of each source's folders kept above it in Media (0: none)
    // Clips nothing shows left out (ConsolidateResult::leftOut): disabled ones, and those hidden their whole length
    // under opaque clips above (enabled footage with no alpha, filling the frame untouched, blended normally).
    bool leaveOutHidden = false;
    // Each footage clip on a video track rendered as the sequence shows it (grade, effects, transform, speed) with the
    // handles, as ProRes 4444 with its alpha (or, for "frames", OpenEXR with alpha); the new project's clips then play
    // those files plainly. Sound is consolidated as `output` says.
    bool bake = false;
    bool trimSound = false;      // sound files: their used part (with handles) as WAV, rather than copied whole
    // A source with Content Credentials: each copy made from it that is not the file itself (trimmed, transcoded, baked)
    // signed with this certificate and key (PEM), the source's credentials its parent (media/ContentCredentials.h); a
    // manifest beside a file copied whole is copied with it. Empty: copies are not signed.
    std::string credentialCertificate, credentialKey;
    // A job taken up again after it was cut short (app/DialogJobs.h): `began` is told each file or folder in the
    // folder's Media before it is written, and those of the run before, given as `replace`, are this job's own, so
    // they are written again under the same names rather than beside themselves. A whole copy among them that is
    // already complete (the source's size and the same hash) is passed over instead; trimmed copies are made again.
    std::function<void(const std::string& file)> began;
    std::vector<std::string> replace;
};

struct ConsolidateResult {
    std::string projectPath;
    int copied = 0;    // files copied whole
    int passedOver = 0;  // a job taken up again: files of the run before found whole and not written again
    int trimmed = 0;   // videos transcoded (or cut) to their used part
    int64_t bytes = 0;  // written to the folder's Media
    std::vector<std::string> missing;  // used files that could not be found (left pointing where they were)
    int framesCopied = 0;  // trimmed copies' frames copied from their sources, not coded again
    int framesCoded = 0;   // and those coded
    int baked = 0;         // clips baked
    std::vector<std::string> leftOut;  // "<sequence>: <clip> (disabled)" / "(hidden under <clip>)"
};

// The media items the sequences use, directly or through nested sequences and multicam clips.
std::vector<Id> usedMedia(const Project& p, const std::vector<Id>& sequences);

// The clips of a sequence nothing shows (ConsolidateOptions::leaveOutHidden), by id, with why.
std::vector<std::pair<Id, std::string>> hiddenClips(const Project& p, const Sequence& s);

bool consolidateProject(const Project& p, const ConsolidateOptions& o, ConsolidateResult* result = nullptr,
                        const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                        std::string* error = nullptr);

// ---- Presets ---------------------------------------------------------------------------------------------------------

// The choices (not the folder, name or sequences) as a JSON object; and read back into `o`, only those it names.
std::string consolidateChoicesJson(const ConsolidateOptions& o);
bool applyConsolidateChoices(const std::string& json, ConsolidateOptions& o, std::string* error = nullptr);

// Presets by name, each a JSON object of some or all of the choices, at three levels: the facility's (a folder of
// <name>.json files, $MONTAGE_FACILITY_PRESETS), the user's library (another such folder) and the project's
// (Project::consolidatePresets). A preset's value for a choice is the narrowest level's that sets it.
enum class PresetLevel { Facility = 0, User = 1, Project = 2 };
const char* presetLevelName(PresetLevel level);  // "facility", "user", "project"
struct ConsolidatePresets {
    std::map<std::string, std::string> levels[3];  // by PresetLevel: name -> JSON
    std::vector<std::string> names() const;
};
// The facility's folder ($MONTAGE_FACILITY_PRESETS, "" when unset); the user's library (a folder in Montage's data
// folder); the presets in a folder; one written to it.
std::string facilityPresetFolder();
std::string userPresetFolder();
std::map<std::string, std::string> readPresetFolder(const std::string& folder);
bool writePreset(const std::string& folder, const std::string& name, const std::string& json, std::string* error = nullptr);
// The project's, the user's (from `userFolder`) and the facility's presets together.
ConsolidatePresets loadConsolidatePresets(const Project& p, const std::string& userFolder);
// The preset `name` over `base`: each level from the widest, so the narrowest one setting a choice gives it. `from`,
// when given, gets each choice's level (by its JSON key). False when no level has it.
bool resolvePreset(const ConsolidatePresets& presets, const std::string& name, ConsolidateOptions& base,
                   std::map<std::string, PresetLevel>* from = nullptr, std::string* error = nullptr);
// The compare view: the choices `level`'s preset sets differently from the levels wider than it (what they would give
// without it), each key with its value there, the wider value and the level that gives it ("" and the default's
// level name "default" when no wider level sets it).
struct PresetDifference {
    std::string key, value, wider, widerLevel;
};
std::vector<PresetDifference> comparePreset(const ConsolidatePresets& presets, const std::string& name, PresetLevel level);

}  // namespace montage
