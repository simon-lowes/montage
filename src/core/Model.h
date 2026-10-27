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
    // A sound key's place between frames (0..1): it sits at t + sub, sample-accurate for volume and pan. Pictures are
    // made a frame at a time and see it at t + sub too.
    double sub = 0;
    double time() const { return double(t) + sub; }
    bool operator==(const Keyframe&) const = default;
};

// The handles key i actually uses (its own, or the automatic ones).
void keyHandles(const std::vector<Keyframe>& keys, size_t i, double& inDt, double& inDv, double& outDt, double& outDv);

// What an animation does after its last keyframe (After Effects' loopOut, Resolve 21's keyframe loop and ping pong).
enum class Repeat { Hold, Loop, PingPong, Offset };

struct Param {
    double value = 0;              // used when there are no keyframes
    std::vector<Keyframe> keys;    // sorted by t
    // After the last key: hold its value, play the keys again (Loop), back and forth (PingPong), or again carrying
    // on from where the last cycle ended (Offset: a value climbing by the same step each cycle). Needs two keys.
    Repeat repeat = Repeat::Hold;
    // An expression that gives the value at each frame from the keyed one (core/Expression.h); "" = none. Its time
    // counts from clip frame exprFrom (moved with the keys when the clip's head is trimmed or split off), and its noise
    // and random numbers come from exprSeed (kept by copies, so a split clip carries on where it was).
    std::string expr;
    FrameTime exprFrom = 0;
    uint32_t exprSeed = 0;

    Param() = default;
    Param(double v) : value(v) {}  // NOLINT(google-explicit-constructor)

    bool animated() const { return !keys.empty(); }
    // Whether the value can change over time: keys or an expression.
    bool varies() const { return !keys.empty() || !expr.empty(); }
    double at(FrameTime t) const;
    // At a fractional frame (sound is mixed between frames); an expression is worked out at the frame it falls in.
    double atTime(double t) const;
    double keyedAt(FrameTime t) const;  // the keys' (or the still) value, without the expression
    double keyedAtTime(double t) const;
    // Sets the value at t: adds/replaces a keyframe if animated, else the static value.
    void set(FrameTime t, double v);
    void addKey(FrameTime t, double v, Interp interp = Interp::Linear);
    // A key at a fractional frame (replacing one on the same frame); a sound key placed between frames.
    void addKeyAt(double time, double v, Interp interp = Interp::Linear);
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
    // The grade node it belongs to (GradeNode::key; core/GradeGraph.h), 0 = none: outside a clip's node graph, or a
    // clip without one.
    int node = 0;

    double p(const std::string& name, FrameTime t, double def = 0) const;
    double pAt(const std::string& name, double t, double def = 0) const;  // at a fractional frame (Param::atTime)
    std::string s(const std::string& name, const std::string& def = {}) const;
    bool empty() const { return type.empty(); }
    bool operator==(const Effect&) const;  // compares the object mask by value
};

// ---------------------------------------------------------------------------
// Media

enum class MediaKind { Video, Audio, Image, Sequence };

// Adaptive HDR metadata (SMPTE ST 2094-50, core/AdaptiveHdrMetadata.h): the picture's HDR reference white and, when
// given, how to tone map it to any display's headroom: gain curves for alternate renditions (an SDR one, a less bright
// HDR one) that a display blends between by its own headroom. Headrooms are log2 of light over reference white.
struct AdaptiveGainCurve {
    // How a colour's components are mixed before the curve: red, green, blue, the brightest, the dimmest and each
    // component itself, summing to 1 (the brightest alone by default: one gain for the whole colour, keeping its hue).
    double mix[6] = {0, 0, 0, 1, 0, 0};
    std::vector<double> x, y, m;  // control points: light (relative to reference white), log2 gain, slope
    bool pchip = true;            // slopes worked out from the points (monotone cubic), not given
    bool operator==(const AdaptiveGainCurve&) const = default;
};
struct AdaptiveHdr {
    double referenceWhite = 203;  // cd/m² (BT.2408's by default)
    bool toneMap = false;         // the gain curves below are given (else the display tone maps as it likes)
    bool computed = false;        // with toneMap: the standard's own curves for `baseline`, worked out rather than sent
    double baseline = 0;          // the picture's own headroom
    int chromaMode = 2;           // gain application primaries: 0 Rec.709, 1 P3-D65, 2 Rec.2020, 3 `chroma`
    double chroma[8] = {0.708, 0.292, 0.17, 0.797, 0.131, 0.046, 0.3127, 0.329};  // red, green, blue, white xy
    std::vector<double> alternate;          // each alternate rendition's headroom, ascending (at most four)
    std::vector<AdaptiveGainCurve> gains;   // its gain curve
    bool operator==(const AdaptiveHdr&) const = default;
};
// The metadata over frames [start, end): a sequence's (measured for its exports) or a media file's (its own frames).
struct AdaptiveHdrScene {
    FrameTime start = 0, end = 0;
    AdaptiveHdr md;
    std::string key;  // (a sequence's) a fingerprint of the frames measured, as Hdr10PlusScene's
    bool operator==(const AdaptiveHdrScene&) const = default;
};

// What a shot of the media shows, in words (render/FootageWords.h), for logging and search.
struct ShotDescription {
    double start = 0, end = 0;  // media seconds
    std::string text;
    bool operator==(const ShotDescription&) const = default;
};

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
    // Channels in each audio stream, in file order (cameras writing MXF often carry a mono stream per channel).
    // Clip::channels counts across them. Empty: one stream of `channels`.
    std::vector<int> audioStreams;
    // How new clips take its channels (core/AudioChannels.h): "" the main stream mixed to stereo, "mono" a clip per
    // channel, "pairs" a stereo clip per pair (Premiere's Modify > Audio Channels on a project item).
    std::string audioChannelMode;
    std::string videoCodec;
    std::string audioCodec;
    Id sequenceId = 0;       // for MediaKind::Sequence (compound clip)
    std::string bin;         // bin path, "/" between nested bins ("Interviews/Day 1"), "" = the project root
    std::string colorSpace;     // detected from the file's colour tags (ColorSpace.h id), "" = Rec.709
    std::string colorOverride;  // Interpret Colour: the space to read it as, "" = as detected
    // SDR in an HDR sequence (render/ColorSpace.h ToneMapping id): "" = at HDR reference white, "bt2446a" or "bt2446c"
    std::string sdrExpand;
    double maxCll = 0, maxFall = 0;  // HDR light levels the file states (CTA-861.3), nits; 0 = none stated
    // Adaptive HDR metadata the video carries (ST 2094-50, render/AdaptiveHdr.h), scene by scene in its own frames;
    // null = none. Shared: undo snapshots copy the pointer.
    std::shared_ptr<const std::vector<AdaptiveHdrScene>> adaptiveHdr;
    bool gainMap = false;  // an SDR picture with a gain map (media/GainMap.h), read as its HDR rendition
    // 360° footage: "equirect" or "vr180" (the half in front; from the file's spherical metadata, or set by hand),
    // "" = a flat picture.
    std::string projection;
    // Ambisonic sound (core/Ambisonics.h): its order (1 = first-order AmbiX, four channels W Y Z X), from the file's
    // spatial audio metadata (MP4 SA3D) or set by hand; 0 = channels for speakers.
    int ambisonic = 0;
    // Stereoscopic 3D footage: how its two eyes are packed ("sbs", "sbs_half", "tb", "tb_half"; core/Interpretation.h; "mv"
    // for MV-HEVC spatial video, each eye a view of its own),
    // from the file's stereo metadata or Interpret Footage; "" = a flat picture. Its size is one eye's, as shown.
    std::string stereo;
    double timecode = -1;       // start timecode in seconds (for multicam sync), -1 = none
    // Timecode read from its sound (media/LtcTimecode.h): the channel carrying LTC, which new clips leave out (-1 =
    // none); the start the LTC gives (seconds, -1 = not read); and, while `timecode` is that, the file's own (-1 = none).
    int ltcChannel = -1;
    double ltcTimecode = -1;
    double fileTimecode = -1;
    // Speech-to-text of the media's audio (shared: undo snapshots copy the pointer).
    std::shared_ptr<const Transcript> transcript;
    // What the footage shows, for search by description (shared like the transcript).
    std::shared_ptr<const VisualIndex> visual;
    // What its sound is, for search by description (CLAP embeddings of 10-second windows; shared like the visual index).
    std::shared_ptr<const VisualIndex> sounds;
    // The faces in it, for finding people (shared like the visual index).
    std::shared_ptr<const FaceIndex> faces;
    // Logging (core/MediaLog.h): what the editor notes about the media to find it again.
    int rating = 0;                                // -1 rejected, 0 unrated, 1-5 stars
    int label = 0;                                 // colour label (core/MediaLog.h labelName), 0 = none
    std::vector<std::string> keywords;
    std::map<std::string, std::string> metadata;  // scene, shot, take, camera, description, comment, ...
    std::vector<ShotDescription> shots;            // each shot described (Describe Shots), in order
    std::string created;                           // when it was recorded (ISO 8601, from the file), "" = unknown
    // A subclip: a saved range of another media item. Clips made from it use
    // that item (with this range), so only bins, logging and search see it.
    Id subclipOf = 0;
    double subclipIn = 0, subclipOut = 0;  // seconds of the parent media
    // Where it sits on its bin's storyboard (core/Storyboard.h: Premiere's Freeform view), in thumbnail cells.
    struct BoardSpot {
        double x = 0, y = 0;
        bool placed = false;
        bool operator==(const BoardSpot&) const = default;
    } board;
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

// A sequence marker (t in timeline frames) or a clip marker (Clip::markers: t in the clip's source frames).
struct Marker {
    FrameTime t = 0;
    FrameTime duration = 0;
    std::string name;
    std::string comment;
    int color = 0;
    bool chapter = false;  // a chapter marker: a chapter in exported MP4 / MOV / MKV files and YouTube's list
    std::string drawing;   // a reviewer's drawing over the frame (core/ReviewPage.h): strokes as JSON, in 0..1 of the picture
    FrameTime drawnAt = -1;  // the frame it was drawn on, when the marker had to sit elsewhere (-1: t)
    std::string reviewId;  // the review note it came from (the notes imported again bring it up to date)
    std::string reviewStatusAt;  // when the note's status was last set (the later status wins on import)
    FrameTime drawingFrame() const { return drawnAt >= 0 ? drawnAt : t; }
    double sub = 0;  // between frames (0..1): it sits at t + sub (a sync point in the sound)
    bool operator==(const Marker&) const = default;
};

// An ADR cue (core/Adr.h): a line to re-record to picture, from `start` to `end` (timeline frames, end exclusive).
struct AdrCue {
    Id id = 0;
    std::string name;       // the cue number ("JD101")
    std::string character;  // who says it
    std::string line;       // what is said
    std::string note;       // why it is replaced (noise, performance, a new line)
    FrameTime start = 0;
    FrameTime end = 0;
    int status = 0;  // AdrStatus
    Id clip = 0;     // the clip holding its takes (an audition), 0 before the first take
    bool operator==(const AdrCue&) const = default;
};

// One take of an audition (Final Cut's auditions, Resolve's take selector): a piece of media, starting `offset`
// sequence frames from the clip's own in-point, so trims and splits of the clip carry its takes along. Its sound plays
// `channels` (Clip::channels) when it is the pick.
struct Take {
    Id mediaId = 0;
    double offset = 0;
    std::string name;
    std::vector<int> channels;
    bool operator==(const Take&) const = default;
};

// A clip's node graph (Resolve's nodes; core/GradeGraph.h): correctors and mixers joined by links. A corrector holds
// the effects marked with its key (Effect::node), or a shared node's; its window, qualifier, mix and key settings are
// a "grade_node" effect of its own. Links run from a node's output (picture and key) to an input: a corrector's
// picture (0) or key (1) input, a mixer's inputs in order. Node 0 is the clip's picture (as a link's `from`) and the
// graph's output (as a link's `to`). Empty: the effects run in order, as a stack.
struct GradeNode {
    int key = 0;                     // 1, 2, 3...: unique in the clip, what Effect::node names
    std::string kind = "corrector";  // "corrector", "parallel", "layer" or "key_mixer"
    std::string label;
    bool enabled = true;
    Id shared = 0;                   // a corrector whose effects are a shared node's (Sequence::sharedNodes)
    std::vector<std::string> blend;  // a layer mixer's blend mode for each input after the first ("normal"...)
    double x = 0, y = 0;             // where the node editor shows it
    bool operator==(const GradeNode&) const = default;
};
struct GradeLink {
    int from = 0;      // the node it comes from (0: the clip's picture)
    int to = 0;        // the node it goes to (0: the graph's output)
    int input = 0;     // which of its inputs: a corrector's 0 picture, 1 key; a mixer's in order
    bool key = false;  // from the node's key output rather than its picture
    bool operator==(const GradeLink&) const = default;
};
struct GradeGraph {
    std::vector<GradeNode> nodes;
    std::vector<GradeLink> links;
    bool empty() const { return nodes.empty(); }
    bool operator==(const GradeGraph&) const = default;
};
// A trim pass's deliverable (render/TrimPasses.h): a display the master is mapped to, its space and peak, each shot
// trimmed for it (Clip::trims).
struct TrimTarget {
    std::string id;          // unique in its sequence
    std::string name;        // shown
    std::string colorSpace;  // a display space (ColorSpace.h id)
    double peakNits = 100;   // its display's peak (a PQ target is rolled off to it)
    // An SDR target made from HDR (render/ColorSpace.h ToneMapping id): "" = Montage's roll-off, "bt2446a" or "bt2446c"
    std::string toneMapping;
    bool operator==(const TrimTarget&) const = default;
};
// A grade node's effects shared by clips (Resolve's shared nodes): edited once, seen on each clip using it. Its id is
// the owner of its effects (edit::effectChain).
struct SharedNode {
    Id id = 0;
    std::string name;
    std::vector<Effect> effects;
    bool operator==(const SharedNode&) const = default;
};

// A grade version (core/GradeVersions.h): its name and, while it is not the one shown, its colour effects.
struct GradeVersion {
    std::string name;
    std::vector<Effect> effects;
    std::vector<int> anchors;  // where each went in the stack: how many other effects came before it
    GradeGraph grade;          // its node graph
    bool operator==(const GradeVersion&) const = default;
};

// A clip animation preset (core/ClipAnimation.h): its kind ("" = none) and how long it lasts, in seconds.
struct ClipAnimation {
    std::string type;
    double seconds = 0.5;
    bool operator==(const ClipAnimation&) const = default;
};

struct Clip {
    Id id = 0;
    Id mediaId = 0;              // 0 for generator clips (titles, colour mattes...)
    std::string name;
    FrameTime start = 0;         // timeline position
    FrameTime duration = 1;      // timeline length
    // Sound placed between frames: how much of the clip's first and last frame is silent (each 0..1, in frames), so
    // its sound starts at start + head and ends at end() - tail, to the sample (edit::editSoundSamples).
    double head = 0, tail = 0;
    double sourceIn = 0;         // in-point into the media, in sequence frames
    double speed = 1.0;          // > 0
    bool reverse = false;
    bool enabled = true;
    Id linkGroup = 0;            // clips sharing a non-zero link group act as one
    int colorLabel = 0;
    // Audio role (Final Cut's roles, Premiere's clip types): "Dialogue", "Music", "Effects" or the editor's own; ""
    // = none. A sequence can mute a role, and stems can be split by role.
    std::string role;
    std::string blendMode = "normal";
    Effect generator;            // non-empty type => clip is generated, not decoded
    Effect motion;               // fixed "transform" attributes (video clips)
    Effect audio;                // fixed "volume" attributes (audio clips)
    Effect timing;               // fixed "time" attributes: Time Remapping speed (%) and frame sampling
    std::vector<Effect> effects; // filter stack, applied in order
    std::string unrendered;      // after Render and Replace: the clip as it was (JSON), for Restore
    int angle = 0;               // multicam video clip: the angle shown (video track of the multicam sequence)
    int audioAngle = -1;         // multicam audio clip: the audio track played, -1 = all of them
    // Source channels played (Premiere's Modify > Audio Channels): indexes into every channel of the file's audio
    // streams, in order (MediaItem::audioStreams). Empty = the main audio stream mixed to stereo. One channel plays
    // in the centre, two as left and right; with more, the first, third... go left and the others right.
    std::vector<int> channels;
    // A multi-layer OpenEXR's layer (AOV) the clip shows (media/Exr.h): "" the main picture.
    std::string layer;

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
    // Clip markers (Premiere's and Final Cut's): on the clip's source, in source frames like sourceIn, so they stay
    // on the same moment of the media through moves, trims and splits. Shown where that moment is in the clip.
    std::vector<Marker> markers;
    // Where marker m falls on the timeline, or -1 when the clip does not show that moment.
    FrameTime markerFrame(const Marker& m) const;
    // An audition (core/EditOps.h pickTake): every take, the clip's own included as takes[take]; empty for a plain clip.
    std::vector<Take> takes;
    int take = 0;
    // Animation presets (core/ClipAnimation.h): an entrance, an exit and a repeating motion on top of the transform.
    ClipAnimation animIn, animOut, animLoop;
    // Grade versions (core/GradeVersions.h): empty for a clip with one grade; the shown one's effects are in `effects`.
    std::vector<GradeVersion> gradeVersions;
    int gradeVersion = 0;
    Id colorGroup = 0;  // the colour group it is graded with (Sequence::colorGroups), 0 = none
    GradeGraph grade;   // its node graph (core/GradeGraph.h); empty: its effects run as a stack
    // A graphic's own settings (core/GraphicsTemplate.h): values for the fields published by the sequence it plays,
    // by field id, as text ("#rrggbb", "1"); a field without one shows the template's own setting.
    std::map<std::string, std::string> fields;
    // Trim passes (render/TrimPasses.h): its trim for each of its sequence's targets, by TrimTarget::id (a "trim"
    // effect: lift, gamma, gain, saturation, highlights, mid_detail; keyframable). None: the target as mapped.
    std::map<std::string, Effect> trims;
    bool operator==(const Clip&) const = default;
};

// A setting a graphic's designer publishes (Premiere's Essential Graphics properties, Final Cut's published parameters;
// core/GraphicsTemplate.h): a parameter of one of the sequence's clips that each clip playing the sequence can set
// for itself in the Inspector.
struct TemplateField {
    std::string id;     // unique in the sequence
    std::string label;  // what the Inspector shows
    std::string kind;   // "text", "color", "number", "toggle", "choice" or "media"
    Id clip = 0;        // the clip in the sequence it sets
    Id effect = 0;      // the effect on it: 0 its generator (a title...), else one of its effects; unused for media
    std::string param;  // the parameter (a colour's name without .r/.g/.b) or text string; "" for media
    bool operator==(const TemplateField&) const = default;
};

// A colour group (Resolve's groups, core/ColorGroups.h): clips graded together. Its pre-clip grade runs on each member
// before the clip's own effects (to match the shots), its post-clip grade after them (the group's look). Keyframes in
// either are timed from each clip's start.
// One scene of HDR10+ dynamic metadata (SMPTE ST 2094-40): what an HDR10+ display needs to tone map it, measured on
// the PQ picture in linear light (render/Hdr10Plus.h).
struct Hdr10PlusScene {
    FrameTime start = 0, end = 0;   // sequence frames [start, end)
    double maxScl[3] = {0, 0, 0};   // cd/m²: the brightest red, green and blue anywhere in the scene
    double average = 0;             // cd/m²: the mean over the scene of each pixel's brightest channel (maxRGB)
    double percentiles[7] = {};     // cd/m²: maxRGB at 1, 25, 50, 75, 90, 95 and 99.98 % of the scene's pixels
    std::string key;                // a fingerprint of the frames measured, to tell when the cut has changed under it
    bool operator==(const Hdr10PlusScene&) const = default;
};

struct ColorGroup {
    Id id = 0;      // also the owner of its pre-clip effects (edit::effectChain)
    Id postId = 0;  // the owner of its post-clip effects
    std::string name;
    std::vector<Effect> pre, post;
    // Groups made by Group Shots (render/ShotGroups.h): an angle group (its shots from one camera position) inside a
    // scene group (`parent`), whose grades wrap the angle's. A group made by hand has no kind. Running Group Shots
    // again remakes the groups it made, except locked ones.
    Id parent = 0;
    std::string kind;  // "" by hand, "angle", "scene"
    bool locked = false;
    bool operator==(const ColorGroup&) const = default;
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
// the speakers, nearer the middle spreads it over all of them; z its height in
// immersive layouts), how far apart
// its left and right channels are (1: as wide as the front pair, 0: one
// point), and how much goes to the LFE. The default puts a stereo track on
// the front left and right speakers, as in stereo.
struct SurroundPan {
    double x = 0;
    double y = 1;
    double width = 1;
    double lfeDb = -100;  // -100 = none
    double z = 0;         // height in immersive layouts: 0 at the ear, 1 overhead
    bool object = false;  // an audio object in immersive masters (render/Adm.h), not part of the bed
    bool operator==(const SurroundPan&) const = default;
};

// An aux send (Pro Tools', Fairlight's and Premiere's sends): a copy of an audio track's sound fed to a bus, an effects
// return such as a shared reverb, at its own level. Post-fader (the usual) it follows the track's fader, pan and mute;
// pre-fader it is taken after the inserts, before the fader, for a level of its own (a foldback or a stem).
struct Send {
    Id bus = 0;
    double levelDb = 0;
    bool preFader = false;
    bool muted = false;
    Param levelAuto;  // send level automation (dB), keyed in timeline frames and played in the track's mode
    Param levelTrim;  // its trim (dB on top of the level; core/Automation.h), written in the track's trim modes
    bool operator==(const Send&) const = default;
};

// A take lane (Pro Tools' playlists, Logic's take folders, Nuendo's lanes; core/TakeLanes.h): one pass of a loop
// recording, or any take kept with a track, its sound starting at timeline frame `start`. The track plays a comp of
// its lanes as ordinary clips.
struct TakeLane {
    Id id = 0;
    std::string name;
    Id mediaId = 0;
    FrameTime start = 0;  // where its first sample plays
    int rating = 0;       // 0 none, 1..5
    bool operator==(const TakeLane&) const = default;
};

// A track version (Pro Tools' playlists, Resolve's track layers kept aside; core/TakeLanes.h): a whole other edit
// of a track's clips, switched to in one step.
struct TrackVersion {
    std::string name;
    std::vector<Clip> clips;  // empty for the one shown: its clips are the track's
    std::vector<Transition> transitions;
    bool operator==(const TrackVersion&) const = default;
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
    // Fader automation (core/Automation.h): volume (dB) and pan lanes keyed in timeline frames, and the
    // AutomationMode (0 Off, 1 Read, 2 Write, 3 Latch, 4 Touch, 5 Trim Write, 6 Trim Latch, 7 Trim Touch).
    Param volumeAuto, panAuto;
    // The fader's trim: an offset in dB played on top of the volume lane, written in the trim modes; its static value
    // without keys.
    Param volumeTrim;
    // Surround position lanes (x, y, z as in SurroundPan), keyed in timeline frames and played in the same mode: a
    // sound, or an ADM object, moving round the room.
    Param surroundXAuto, surroundYAuto, surroundZAuto;
    // Mute automation: muted where the lane is 0.5 or more, holding each key's value until the next (a switch).
    Param muteAuto;
    int automation = 1;
    std::vector<Send> sends;  // audio: aux sends to buses
    // Track folder (Resolve's Fairlight folders): tracks of a kind with the same folder, next to each other, show under
    // one header that can collapse them and mute, solo or hide them together. "" = none.
    std::string folder;
    // Take lanes under the track (shown or folded away) and its versions: empty for a track with one version, else
    // versions[version] is the one shown, whose clips are the track's own.
    std::vector<TakeLane> lanes;
    bool lanesShown = true;
    std::vector<TrackVersion> versions;
    int version = 0;
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
    // Its fader, pan and mute automation (core/Automation.h), keyed in timeline frames, and its AutomationMode.
    Param volumeAuto, panAuto, muteAuto;
    Param volumeTrim;  // its fader's trim, as a track's
    int automation = 1;
    bool operator==(const Bus&) const = default;
};

struct Sequence {
    Id id = 0;
    std::string name = "Sequence 1";
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    int sampleRate = 48000;
    std::string audioLayout = "stereo";  // "stereo", "5.1" or "7.1" (core/Surround.h)
    std::string filmGauge;  // "" (none: timecode), or a gauge cut on film is counted in (core/FilmCount.h)
    std::vector<Track> videoTracks;   // [0] = V1 (bottom-most)
    std::vector<Track> audioTracks;   // [0] = A1
    std::vector<Marker> markers;
    std::vector<CaptionTrack> captionTracks;  // subtitles, drawn above the video tracks
    std::vector<Bus> buses;                   // audio submixes
    std::vector<Effect> masterEffects;        // on the final mix, before the master fader
    double masterVolumeDb = 0;
    bool multicam = false;              // a multicam clip's sequence: video tracks are angles (Multicam.h)
    std::vector<std::string> collapsedFolders;  // track folders shown collapsed: "V/name" or "A/name"
    std::map<std::string, double> folderGains;  // audio track folders' faders (a VCA over their tracks), dB, by "A/name"
    std::vector<std::string> mutedRoles;  // audio roles not heard (clip roles, see Clip::role)
    std::vector<ColorGroup> colorGroups;   // clips graded together (core/ColorGroups.h)
    std::vector<SharedNode> sharedNodes;   // grade nodes shared by its clips (core/GradeGraph.h)
    std::vector<TrimTarget> trimTargets;   // HDR and SDR deliverables the grade is trimmed for (render/TrimPasses.h)
    std::vector<AdrCue> adrCues;           // lines to re-record, in time order (core/Adr.h)
    std::string colorSpace = "rec709";  // delivery space (ColorSpace.h id), and the working space when workingSpace is ""
    // Scene-referred grading: the space clips are graded and composited in (a scene-referred ColorSpace.h id: ACEScct,
    // ACEScg, a camera log), each viewer and deliverable reached through an output transform; "" = the delivery space.
    std::string workingSpace;
    std::string outputTransform;  // "" (Montage's filmic) or "aces2" (ACES 2.0 through OpenColorIO, where it has it)
    double hdrPeakNits = 1000;          // mastering peak for HDR spaces
    double hdrMaxCll = 0, hdrMaxFall = 0;  // measured light levels (Analyse HDR Light Levels), nits; 0 = not measured
    std::vector<Hdr10PlusScene> hdr10Plus;  // HDR10+ metadata from the same analysis (PQ only), in time order
    std::vector<AdaptiveHdrScene> adaptiveHdr;  // adaptive HDR metadata measured for exports (PQ, HLG), in time order
    std::vector<TemplateField> fields;      // published for the clips that play it: a graphics template's settings
    bool spherical = false;             // a 360° sequence (equirectangular): exports say so to players and YouTube
    bool vr180 = false;                 // with spherical: half the sphere in front (VR180), as stereo VR180 cameras shoot
    // Stereoscopic 3D: the sequence is made for two eyes (its size is one eye's); each is rendered from stereo footage's
    // own eye, clips' Stereo 3D effect placing them in depth; exports pack the eyes (render/Stereo.h).
    bool stereo3d = false;
    FrameTime inPoint = -1;   // In / Out marks; both frames are included, -1 = unset
    FrameTime outPoint = -1;
    FrameTime playhead = 0;

    FrameTime duration() const;
    double fpsValue() const { return fps.toDouble(); }
    bool operator==(const Sequence&) const = default;
};

// The screenplay the project is cut from (core/ScriptCut.h readScreenplay): its text as written and, for each of its
// lines (in the order readScreenplay gives them), the takes that read it, best first, found in the transcripts.
struct ScriptReading {
    Id media = 0;
    double start = 0, end = 0;  // media seconds, first to last word of the line
    double coverage = 0;        // share of the line's words said (0..1)
    int extraWords = 0;         // words said in that range that are not the line's
    double score = 0;
    bool operator==(const ScriptReading&) const = default;
};
struct ProjectScript {
    std::string name;    // the file it was read from, or what it was called ("" for none)
    std::string format;  // "fountain", "fdx" or "text"
    std::string text;    // "" when the project has no script
    std::vector<std::vector<ScriptReading>> readings;  // per line; empty until matched
    bool operator==(const ProjectScript&) const = default;
};

struct Project {
    std::string name = "Untitled";
    std::vector<MediaItem> media;
    std::vector<Sequence> sequences;
    std::vector<std::string> bins;  // bin paths ("Interviews/Day 1"), including empty ones
    std::vector<SmartBin> smartBins;
    std::vector<Person> people;  // the people found in the footage (FaceIndex.h), with their names
    std::vector<std::string> fillerWords;  // the editor's own filler words or phrases (core/TranscriptEdit.h)
    std::vector<std::string> vocabulary;   // names and terms speech-to-text should expect (core/TranscriptCorrect.h)
    std::vector<std::string> soundCaptions;  // the sounds SDH captions describe (core/SdhCaptions.h); empty = the ready-made list
    std::vector<std::string> watchFolders;  // folders whose arriving media files are imported (app/EditorState)
    ProjectScript script;  // the screenplay and its lined coverage (core/ScriptCut.h)
    // The version this project and a copy of it both started from (core/ProjectMerge.h), as project file JSON; null =
    // none. A copy made for someone keeps it, so the two can be merged later. Shared: undo snapshots copy the pointer.
    std::shared_ptr<const std::string> mergeBase;
    // After a merge that gave the other copy's ids new ones here: their ids (as in their copy and the merge base) to
    // this project's, so the next merge of that copy matches them up.
    std::map<Id, Id> mergeIds;
    Id activeSequence = 0;
    Id nextId = 1;
    // Whether its media's stereo packing (MediaItem::stereo) has been read: false for a project saved before Montage
    // read it, until media/Decoder.h checkStereoMedia looks (not saved).
    bool stereoChecked = true;

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
