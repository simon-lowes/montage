// Montage — merging two copies of a project from the version both started from (a three-way merge, as software teams
// merge code): the work done on another computer, an assistant's bins and syncing, or a colourist's grades brought
// into this copy. Everything in the project file is compared base to mine and base to theirs: things with ids (media,
// sequences, tracks, clips, effects, transitions, buses, colour groups...) by their ids, keyframes, captions and
// markers by their times, lists of names (bins, keywords) as sets, and everything else field by field. A change made
// on one side only is taken, the same change made on both sides is taken once, and different changes to one field
// are a conflict (as is something removed on one side and changed on the other). Ids both copies made on their own
// are given new ids on their side, every reference following. Clips from the two sides that would overlap on a track
// are a conflict too: until it is settled their side's clip goes on a new track.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "Model.h"

namespace montage {

enum class MergeSide { Mine, Theirs, Both };
const char* mergeSideName(MergeSide s);  // "Mine", "Theirs", "Both"

struct MergeChange {
    enum Kind { Added, Removed, Changed };
    Kind kind = Changed;
    MergeSide side = MergeSide::Mine;  // who made it (Both: the same change on both sides)
    std::string path;                  // where in the project file, by ids ("sequences/3/videoTracks/5/clips/40")
    std::string what;                  // what it is, by name ("Sequence 1 › V1 › Interview.mov")
    std::string field;                 // the field changed ("duration"), "" for the whole thing
    Id sequence = 0, clip = 0, media = 0;  // where to go to see it (0: not in one)
};
const char* mergeChangeKindName(MergeChange::Kind k);  // "Added", "Removed", "Changed"

struct MergeConflict {
    enum Kind { Field, Removed, Overlap, Moved };  // Moved: to a different track on each side
    Kind kind = Field;
    std::string key;     // names the conflict in `choices` (stable from one merge of the same projects to the next)
    std::string what, field;
    std::string mine, theirs;  // each side's value, as text ("removed" for one taken out)
    bool canBoth = false;      // Both can be chosen (overlapping clips: theirs on a new track)
    MergeSide choice = MergeSide::Mine;  // what the merge took
    Id sequence = 0, clip = 0, media = 0;
};

struct MergeResult {
    bool ok = false;
    std::string error;
    Project project;  // the merge; its merge base is now theirs (what the two have in common from here on)
    std::vector<MergeChange> changes;
    std::vector<MergeConflict> conflicts;
    std::map<Id, Id> renumbered;  // their ids given new ones: old to new
    int newTracks = 0;            // tracks made for their clips that overlapped
};

// The version two copies started from, and how ids given new ones by an earlier merge of the two match up.
struct MergeBase {
    Project base;
    std::map<Id, Id> theirIds;  // their ids as mine knows them (a merge that renumbered them recorded it)
    bool baseInTheirIds = false;  // the base uses their ids too (it is mine's record of their copy)
};

// Merges `theirs` into `mine` from `base`. `choices` settles conflicts by key (unsettled: Mine, or Both for clips
// that overlap). Merging theirs into mine and mine into theirs give the same project where nothing conflicts. The
// result records theirs as its merge base, with the ids of theirs it renumbered.
MergeResult mergeProjects(const MergeBase& base, const Project& mine, const Project& theirs,
                          const std::map<std::string, MergeSide>& choices = {});
MergeResult mergeProjects(const Project& base, const Project& mine, const Project& theirs,
                          const std::map<std::string, MergeSide>& choices = {});

// The version `p` started from, kept when it was copied or merged (false when it has none or it cannot be read).
bool mergeBaseOf(const Project& p, Project& base);
// Gives `copy` the merge base `base` (its own merge base left out), its ids as they are.
void setMergeBase(Project& copy, const Project& base);
// The merge base for `mine` and `theirs`: the later of the two copies' records (the one with more ids made; mine's
// when they are level), with the ids an earlier merge renumbered. False when neither has one.
bool commonMergeBase(const Project& mine, const Project& theirs, MergeBase& base);

}  // namespace montage
