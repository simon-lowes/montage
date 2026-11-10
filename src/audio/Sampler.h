// Montage — a sound-effect sampler played to picture (Foley): footsteps, cloth and props on MIDI keys, pads or the
// computer's keys, recorded in passes onto an audio track's take lanes.
//
// A track's sampler (core/Model.h SamplerSettings) holds up to 128 slots, one a key. Each slot has one or more sounds
// (files, or stretches of them), with velocity layers (the sounds answering a velocity take turns: round robin),
// random variation of pitch and level for each hit, attack and release, one-shot or held playback, and choke groups
// (a note cuts every other voice of its group, within a millisecond: one foot cuts the other).
//
// The Engine plays notes block by block, in any thread: notes are queued from the MIDI thread or the interface and
// started in the audio thread at their sample, or at the next block's start when they come as they are played. A
// recorded pass keeps its notes (at the sample the picture showed when each key went down, so the output latency is
// taken off) and is rendered to a file that goes onto a take lane as an ordinary clip; voicing it again renders the
// same notes, with the same timing and variation, through the slots as they are now.
//
// Long sounds (ambiences, room tones, long cloth passes) are not held in memory whole: their first second is, and the
// rest is read from disk as they play (a thread fills each voice's window ahead of it). A slot's velocity curve
// shapes how the force a key is played with comes out; a slot can send its part of a pass to another track (its own
// lane and strip); and a pass can be placed as one clip a hit.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"
#include "media/Decoder.h"

namespace montage::sampler {

// Velocity in, through the slot's curve, to velocity out (1-127): what sets its level and the layer that answers.
int curveVelocity(const SamplerSlot& slot, int velocity);

// A long sound's frames on disk: 32-bit float stereo at the kit's rate, decoded once into a cache file.
struct DiskSound {
    std::string file;
    int64_t frames = 0;
};

// A track's slots with their sounds decoded at the rate they are played at.
struct Kit {
    struct Sound {
        AudioBufferPtr buffer;  // stereo at the kit's rate: the whole file, or for a streamed sound its first frames
        int64_t start = 0, end = 0;  // frames of the file
        int velLow = 1, velHigh = 127;
        // Streamed: `buffer` holds frames start .. start + head, and the rest comes from `disk`.
        std::shared_ptr<const DiskSound> disk;
        int64_t head = 0;
    };
    struct Slot {
        SamplerSlot settings;
        std::vector<Sound> sounds;
    };
    int sampleRate = 48000;
    std::vector<Slot> pads;
    std::array<int, 128> byKey{};  // index into slots + 1; 0 none
    const Slot* slot(int key) const { return key >= 0 && key < 128 && byKey[size_t(key)] ? &pads[size_t(byKey[size_t(key)] - 1)] : nullptr; }
};
using KitPtr = std::shared_ptr<const Kit>;
using Loader = std::function<AudioBufferPtr(const std::string& path, int sampleRate, std::string* error)>;

// Which sounds are streamed from disk: those longer than `longerThan` seconds (never when 0), their first
// `headSeconds` kept in memory, the rest decoded into a cache file under `cacheDir` ("" the system's temporary folder).
struct StreamOptions {
    double longerThan = 20;
    double headSeconds = 1;
    std::string cacheDir;
};

// Decodes a sampler's sounds (through `load`, by default the media pool's decoder; long ones, by default, in blocks
// straight into their cache file); files that cannot be read are listed in `missing` and left out.
KitPtr loadKit(const SamplerSettings& settings, int sampleRate, std::vector<std::string>* missing = nullptr, const Loader& load = {},
               const StreamOptions& stream = {});
// A kit of only the slots sent to `output` (SamplerSlot::output; 0 the sampler's own track), sharing their sounds.
KitPtr kitFor(const KitPtr& kit, Id output);

// Streaming from disk: every read takes at least this long a megabyte (tests: a slow disk), and the frames played as
// silence because the disk had not caught up, since Montage started.
void setDiskThrottle(double secondsPerMegabyte);
int64_t streamUnderruns();

// The seed of the n-th note of a pass (its pitch and level variation).
uint32_t noteSeed(uint32_t passSeed, size_t index);

class Engine {
public:
    explicit Engine(int sampleRate = 48000);
    ~Engine();
    int sampleRate() const { return rate_; }
    // Offline renders (passes) wait for the disk where a streamed sound has not been read yet, rather than play
    // silence there as the audio thread does.
    void setBlocking(bool on) { blocking_ = on; }
    // The sounds (any thread); voices already sounding keep theirs.
    void setKit(KitPtr kit);
    KitPtr kit() const;
    // A key down or up (any thread): `at` the sample in the render's timeline, or -1 for the start of the next block.
    // A note given an `id` (not 0) is released by that id alone; else a key up releases every voice held on the key.
    void noteOn(int key, int velocity, int64_t at = -1, uint32_t seed = 0, uint64_t id = 0);
    void noteOff(int key, int64_t at = -1, uint64_t id = 0);
    void allNotesOff();  // every voice released
    // Adds `frames` stereo samples from timeline sample `start` to `out` (interleaved). Notes due before `start` start
    // at its first sample.
    void render(int64_t start, int frames, float* out);
    void reset();  // silence, queued notes dropped, round robin from the start
    int activeVoices() const { return int(voices_.size()); }

private:
    struct Event {
        bool on = true;
        int key = 0, velocity = 0;
        int64_t at = -1;
        uint32_t seed = 0;
        uint64_t order = 0;
        uint64_t id = 0;
    };
    struct Window;
    struct Disk;
    struct Voice {
        KitPtr kit;
        const Kit::Sound* sound = nullptr;
        Window* window = nullptr;  // a streamed sound's frames past its head, read ahead from disk
        int key = 0, choke = 0;
        uint64_t id = 0;
        bool oneShot = true;
        double pos = 0, step = 1;
        float gain = 1;
        int64_t age = 0;                     // samples played
        int64_t attackLen = 0, releaseLen = 0;
        int64_t released = -1;               // samples since the release began (-1 none)
        int64_t choked = -1;                 // samples since a choke began
        float releaseFrom = 1;
        uint64_t serial = 0;
    };
    void startVoice(const Event& e, const KitPtr& kit);
    void release(int key, uint64_t id);
    void renderVoices(float* out, int frames);
    void letGo(Voice& v);  // its window back to the disk thread

    int rate_;
    int64_t chokeLen_;
    mutable std::mutex kitMutex_, eventMutex_;
    KitPtr pendingKit_, kit_;
    // Kits the audio thread let go of, freed outside it once no voice plays them (nothing is freed in the callback).
    std::vector<KitPtr> retired_;
    std::vector<Event> queued_, due_;
    uint64_t order_ = 0, serial_ = 0;
    std::vector<Voice> voices_;
    std::vector<std::pair<int, int>> robinKeys_;  // (key, first sound index) -> turn, as a small list
    std::vector<uint32_t> robinTurns_;
    bool resetPending_ = false;
    bool blocking_ = false;
    std::shared_ptr<Disk> disk_;  // (made by setKit for a kit with streamed sounds)
};

// A pass's notes rendered (stereo, interleaved) from timeline sample `start`, in the order given (the order they were
// played, which numbers their variation and round robin turns): `frames` long, or until the last voice
// has ended when `frames` is negative (at most ten minutes after the last note).
std::vector<float> renderNotes(const KitPtr& kit, const std::vector<SamplerNote>& notes, uint32_t seed, int64_t start,
                               int64_t frames = -1);
// A 32-bit float WAV file.
bool writeWav(const std::string& path, const std::vector<float>& stereo, int sampleRate, std::string* error = nullptr);

// Split by Onsets: the hits in a recording of many (spectral flux over the noise floor, as Elastic Audio's onsets),
// each from its onset to the next (seconds).
std::vector<std::pair<double, double>> splitByOnsets(const AudioBuffer& recording, double sensitivity = 50, double minGap = 0.05);

// ---- Edits ------------------------------------------------------------------------------------------------------
// A slot set (replacing the one on its key).
edit::Result setSlot(Sequence& s, TrackRef track, const SamplerSlot& slot);
edit::Result removeSlot(Sequence& s, TrackRef track, int key);
// The stretches of `path` onto successive keys from `firstKey` (their slots replaced); created: nothing.
edit::Result addSlices(Sequence& s, TrackRef track, const std::string& path, const std::vector<std::pair<double, double>>& slices,
                       int firstKey, const std::string& name = {});
// A recorded pass: its notes rendered through `kit` to `file` (from the frame of its first note, or `from` when that
// is earlier, by at most two seconds), the file added to the project and put on a new take lane, the lane given to the comp over the pass,
// and the notes kept. created: the lane, then the comp clip.
// Slots sent to other tracks have their part rendered on its own onto a lane of that track (its file named after
// `file` with the track's id), kept as a pass of this track's that names it. With clip-per-hit on, each comp is cut at
// every later note.
edit::Result addPass(Project& p, Sequence& s, TrackRef track, const KitPtr& kit, std::vector<SamplerNote> notes, uint32_t seed,
                     const std::string& file, FrameTime from = -1, const std::string& name = {});
// A pass voiced again through `kit` (the track's slots as they are now) into `file`, with the same notes and timing:
// the lane and the comp clips cut from it take the new sound.
edit::Result revoicePass(Project& p, Sequence& s, TrackRef track, Id lane, const KitPtr& kit, const std::string& file);

}  // namespace montage::sampler
