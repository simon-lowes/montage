// Montage — the lens profile database: lensfun's (lensfun.github.io; the data is CC BY-SA 3.0), downloaded on first use
// at a pinned revision, or the one a lensfun package installed.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/LensProfile.h"
#include "core/Model.h"
#include "media/ModelFiles.h"

namespace montage {

const ModelPack& lensDatabasePack();
// Where a database is looked for, in order: MONTAGE_LENS_DB alone when set; else the downloaded pack's folder, then
// lensfun's updates and installed folders.
std::vector<std::string> lensDatabaseFolders();
// The first of them holding one, read once (null when none does); read again after reloadLensDatabase.
std::shared_ptr<const lens::Database> lensDatabase();
void reloadLensDatabase();

// What a Lens Correction corrects for: the lens, the camera it was on (its sensor's size), the focal length and the
// aperture (0: the lens's shortest and widest).
struct LensChoice {
    const lens::Lens* lens = nullptr;
    const lens::Camera* camera = nullptr;
    double focal = 0;     // 0: the lens's shortest
    double aperture = -1;  // -1: the lens's widest; 0: not known (no vignetting correction)
};
// The camera `m` was recorded with (by its Camera Model), and its lens when it has a fixed one.
LensChoice suggestLens(const lens::Database& db, const MediaItem& m);
// The choice a Lens Correction holds, found in `db` by its names (null lens when the database lacks it).
LensChoice lensChoiceOf(const lens::Database& db, const Effect& e);
// A Lens Correction for a choice: a new one with `id`, or `existing` with the lens changed and its other settings kept.
Effect lensCorrectionEffect(const LensChoice& c, Id id, const Effect* existing = nullptr);
// Puts it first on each picture clip of `clips` (so the other effects see the corrected picture), in place of a Lens
// Correction there; how many clips.
int applyLensCorrection(Project& p, Sequence& s, const std::vector<Id>& clips, const LensChoice& c);

}  // namespace montage
