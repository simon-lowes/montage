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
// The clip's group and the groups around it (a scene group around an angle group), outermost first.
std::vector<const ColorGroup*> colorGroupsOf(const Sequence& s, const Clip& c);
ColorGroup* findColorGroup(Sequence& s, Id group);
const ColorGroup* findColorGroup(const Sequence& s, Id group);
// What runs on the clip's picture, in order: its groups' pre-clip effects (the outer group's first), its own, its
// groups' post-clip effects (the inner group's first).
std::vector<const Effect*> gradeChain(const Sequence& s, const Clip& c);
// The clips in a group, in timeline order; with `nested`, those of the groups inside it too.
std::vector<Id> colorGroupMembers(const Sequence& s, Id group, bool nested = false);

// What Group Shots found (render/ShotGroups.h): angle groups, each in a scene (-1 none), and the scenes' names.
struct ShotGrouping {
    struct Angle {
        std::vector<Id> clips;
        int scene = -1;
    };
    std::vector<Angle> angles;
    std::vector<std::string> scenes;  // names ("" numbered)
    std::vector<Id> sceneGroups;      // each scene's existing group (the scene around a group kept as it was; 0 a new one)
};

namespace edit {

// A new group of the video clips `clips` (leaving any group they were in), named `name` (or "Group N").
Result makeColorGroup(Project& p, Sequence& s, const std::vector<Id>& clips, const std::string& name, Id* created = nullptr);
// The video clips joined to `group` (leaving any other).
Result addToColorGroup(Sequence& s, const std::vector<Id>& clips, Id group);
// The clips out of their groups.
Result removeFromColorGroup(Sequence& s, const std::vector<Id>& clips);
Result renameColorGroup(Sequence& s, Id group, const std::string& name);
// The group and its grades gone; its clips keep their own grades (the groups inside it are left on their own).
Result deleteColorGroup(Sequence& s, Id group);
// The clips moved out of their group into a new group beside it (in the same scene) with a copy of its grades.
Result splitColorGroup(Project& p, Sequence& s, const std::vector<Id>& clips, Id* created = nullptr);
// The groups `others` joined into `group`: their clips and inner groups move to it, and they go.
Result joinColorGroups(Sequence& s, Id group, const std::vector<Id>& others);
// A locked group is kept as it is when Group Shots runs again.
Result setColorGroupLocked(Sequence& s, Id group, bool locked);
// The group put inside `parent` (a scene group; 0 none).
Result setColorGroupParent(Sequence& s, Id group, Id parent);
// Group Shots' groups made: the unlocked groups an earlier run made are remade (one that keeps most of its shots keeps
// its id, name and grades), scene groups around their angle groups; groups made by hand and locked ones stay.
Result applyShotGrouping(Project& p, Sequence& s, const ShotGrouping& g);

}  // namespace edit

}  // namespace montage
