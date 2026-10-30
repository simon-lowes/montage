// Montage — immersive masters as ADM BWF: a BW64 file (ITU-R BS.2088) carrying the Audio Definition Model (BS.2076)
// in its axml chunk and the channel assignment in its chna chunk, the interchange format of Dolby Atmos and the EBU's
// renderer (DaVinci Resolve's Fairlight and Pro Tools import and export it; Premiere and Final Cut do not).
//
// The bed is the mix of every track that is not an object, in the sequence's layout (stereo, 5.1, 7.1 or an immersive
// one with overhead speakers), named by the common definitions of BS.2094 so any ADM tool knows the speakers (7.1.2,
// which those definitions lack in Dolby's form, has a pack of its own made of their channels). Each audio track marked
// as an object (SurroundPan::object) and going straight to the speakers (a track sent to a bus is heard through the
// bus, so it stays in the bed) becomes an object of its own: its sound after its fader, as one channel, at its
// panner's position in the ADM's cartesian coordinates (its angle and distance by BS.2127's polar to cartesian mapping,
// its height as Z), so a renderer puts it where the mix does; its LFE send stays in the bed. Solo and mute count as
// they do when playing. 48 kHz, 24 bits.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "core/Surround.h"

namespace montage {

struct AdmSettings {
    std::string title;
    bool inOut = false;  // only In to Out (both included)
};

struct AdmResult {
    std::string path;
    int bedChannels = 0;
    int objects = 0;
    int64_t samples = 0;  // per channel, at 48 kHz
    std::string bedPack;  // the bed's audioPackFormat ID
};

// The audio tracks a master would write as objects, in order: those marked as objects, not sent to a bus, and heard
// (not muted, and soloed when any track is).
std::vector<int> admObjectTracks(const Sequence& s);

// The bed's pack for a layout (BS.2094 common definitions, or Montage's own for 7.1.2) and each speaker's channel
// format, in the layout's channel order.
std::string admBedPack(const std::string& layout);
std::vector<std::string> admBedChannels(const std::string& layout);

// A point in ADM's cartesian coordinates: X from -1 (left) to 1 (right), Y from -1 (behind) to 1 (in front), Z from 0
// (at the ear) to 1 (overhead).
struct AdmPoint {
    double x = 0, y = 1, z = 0;
};

// Where a layout's speaker is in them: its angle by BS.2127's polar to cartesian mapping, overhead speakers at Z 1.
AdmPoint admSpeakerPoint(const Speaker& sp);

// The sound of an immersive master, mixed a stretch at a time as exportAdmBwf writes it (and the IAB masters of
// render/Iab.h): the bed, every track but the objects in the sequence's layout (stereo when it has none of its own;
// not ambisonics), and each object track alone after its fader as one channel; and where each object is.
class ImmersiveMix {
public:
    ImmersiveMix();
    ~ImmersiveMix();
    // `rate`: samples a second along the sequence's timeline (48000; a DCP of a 23.976 sequence, which plays at 24,
    // mixes at 47952).
    bool open(const Project& p, const Sequence& s, int rate, std::string* error = nullptr);
    const std::string& layout() const;
    int bedChannels() const;
    bool overhead() const;  // the layout has speakers above
    const std::vector<int>& objectTracks() const;  // admObjectTracks
    // `n` samples from `start` (at the rate, from the sequence's start): the bed interleaved into `bed`, bedChannels()
    // a sample, and object k's channel into objects[k].
    void mix(int64_t start, int n, float* bed, float* const* objects);
    // Where object k is at sequence frame `frame` (between frames as the panner glides).
    AdmPoint objectAt(size_t k, double frame) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// One programme's ADM description (BS.2076) as Montage writes and reads it: a bed of speakers in one of its layouts
// and objects that move, each on a channel of the sound that carries it (a BW64 file's, an IMF ADM or S-ADM track
// file's). Times are samples at `rate` from the programme's start.
struct AdmBlock {
    int64_t from = 0, to = 0;
    AdmPoint at;        // where the object is at the block's end, gliding there from where the block before left it
    bool jump = false;  // ... or there from the block's start
};
struct AdmScene {
    std::string title;
    int rate = 48000;
    int64_t samples = 0;
    int channels = 0;    // channels of the sound
    std::string layout;  // the bed's (core/Surround.h audioLayouts()), "" without one
    std::string bedPack;
    std::vector<int> bed;  // the channel carrying each of the layout's speakers, in its order (-1: none)
    struct Object {
        std::string name;
        int channel = -1;
        std::vector<AdmBlock> blocks;  // in order, back to back
    };
    std::vector<Object> objects;
    // Object k's position at `sample`, as a renderer glides it.
    AdmPoint objectAt(size_t k, double sample) const;
};

// What exportAdmBwf writes for the sequence from `first` to `end` (frames): the bed's speakers on channels 0..N-1,
// then one channel for each object track (admObjectTracks).
bool admSceneOf(const Project& p, const Sequence& s, FrameTime first, FrameTime end, const std::string& title, AdmScene& out,
                std::string* error = nullptr);

// The ADM document of a scene (an axml chunk's ebuCoreMain), and each channel's chna entry (track UID, track format,
// pack).
std::string admDocument(const AdmScene& scene);
struct AdmTrack {
    int channel = 0;  // from 0
    std::string uid, trackFormat, pack;
};
std::vector<AdmTrack> admTracks(const AdmScene& scene);

// The scene as S-ADM (ITU-R BS.2125-1): a 'full' frame for each stretch of `length` samples from `start` (the frames
// counted from 1 by `index`), each holding the whole programme, the channels' transport track format, and each object's
// position over the frame (its blocks cut at the frame's edges, each ending where the object is then), times from the
// programme's start.
std::string sadmFrame(const AdmScene& scene, int64_t index, int64_t start, int64_t length, const std::string& flowId);

// Reads an ADM document (ebuCoreMain, audioFormatExtended) or an S-ADM frame (BS.2125) into `out`: its first programme's
// bed (speakers named by BS.2094's common definitions or their speakerLabel, matched to the smallest layout that has
// them all) and objects (cartesian or polar positions), on the channels `tracks` gives each track UID (a chna chunk's,
// an MXF file's ADM channel mapping; an S-ADM frame's transport track format when empty). An S-ADM frame's blocks are
// appended to those `out` already holds (merging a block that only stays put), so a stream of frames reads into one
// scene. False, with the reason, when it holds no programme Montage reads.
bool parseAdm(const std::string& xml, AdmScene& out, const std::vector<AdmTrack>& tracks = {}, std::string* error = nullptr,
              std::vector<std::string>* warnings = nullptr);

// A scene's bed from the channels carrying BS.2051's speakers (labels such as "M+030"): the smallest layout that has
// them all, or the most of them (the others are noted in `warnings`).
void admSetBed(AdmScene& scene, const std::map<std::string, int>& speakers, std::vector<std::string>* warnings = nullptr);
// Adds a block after an object's others: one that only keeps it where it is lengthens the last.
void admAppendBlock(std::vector<AdmBlock>& blocks, AdmBlock b);

// The panner position (a point, width 0) that puts a sound at an ADM point: the inverse of how objects are written.
SurroundPan admPanPosition(const AdmPoint& at);

// Writes the master to `path` (.wav). `progress` (0..1) may return false to stop (the file is removed).
bool exportAdmBwf(const Project& p, const Sequence& s, const AdmSettings& settings, const std::string& path, AdmResult* result = nullptr,
                  const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// What a BW64 file holds: its channels, sample rate, bits and frames, its chna entries (track UID, track format,
// pack) and its axml text; false when it is not a readable BW64/RIFF WAVE file.
struct BwfInfo {
    int channels = 0, sampleRate = 0, bits = 0;
    int64_t frames = 0;
    struct Track {
        int index = 0;
        std::string uid, trackFormat, pack;
    };
    std::vector<Track> chna;
    std::string axml;
};
bool readBwfInfo(const std::string& path, BwfInfo& info, std::string* error = nullptr);

}  // namespace montage
