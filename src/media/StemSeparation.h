// Montage — separating a recording into stems: music into vocals, drums, bass, piano and the rest, or a film's finished
// mix into dialogue, music and effects (3 stems, with Bandit v2: media/BanditSeparation.h). Music is split with Spleeter (Deezer Research, MIT; trained on Deezer's own
// catalogue). A U-Net per stem estimates its share of each bin of the mix's magnitude spectrogram (44.1 kHz stereo,
// 4096-point frames every 1024 samples, in 512-frame segments); the shares become ratio masks that sum to one below
// 11 kHz, and each stem is its masked spectrogram turned back into sound. Runs on this computer, many times faster
// than real time; the authors' model archive is downloaded on first use and its weights read in place by a graph made
// from their code (scripts/gen-spleeter-graph.py).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ModelFiles.h"
#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

constexpr int kStemRate = 44100;  // Spleeter's
// The rate a separation's sound goes in and comes out at: 48 kHz for dialogue, music and effects, else kStemRate.
int stemRate(int stems);

// The model for 2 stems (vocals, accompaniment), 3 (dialogue, music, effects), 4 (vocals, drums, bass, other) or 5
// (vocals, piano, drums, bass, other). $MONTAGE_STEM_MODELS names a folder holding them (spleeter-<n>stems,
// bandit-v2-eng), $MONTAGE_STEM_MODEL_URL a mirror of Spleeter's and $MONTAGE_DME_MODEL_URL of Bandit's.
const ModelPack& stemModel(int stems);
bool stemSeparationAvailable();  // built with ONNX Runtime
// The stems a model gives, in its order ("vocals", "drums"...); empty for a count it does not offer.
const std::vector<std::string>& stemNames(int stems);
// "Vocals", "Drums", "Bass", "Piano", "Other", "Accompaniment", "Dialogue", "Music", "Effects".
std::string stemTitle(const std::string& stem);

// A separation in progress. The sound is pushed as it comes (interleaved stereo at stemRate) and then finished;
// each stretch of the stems that is complete goes to `write`, one interleaved stereo block per stem (stemNames order),
// all the same length, in order, as Spleeter's own separation would give them. False from `write` stops it.
class StemSplitter {
public:
    using Output = std::function<bool(const std::vector<std::vector<float>>& stems)>;
    // Loads the model (once per process); null with a reason if it is missing or cannot run.
    static std::unique_ptr<StemSplitter> create(int stems, Output write, std::string* error = nullptr);
    ~StemSplitter();
    int stems() const;
    bool push(const float* stereo, size_t frames, std::string* error = nullptr);
    bool finish(std::string* error = nullptr);
    // Frames of input separated so far.
    int64_t done() const;

    struct Impl;

private:
    StemSplitter();
    std::unique_ptr<Impl> d_;
};

// A file's sound from `from` to `to` seconds (`to` <= 0: to its end), its `channels` when given (Clip::channels, as
// decodeAudio reads them), separated into one 32-bit float stereo WAV per stem at stemRate (`outPaths`, stemNames
// order). `progress` goes from 0 to 1 when the stretch's length is known. Nothing is left behind on failure.
bool separateStemsToFiles(const std::string& path, double from, double to, const std::vector<int>& channels, int stems,
                          const std::vector<std::string>& outPaths, const std::function<void(double)>& progress = {},
                          const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// Where a clip's stems are separated from: the source it plays (either way round, at any speed) with `handles` seconds
// either side, within its media; false (with a reason) for a clip with no sound of a file to separate.
bool stemSource(const Project& p, const Sequence& s, Id clip, double handles, double& from, double& to, std::string* error = nullptr);

// Places separated stems in sync with `clip`: a copy of it per stem (its timing, speed, volume and pan; not its effects,
// channels or markers, which belong to the mix) playing `stemMedia[i]`, whose time 0 is source time `from` seconds;
// each on the audio track named `titles[i]` that is free there (one is added below the others if none is), linked
// with the clip and anything linked to it, and given the audio role its title names (Dialogue, Music or Effects);
// the clip itself disabled when `muteOriginal`.
edit::Result placeStems(Project& p, Sequence& s, Id clip, const std::vector<Id>& stemMedia, const std::vector<std::string>& titles,
                        double from, bool muteOriginal);

}  // namespace montage
