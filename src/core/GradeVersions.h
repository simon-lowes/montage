// Montage — grade versions (Resolve's local versions): several named grades
// on one clip, to show a client options or try a look without losing the
// last one. A grade is the clip's colour effects (the effect list's Color
// group: Color Correct, Curves, Hue Curves, Colour Warper, LUTs...); the
// current version's live in the effect stack, the others are kept aside,
// and switching swaps them in place, leaving every other effect alone.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

// Whether an effect belongs to the grade (the Color filters, a Chart Match aside: it stays under every version).
bool isGradeEffect(const Effect& e);

// A clip's grade as it stands: its grade effects, where each sits among the others (how many come before it), and
// its node graph.
void gradeOf(const Clip& c, std::vector<Effect>& effects, std::vector<int>& anchors);

namespace edit {
// Replaces the clip's grade (its grade effects and node graph, its versions kept aside untouched) with `effects`
// (fresh ids), each where `anchors` says or where the grade it replaces was, and `graph`, its shared nodes renamed by
// `sharedIds` (the sequence's nodes they became, adoptSharedNodes); one the sequence lacks is dropped.
Result replaceGrade(Project& p, Sequence& s, Id clipId, const std::vector<Effect>& effects, const std::vector<int>& anchors,
                    const GradeGraph& graph, const std::map<Id, Id>& sharedIds);
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
