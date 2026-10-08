// Montage — project data model.
//
// The model is plain value types: a Project can be copied cheaply enough to
// snapshot it for undo, and every edit is a pure function over it
// (see EditOps.h). Timeline positions are integer frames in the owning
// sequence's frame rate; source in-points use the same unit so that
// "sourceIn / fps" gives seconds into the media file.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Captions.h"
#include "ObjectMask.h"
#include "Transcript.h"
#include "FaceIndex.h"
#include "VisualIndex.h"

namespace montage {

using FrameTime = int64_t;
using Id = uint64_t;

struct Rational {
    int num = 30;
    int den = 1;
    double toDouble() const { return den ? double(num) / double(den) : 0.0; }
    bool valid() const { return num > 0 && den > 0; }
    bool operator==(const Rational&) const = default;
};

// ---------------------------------------------------------------------------
// Keyframed parameters

enum class Interp { Linear, Hold, Smooth, Bezier };

struct Keyframe {
    FrameTime t = 0;          // relative to the start of the owning clip
    double v = 0;
    Interp interp = Interp::Linear;  // interpolation towards the next key
    // Bezier handles, relative to the key in frames and value: the one coming in
    // (dt <= 0, used when the key before is Bezier) and the one going out (dt >= 0,
    // used when this key is). Both zero means automatic: smooth through the key,
    // flat where it turns and at the ends.
    double inDt = 0, inDv = 0, outDt = 0, outDv = 0;
    bool operator==(const Keyframe&) const = default;
};

// The handles key i actually uses (its own, or the automatic ones).
void keyHandles(const std::vector<Keyframe>& keys, size_t i, double& inDt, double& inDv, double& outDt, double& outDv);

struct Param {
    double value = 0;              // used when there are no keyframes
    std::vector<Keyframe> keys;    // sorted by t

    Param() = default;
    Param(double v) : value(v) {}  // NOLINT(google-explicit-constructor)

    bool animated() const { return !keys.empty(); }
    double at(FrameTime t) const;
    // Sets the value at t: adds/replaces a keyframe if animated, else the static value.
    void set(FrameTime t, double v);
    void addKey(FrameTime t, double v, Interp interp = Interp::Linear);
    bool removeKey(FrameTime t);
    const Keyframe* keyAt(FrameTime t) const;
    bool operator==(const Param&) const = default;
};

// ---------------------------------------------------------------------------
// Effects (also used for generators, transitions parameters and the fixed
// "motion" / "audio" attributes every clip carries).

struct Effect {
    Id id = 0;
    std::string type;
    bool enabled = true;
    std::map<std::string, Param> params;
    std::map<std::string, std::string> strings;
    // The object picked for an "Object" mask (shared: undo snapshots copy the pointer).
    std::shared_ptr<const ObjectMask> object;

    double p(const std::string& name, FrameTime t, double def = 0) const;
    std::string s(const std::string& name, const std::string& def = {}) const;
    bool empty() const { return type.empty(); }
    bool operator==(const Effect&) const;  // compares the object mask by value
};

// ---------------------------------------------------------------------------
// Media

enum class MediaKind { Video, Audio, Image, Sequence };

struct MediaItem {
    Id id = 0;
    MediaKind kind = MediaKind::Video;
    std::string name;
    std::string path;        // absolute path on disk (empty for nested sequences)
    std::string proxyPath;   // optional low-resolution proxy
    double duration = 0;     // seconds; 0 for stills (infinite)
    int width = 0;
    int height = 0;
    Rational fps{0, 1};
    bool hasVideo = false;
    bool hasAudio = false;
    int sampleRate = 0;
    int channels = 0;
    std::string videoCodec;
    std::string audioCodec;
    Id sequenceId = 0;       // for MediaKind::Sequence (compound clip)
    std::string bin;         // bin path, "/" between nested bins ("Interviews/Day 1"), "" = the project root
    std::string colorSpace;     // detected from the file's colour tags (ColorSpace.h id), "" = Rec.709
    std::string colorOverride;  // Interpret Colour: the space to read it as, "" = as detected
    double timecode = -1;       // start timecode in seconds (for multicam sync), -1 = none
    // Speech-to-text of the media's audio (shared: undo snapshots copy the pointer).
    std::shared_ptr<const Transcript> transcript;
    // What the footage shows, for search by description (shared like the transcript).
    std::shared_ptr<const VisualIndex> visual;
    // The faces in it, for finding people (shared like the visual index).
    std::shared_ptr<const FaceIndex> faces;
    // Logging (core/MediaLog.h): what the editor notes about the media to find it again.
    int rating = 0;                                // -1 rejected, 0 unrated, 1-5 stars
    int label = 0;                                 // colour label (core/MediaLog.h labelName), 0 = none
    std::vector<std::string> keywords;
    std::map<std::string, std::string> metadata;  // scene, shot, take, camera, description, comment, ...
    std::string created;                           // when it was recorded (ISO 8601, from the file), "" = unknown
    // A subclip: a saved range of another media item. Clips made from it use
    // that item (with this range), so only bins, logging and search see it.
    Id subclipOf = 0;
    double subclipIn = 0, subclipOut = 0;  // seconds of the parent media
    bool operator==(const MediaItem&) const = default;
};

// A saved search: the media matching its rules (core/MediaLog.h).
struct SmartRule {
    std::string field;  // a mediaFields() key, or "any" for any text
    std::string op;     // contains, !contains, is, !is, starts, empty, !empty, >, >=, <, <=, includes, !includes
    std::string value;
    bool operator==(const SmartRule&) const = default;
};

struct SmartBin {
    Id id = 0;
    std::string name;
    bool matchAll = true;  // all rules must match, or any of them
    std::vector<SmartRule> rules;
    bool operator==(const SmartBin&) const = default;
};

// ---------------------------------------------------------------------------
// Timeline

enum class TrackKind { Video, Audio };

struct Clip {
    Id id = 0;
    Id mediaId = 0;              // 0 for generator clips (titles, colour mattes...)
    std::string name;
    FrameTime start = 0;         // timeline position
    FrameTime duration = 1;      // timeline length
    double sourceIn = 0;         // in-point into the media, in sequence frames
    double speed = 1.0;          // > 0
    bool reverse = false;
    bool enabled = true;
    Id linkGroup = 0;            // clips sharing a non-zero link group act as one
    int colorLabel = 0;
    std::string blendMode = "normal";
    Effect generator;            // non-empty type => clip is generated, not decoded
    Effect motion;               // fixed "transform" attributes (video clips)
    Effect audio;                // fixed "volume" attributes (audio clips)
    Effect timing;               // fixed "time" attributes: Time Remapping speed (%) and frame sampling
    std::vector<Effect> effects; // filter stack, applied in order
    std::string unrendered;      // after Render and Replace: the clip as it was (JSON), for Restore
    int angle = 0;               // multicam video clip: the angle shown (video track of the multicam sequence)
    int audioAngle = -1;         // multicam audio clip: the audio track played, -1 = all of them

    FrameTime end() const { return start + duration; }
    bool contains(FrameTime t) const { return t >= start && t < end(); }
    bool isGenerator() const { return !generator.empty(); }
    // Source position, in sequence frames, displayed at timeline frame t.
    double sourceFrameAt(FrameTime t) const;
    // The same at a fractional clip-local time (audio is sampled between frames).
    double sourceAt(double local) const;
    // Number of source frames consumed by the clip.
    double sourceExtent() const { return sourceOffset(double(duration)); }
    // Time Remapping: the speed curve (timing "speed", % of `speed`) is keyframed.
    // Reversed clips play at their constant speed.
    bool ramped() const;
    // Source frames per timeline frame at clip-local time `local`.
    double speedAt(double local) const;
    // Source frames consumed over clip-local [0, local) (the speed integrated; negative before 0).
    double sourceOffset(double local) const;
    // The clip-local time showing source position `source` (sequence frames).
    double localForSource(double source) const;
    bool operator==(const Clip&) const = default;
};

struct Transition {
    Id id = 0;
    std::string type = "cross_dissolve";
    Id clipA = 0;               // outgoing clip (0 = fade in from nothing)
    Id clipB = 0;               // incoming clip (0 = fade out to nothing)
    FrameTime duration = 15;    // centred on the edit point
    Effect params;              // type-specific parameters (e.g. wipe softness)
    bool operator==(const Transition&) const = default;
};

// Where a track or bus sits in a surround mix (core/Surround.h): its position
// (x left to right, y back to front, both -1..1; the edge of the circle is at
// the speakers, nearer the middle spreads it over all of them), how far apart
// its left and right channels are (1: as wide as the front pair, 0: one
// point), and how much goes to the LFE. The default puts a stereo track on
// the front left and right speakers, as in stereo.
struct SurroundPan {
    double x = 0;
    double y = 1;
    double width = 1;
    double lfeDb = -100;  // -100 = none
    bool operator==(const SurroundPan&) const = default;
};

struct Track {
    Id id = 0;
    TrackKind kind = TrackKind::Video;
    std::string name;
    std::vector<Clip> clips;           // sorted by start, never overlapping
    std::vector<Transition> transitions;
    bool muted = false;   // audio mute / video track output off
    bool solo = false;
    bool locked = false;
    bool syncLock = true;
    double volumeDb = 0;  // audio track fader
    double pan = 0;       // audio track pan -1..1
    int height = 0;       // UI hint, 0 = default
    std::vector<Effect> effects;  // audio track inserts, before the fader (keyframes in timeline frames)
    Id output = 0;                // audio: the bus the track feeds, 0 = master
    SurroundPan surround;         // audio, in 5.1 and 7.1 sequences
    bool operator==(const Track&) const = default;
};

// An audio bus (submix): tracks routed to it are summed, run through its
// effects, fader and pan, and go to the master.
struct Bus {
    Id id = 0;
    std::string name = "Bus";
    std::vector<Effect> effects;
    double volumeDb = 0;
    double pan = 0;
    bool muted = false;
    SurroundPan surround;
    bool operator==(const Bus&) const = default;
};

struct Marker {
    FrameTime t = 0;
    FrameTime duration = 0;
    std::string name;
    std::string comment;
    int color = 0;
    bool operator==(const Marker&) const = default;
};

struct Sequence {
    Id id = 0;
    std::string name = "Sequence 1";
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    int sampleRate = 48000;
    std::string audioLayout = "stereo";  // "stereo", "5.1" or "7.1" (core/Surround.h)
    std::vector<Track> videoTracks;   // [0] = V1 (bottom-most)
    std::vector<Track> audioTracks;   // [0] = A1
    std::vector<Marker> markers;
    std::vector<CaptionTrack> captionTracks;  // subtitles, drawn above the video tracks
    std::vector<Bus> buses;                   // audio submixes
    std::vector<Effect> masterEffects;        // on the final mix, before the master fader
    double masterVolumeDb = 0;
    bool multicam = false;              // a multicam clip's sequence: video tracks are angles (Multicam.h)
    std::string colorSpace = "rec709";  // working and delivery space (ColorSpace.h id)
    double hdrPeakNits = 1000;          // mastering peak for HDR spaces
    FrameTime inPoint = -1;   // In / Out marks; both frames are included, -1 = unset
    FrameTime outPoint = -1;
    FrameTime playhead = 0;

    FrameTime duration() const;
    double fpsValue() const { return fps.toDouble(); }
    bool operator==(const Sequence&) const = default;
};

struct Project {
    std::string name = "Untitled";
    std::vector<MediaItem> media;
    std::vector<Sequence> sequences;
    std::vector<std::string> bins;  // bin paths ("Interviews/Day 1"), including empty ones
    std::vector<SmartBin> smartBins;
    std::vector<Person> people;  // the people found in the footage (FaceIndex.h), with their names
    Id activeSequence = 0;
    Id nextId = 1;

    Id newId() { return nextId++; }
    MediaItem* findMedia(Id id);
    const MediaItem* findMedia(Id id) const;
    Sequence* findSequence(Id id);
    const Sequence* findSequence(Id id) const;
    Sequence* active() { return findSequence(activeSequence); }
    const Sequence* active() const { return findSequence(activeSequence); }
    bool operator==(const Project&) const = default;
};

// Track addressing helpers.
struct TrackRef {
    TrackKind kind = TrackKind::Video;
    int index = 0;
    bool operator==(const TrackRef&) const = default;
    auto operator<=>(const TrackRef&) const = default;
};

Track* trackAt(Sequence& s, TrackRef r);
const Track* trackAt(const Sequence& s, TrackRef r);
std::vector<TrackRef> allTracks(const Sequence& s);

// Factory helpers producing fully initialised objects.
Sequence makeSequence(Project& p, const std::string& name, int w, int h, Rational fps,
                      int videoTracks = 3, int audioTracks = 3);
Track makeTrack(Project& p, TrackKind kind, const std::string& name);
// Creates a clip referencing `media`, with fixed attributes initialised.
Clip makeClip(Project& p, const MediaItem& media, TrackKind kind, const Sequence& seq);
Clip makeGeneratorClip(Project& p, const std::string& generatorType, FrameTime duration);
Project makeDefaultProject();

// Duration of a media item expressed in frames of `seq` (or a very large
// number for stills / generators).
FrameTime mediaFrames(const MediaItem& m, const Sequence& seq);
constexpr FrameTime kInfiniteFrames = FrameTime(1) << 40;

}  // namespace montage
