// Montage — faces followed through a clip, for Redact Faces: news, documentary and reality editors must hide the
// faces of people who have not agreed to be shown (Resolve and Premiere do it with a mask drawn and tracked by hand
// for each face; YouTube Studio's Blur Faces finds them). The clip is analysed once: YuNet finds the faces in every
// frame, they are linked from frame to frame into tracks by where they are and who they are, and SFace's identity of
// each track tells whose face it is, so the tracks of one person are grouped and can be left showing or hidden
// together. A face the detector loses for a few frames (turned away, blurred by motion) stays covered: between two
// sightings its box moves from one to the other, and at either end of a track it is held for the chosen frames.
#pragma once

#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "Faces.h"

namespace montage {

struct Project;
struct Sequence;
struct Clip;
struct Effect;

struct FaceTrack {
    struct Box {
        double time = 0;                   // media seconds
        float x = 0, y = 0, w = 0, h = 0;  // fractions of the frame
        bool operator==(const Box&) const = default;
    };
    int id = 0;
    std::vector<Box> boxes;       // by time
    std::vector<float> identity;  // unit length (SFace), the mean of its faces'; empty if none could be taken
    float score = 0;              // the detector's best for it
    double seconds(double step) const { return boxes.empty() ? 0 : boxes.back().time - boxes.front().time + step; }
};

struct FaceTracks {
    double fps = 25;            // the media's
    double start = 0, end = 0;  // the media seconds analysed
    double step = 0;            // seconds between the frames analysed (0: a still)
    std::vector<FaceTrack> tracks;
    const FaceTrack* track(int id) const;
};

std::string faceTracksToString(const FaceTracks& t);
bool faceTracksFromString(const std::string& text, FaceTracks& out);

// A face seen in one analysed frame.
struct FaceSighting {
    double time = 0;
    float x = 0, y = 0, w = 0, h = 0;  // fractions
    float score = 0;
    std::vector<float> identity;       // empty when not taken for this frame
};
// Links the sightings of successive frames into tracks: a face joins the track whose last box is nearest it (allowing
// for how long ago that was, up to `maxGap` seconds) and of a like size, unless both identities are known and are
// different people (below SFace's 0.36). Every find makes or joins a track, even one seen once.
std::vector<FaceTrack> linkFaceTracks(const std::vector<std::vector<FaceSighting>>& frames, double maxGap);

// Analyses media seconds `start`..`end` of a video (every frame up to 30 per second) or a still. `progress` (0..1)
// may return false to stop.
bool trackFaces(const std::string& path, double start, double end, FaceTracks& out, const std::function<bool(double)>& progress = {},
                std::string* error = nullptr);

// The faces at media time `t`: each track's box there (moved between its sightings when they are at most
// 2 x `holdFrames` apart, held for `holdFrames` beyond its ends), with the track's id.
struct TrackedFace {
    int track = 0;
    FaceBox box;
};
std::vector<TrackedFace> trackedFacesAt(const FaceTracks& t, double time, int holdFrames);

// The tracks grouped by person (identities within `threshold`, cosine, of a group's mean), most seen first; tracks
// with no identity each stand alone.
struct FaceGroup {
    std::vector<int> tracks;
    std::vector<float> identity;
    double seconds = 0;          // on screen, all tracks together
    double bestTime = 0;         // their largest sighting, for a picture of them
    FaceTrack::Box best;
    int person = 0;              // the project's person they are (core/FaceIndex.h), or 0
};
std::vector<FaceGroup> groupFaceTracks(const FaceTracks& t, float threshold = 0.42f);
// Names the groups after the project's people whose faces match (People search, media/Faces.h).
void matchProjectPeople(const Project& p, std::vector<FaceGroup>& groups, float threshold = 0.42f);
// A project person's identity: the mean of their faces'; empty if they have none.
std::vector<float> personIdentity(const Project& p, int person);

// The media seconds a clip shows of its video (0..0 for a still).
void clipMediaSpan(const Sequence& s, const Clip& c, bool still, double& start, double& end);
// A clip's Redact Faces effect, added first among its effects when `create` (so it covers the faces before anything
// moves the picture).
Effect* redactFacesEffectOf(Project& p, Clip& c, bool create);

// "1,4,7" and back: the tracks left showing.
std::string trackIdsToString(const std::set<int>& ids);
std::set<int> trackIdsFromString(const std::string& text);

}  // namespace montage
