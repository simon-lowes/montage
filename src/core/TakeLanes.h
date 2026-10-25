// Montage — takes on lanes (Pro Tools' loop record and playlist comping, Logic's take folders, Nuendo's and Cubase's
// lanes): each pass of a loop recording kept as a lane under its audio track, a comp built by giving ranges of the
// lanes to the track, and whole other versions of a track's edit kept and switched in one step.
//
// The comp is the track's own clips, cut from the lanes' media at the lanes' places, so it plays, fades, exports and
// interchanges as any clips do; the joins between them are short equal-power crossfades (10 ms unless set otherwise).
#pragma once

#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

constexpr double kCompCrossfadeSeconds = 0.01;

// The sample ranges of a loop recording's passes: after `preroll` samples (heard once, before the first pass) each
// pass is `length` samples (In to Out; pass k starts at preroll + k * length, rounded, so passes of a length between
// whole samples do not drift); the last, cut short, counts when it holds at least `minPartial` samples (0: never).
struct LoopPass {
    int64_t first = 0, count = 0;  // samples of the recording
    bool complete = true;
};
std::vector<LoopPass> loopPasses(int64_t recorded, int64_t preroll, double length, int64_t minPartial = 0);

// The word errors (words left out, put in or said differently: lower case, punctuation aside) between what a take
// says and its line.
int lineWordErrors(const std::string& said, const std::string& line);

// A take to choose from, and what is known of it (media/TakeChoice measures it).
struct TakeCandidate {
    Id lane = 0;    // a take lane, or
    Id clip = 0;    // a clip's audition take
    int take = -1;  // (that clip's take; -1 for the clip itself, unauditioned)
    std::string name;
    bool transcribed = false;  // its media has a transcript
    std::string said;          // the words in it over the range
    double peakDb = -200, rmsDb = -200;
    double clippedSeconds = 0;  // at or near full scale
    int rating = 0;
};
// The take to suggest for `line` (empty: by sound alone): fewest word errors where takes are transcribed, then no
// clipping, then a usable level (−40 dBFS RMS or more), then the higher rating, then the later take. -1 for none.
// `why` gets a line about the choice.
int suggestTake(const std::vector<TakeCandidate>& takes, const std::string& line, std::string* why = nullptr);

// What a track's lanes show: its take lanes, then each of its clips' other audition takes (Final Cut's auditions,
// ADR takes) at the clip's place.
struct LaneView {
    Id lane = 0;    // a take lane, or 0
    Id clip = 0;    // an audition's clip, or 0
    int take = -1;  // that clip's take
    std::string name;
    Id mediaId = 0;
    FrameTime start = 0, end = 0;  // where it plays on the timeline (its media's length from start)
    int rating = 0;
};
std::vector<LaneView> laneViews(const Project& p, const Sequence& s, TrackRef track);

// The lane a comp clip comes from (same media, at the lane's place), 0 for none.
Id compLaneOf(const Track& t, const Clip& c);

namespace edit {

const TakeLane* findTakeLane(const Track& t, Id lane);
// A take lane on an audio track: `media` starting at `start`; created[0] is its id. Named "Take N" when unnamed.
Result addTakeLane(Project& p, Sequence& s, TrackRef track, Id media, FrameTime start, const std::string& name = {});
Result renameTakeLane(Sequence& s, TrackRef track, Id lane, const std::string& name);
Result rateTakeLane(Sequence& s, TrackRef track, Id lane, int rating);  // 0..5
// Takes the lane away (its comp clips stay).
Result removeTakeLane(Sequence& s, TrackRef track, Id lane);
Result showLanes(Sequence& s, TrackRef track, bool shown);

// A loop recording's passes as take lanes starting at `in` (`complete` says which are whole passes), the last whole
// one given to the comp over [in, out); created: the lanes, then the comp clip (when one was made).
Result addLoopTakes(Project& p, Sequence& s, TrackRef track, const std::vector<Id>& media, const std::vector<bool>& complete,
                    FrameTime in, FrameTime out);
// Gives timeline [from, to) of a lane to the comp: the track's clips there are replaced by the lane's sound, with a
// `crossfade`-second equal-power crossfade at each join with a clip either side. Refused outside the lane.
Result compTake(Project& p, Sequence& s, TrackRef track, Id lane, FrameTime from, FrameTime to,
                double crossfade = kCompCrossfadeSeconds);
// Moves the join at timeline frame `at` (where one comp clip ends and the next begins) by `delta` frames, both sides
// together; its crossfade goes with it.
Result moveCompJoin(Project& p, Sequence& s, TrackRef track, FrameTime at, FrameTime delta);
// The crossfade at the join at `at`: `seconds` long, centred on the join (0 removes it).
Result setCompCrossfade(Project& p, Sequence& s, TrackRef track, FrameTime at, double seconds);

// Track versions: the track's clips kept as a version and a new one shown, a copy of them or empty; created[0] is
// unused. The first call makes the track's present edit version 1 ("Version 1" unless named).
Result newTrackVersion(Project& p, Sequence& s, TrackRef track, const std::string& name = {}, bool copy = true);
// Shows version `index`, keeping the shown one's clips with it.
Result switchTrackVersion(Sequence& s, TrackRef track, int index);
Result renameTrackVersion(Sequence& s, TrackRef track, int index, const std::string& name);
// Takes a version away (not the shown one).
Result deleteTrackVersion(Sequence& s, TrackRef track, int index);

}  // namespace edit
}  // namespace montage
