// Montage — track groups, group comping and track colours.
//
// A group (Sequence::trackGroups, Track::group) holds tracks that are edited, mixed and comped together. Its switches
// say what reaches the other tracks when one of them changes:
//   editing     selecting a clip selects the clips on the other tracks over the same time; a cut, trim, move, delete
//               or fade on one is made on all of them (a trim on the clips whose edge is at the same frame);
//   mute        mute and solo;
//   volume      a fader move moves the others by the same number of decibels, so their offsets stay;
//   pan         a pan move moves the others by the same amount;
//   automation  an automation mode set on one is set on all, and a fader or pan held in a pass writes the others'
//               lanes by the same amount;
//   record      a recording onto one track records onto all of them, one input channel each;
//   comp        passes recorded together share a pass id and a take number (so a colour); giving a range of a take
//               to the comp on one track gives that range of that pass to every grouped track, crossfaded at the
//               same samples; auditions, ratings and Best Take act on the group.
// A group can be suspended (or all of them at once, Sequence::groupsSuspended): its tracks then act alone.
//
// Track colours (Track::color, "#rrggbb") are labels drawn on the header and, with Track::colorClips, on the
// track's clips that have no label of their own; they are saved with the project and carried in OpenTimelineIO.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

enum class GroupSwitch { Editing, Mute, Volume, Pan, Automation, Record, Comp };

const TrackGroup* findTrackGroup(const Sequence& s, Id group);
TrackGroup* findTrackGroup(Sequence& s, Id group);
const TrackGroup* findTrackGroup(const Sequence& s, const std::string& name);  // by name, ignoring case
// The group the track is in when it acts as one for `sw` (not suspended, the switch on), else nullptr.
const TrackGroup* activeGroup(const Sequence& s, const Track& t, GroupSwitch sw);
// The track, then the other tracks of its group that act with it for `sw` (picture tracks first, by index); just
// the track when it acts alone; empty for no such track.
std::vector<TrackRef> groupTracks(const Sequence& s, TrackRef track, GroupSwitch sw);
// Every track in a group, by index (picture tracks first).
std::vector<TrackRef> groupMembers(const Sequence& s, Id group);

// "#rrggbb" in lower case from "#rgb", "#rrggbb" or "rrggbb" (any case); "" for anything else.
std::string normalizeTrackColor(const std::string& color);
// A colour's red, green and blue, 0..1 (false for ""), and back (each clamped to 0..1, rounded to 8 bits).
bool trackColorRgb(const std::string& color, double& r, double& g, double& b);
std::string trackColorFromRgb(double r, double g, double b);
// A take's number: TakeLane::number, else the N of a "Take N" name, else 0.
int takeNumber(const TakeLane& lane);
// The colour a take is drawn in ("#rrggbb", from its number, so the same take is one colour on every grouped track;
// "" for an unnumbered take).
std::string takeColor(const TakeLane& lane);

namespace edit {

// The clips on the other tracks of `clip`'s group (editing switch) that play over any of its time.
std::vector<Id> groupMates(const Sequence& s, Id clip);
// The clips, their links, the group mates of all of those and the mates' links (no further: mates of mates are not
// followed, so a long clip on one track does not pull in a whole other track).
std::vector<Id> expandGroups(const Sequence& s, const std::vector<Id>& ids);
// The clips on the other tracks of `clip`'s group whose edge `edge` is at the same frame as its own.
std::vector<Id> groupEdgeMates(const Sequence& s, Id clip, Edge edge);

// Groups. createTrackGroup makes one of the tracks (taking them out of any other group); created[0] is its id. It
// fails for an empty name, a name another group has, or no tracks.
Result createTrackGroup(Project& p, Sequence& s, const std::string& name, const std::vector<TrackRef>& tracks);
// Puts the tracks in the group (0 takes them out of theirs).
Result setTrackGroup(Sequence& s, const std::vector<TrackRef>& tracks, Id group);
Result renameTrackGroup(Sequence& s, Id group, const std::string& name);
// The group goes; its tracks stay, in no group.
Result removeTrackGroup(Sequence& s, Id group);
// Sets the group's switches and suspension from `settings` (its id and name are kept).
Result setTrackGroupSwitches(Sequence& s, Id group, const TrackGroup& settings);
Result suspendTrackGroup(Sequence& s, Id group, bool suspended);

// The tracks' label colour ("" clears it); fails for a colour normalizeTrackColor does not read.
Result setTrackColor(Sequence& s, const std::vector<TrackRef>& tracks, const std::string& color);
Result setTrackColorClips(Sequence& s, const std::vector<TrackRef>& tracks, bool on);

// Mute and solo through the group (mute switch).
Result setTrackMuted(Sequence& s, TrackRef track, bool muted);
Result setTrackSoloed(Sequence& s, TrackRef track, bool soloed);
// The automation mode (core/Automation.h AutomationMode) of an audio track and, with the automation switch, its group.
Result setTrackAutomationMode(Sequence& s, TrackRef track, int mode);

// A cut at `frame` on each track, the tracks of the clips linked to the clips cut, and their groups' tracks (editing
// switch); the right halves of linked clips are linked to each other again. created: the right halves.
Result razorTogether(Project& p, Sequence& s, const std::vector<TrackRef>& tracks, FrameTime frame);

// Group comping. The lanes that come from the same pass as `lane` on the other tracks of `track`'s group (comp
// switch), with the track and lane itself first.
std::vector<std::pair<TrackRef, Id>> passLanes(const Sequence& s, TrackRef track, Id lane);
// compTake on the track and on each of passLanes' others over the same frames, so their crossfades fall on the same
// samples. A grouped track whose lane does not cover the range is left as it is. created: the comp clips.
Result compGroup(Project& p, Sequence& s, TrackRef track, Id lane, FrameTime from, FrameTime to,
                 double crossfade = 0.01);
// rateTakeLane on the lane and its pass on the grouped tracks.
Result rateGroupLane(Sequence& s, TrackRef track, Id lane, int rating);
// A loop recording's passes on several tracks at once (recorded together): media[t][k] is pass k on tracks[t]. Pass k
// is one pass id and one take number on all of them (the next number free on every track); the last whole pass is
// comped on each over [in, out). created: the lanes, track by track, then the comp clips.
Result addGroupLoopTakes(Project& p, Sequence& s, const std::vector<TrackRef>& tracks, const std::vector<std::vector<Id>>& media,
                         const std::vector<bool>& complete, FrameTime in, FrameTime out);
// Auditions through the group (comp switch): the take picked (or stepped to) on `clip` is picked on the auditions of
// the grouped tracks that start where it does and hold that take.
Result pickGroupTake(Project& p, Sequence& s, Id clip, int take);
Result cycleGroupTake(Project& p, Sequence& s, Id clip, int step);

}  // namespace edit
}  // namespace montage
