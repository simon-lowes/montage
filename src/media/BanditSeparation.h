// Montage — dialogue, music and effects from a finished mix, with Bandit v2 (Watcharasupat, Wu and Lerch, "Remastering
// Divide and Remaster", 2024; code Apache-2.0, weights CC BY-SA 4.0, trained on DnR v3's openly licensed speech, music
// and effects). Each channel is separated alone: in 8-second chunks every 4 seconds (Hann-weighted where they overlap),
// each chunk's spectrogram (2048-point frames every 512 samples at 48 kHz) split into 64 bands a semitone-spaced
// filterbank sets, modelled by bidirectional GRUs across time and across the bands in turn, and given a complex mask per
// stem. The graph (src/media/bandit-v2-eng.onnx, from scripts/gen-bandit-graph.py) reads its weights in place from the
// authors' checkpoint, downloaded on first use. Used through StemSplitter (StemSeparation.h) as its 3-stem separation.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "StemSeparation.h"

namespace montage {

constexpr int kDmeRate = 48000;

const ModelPack& dmeModel();

class BanditSplitter {
public:
    // Loads the model (once per process); null with a reason if it is missing or cannot run.
    static std::unique_ptr<BanditSplitter> create(StemSplitter::Output write, std::string* error = nullptr);
    ~BanditSplitter();
    bool push(const float* stereo, size_t frames, std::string* error = nullptr);
    bool finish(std::string* error = nullptr);
    int64_t done() const;

    struct Impl;

private:
    BanditSplitter();
    std::unique_ptr<Impl> d_;
};

}  // namespace montage
