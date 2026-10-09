// Montage — colour groups (Resolve's groups, Premiere's and Final Cut's adjustment-by-group workarounds): shots that
// belong together (a scene, a camera, an interview) are graded as one. Each group has a pre-clip grade that runs on
// every member before the clip's own effects, to bring the shots to a common start, and a post-clip grade after
// them, for the group's look; each clip keeps its own grade in between. Groups belong to a sequence.
#pragma once

#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

const ColorGroup* colorGroupOf(const Sequence& s, const Clip& c);
ColorGroup* findColorGroup(Sequence& s, Id group);
const ColorGroup* findColorGroup(const Sequence& s, Id group);
// What runs on the clip's picture, in order: its group's pre-clip effects, its own, its group's post-clip effects.
std::vector<const Effect*> gradeChain(const Sequence& s, const Clip& c);
// The clips in a group, in timeline order.
std::vector<Id> colorGroupMembers(const Sequence& s, Id group);

namespace edit {

// A new group of the video clips `clips` (leaving any group they were in), named `name` (or "Group N").
Result makeColorGroup(Project& p, Sequence& s, const std::vector<Id>& clips, const std::string& name, Id* created = nullptr);
// The video clips joined to `group` (leaving any other).
Result addToColorGroup(Sequence& s, const std::vector<Id>& clips, Id group);
// The clips out of their groups.
Result removeFromColorGroup(Sequence& s, const std::vector<Id>& clips);
Result renameColorGroup(Sequence& s, Id group, const std::string& name);
// The group and its grades gone; its clips keep their own grades.
Result deleteColorGroup(Sequence& s, Id group);

}  // namespace edit

}  // namespace montage
