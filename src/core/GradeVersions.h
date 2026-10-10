// Montage — grade versions (Resolve's local versions): several named grades
// on one clip, to show a client options or try a look without losing the
// last one. A grade is the clip's colour effects (the effect list's Color
// group: Color Correct, Curves, Hue Curves, Colour Warper, LUTs...); the
// current version's live in the effect stack, the others are kept aside,
// and switching swaps them in place, leaving every other effect alone.
#pragma once

#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

// Whether an effect belongs to the grade.
bool isGradeEffect(const Effect& e);

namespace edit {
// A new version, a copy of the current grade (or none with `empty`), switched
// to. The first call also makes "Version 1" of the grade the clip already had.
Result addGradeVersion(Project& p, Sequence& s, Id clipId, const std::string& name = {}, bool empty = false);
// Shows version `index` (and keeps the current grade as its version).
Result switchGradeVersion(Sequence& s, Id clipId, int index);
// Removes a version (not the only one); removing the current one shows the one before it.
Result removeGradeVersion(Sequence& s, Id clipId, int index);
Result renameGradeVersion(Sequence& s, Id clipId, int index, const std::string& name);
}  // namespace edit

}  // namespace montage
