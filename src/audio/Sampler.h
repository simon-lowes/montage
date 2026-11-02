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
#pragma once

#include <array>
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

// A track's slots with their sounds decoded at the rate they are played at.
struct Kit {
    struct Sound {
        AudioBufferPtr buffer;  // stereo at the kit's rate
        int64_t start = 0, end = 0;
        int velLow = 1, velHigh = 127;
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

// Decodes a sampler's sounds (through `load`, by default the media pool's decoder); files that cannot be read are
// listed in `missing` and left out.
KitPtr loadKit(const SamplerSettings& settings, int sampleRate, std::vector<std::string>* missing = nullptr, const Loader& load = {});

// The seed of the n-th note of a pass (its pitch and level variation).
uint32_t noteSeed(uint32_t passSeed, size_t index);

class Engine {
public:
    explicit Engine(int sampleRate = 48000);
    int sampleRate() const { return rate_; }
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
    struct Voice {
        KitPtr kit;
        const Kit::Sound* sound = nullptr;
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

    int rate_;
    int64_t chokeLen_;
    mutable std::mutex kitMutex_, eventMutex_;
    KitPtr pendingKit_, kit_;
    std::vector<Event> queued_, due_;
    uint64_t order_ = 0, serial_ = 0;
    std::vector<Voice> voices_;
    std::vector<std::pair<int, int>> robinKeys_;  // (key, first sound index) -> turn, as a small list
    std::vector<uint32_t> robinTurns_;
    bool resetPending_ = false;
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
// is earlier), the file added to the project and put on a new take lane, the lane given to the comp over the pass,
// and the notes kept. created: the lane, then the comp clip.
edit::Result addPass(Project& p, Sequence& s, TrackRef track, const KitPtr& kit, std::vector<SamplerNote> notes, uint32_t seed,
                     const std::string& file, FrameTime from = -1, const std::string& name = {});
// A pass voiced again through `kit` (the track's slots as they are now) into `file`, with the same notes and timing:
// the lane and the comp clips cut from it take the new sound.
edit::Result revoicePass(Project& p, Sequence& s, TrackRef track, Id lane, const KitPtr& kit, const std::string& file);

}  // namespace montage::sampler
