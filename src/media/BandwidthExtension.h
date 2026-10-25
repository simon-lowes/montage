// Montage — restoring the missing top of band-limited speech (a voice recorded through a phone, a video call or a
// low-rate codec): the frequencies above its cut-off rebuilt so it matches the rest of the scene. AP-BWE (Ye-Xin Lu,
// Yang Ai, Hui-Peng Du and Zhen-Hua Ling, 2024; code and weights MIT, trained on VCTK 0.92, CC BY 4.0) predicts the
// wide band's magnitude and phase from the narrow band's STFT. Its graph (scripts/gen-apbwe-graph.py) reads its
// weights in place from the authors' checkpoints, one for each pair of rates, and runs on ONNX Runtime.
//
// The cut-off is found from where the long-term spectrum of the speech falls away, and picks the checkpoint (two in
// turn when the speech is narrower than 4 kHz: up to 16 kHz first). Below the cut-off the original is kept exactly; the
// new band is crossfaded in (equal power) over 500 Hz above it, and only where the speech is (quiet stretches are left
// as they are).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ModelFiles.h"

namespace montage {

// A checkpoint: the rate its speech was band-limited to and the rate it brings it to (Hz).
struct BweCheckpoint {
    int low = 0, high = 0;
    bool operator==(const BweCheckpoint&) const = default;
};
// 2, 4 and 8 kHz to 16 kHz; 8, 12, 16 and 24 kHz to 48 kHz.
const std::vector<BweCheckpoint>& bweCheckpoints();
// Each checkpoint's model (119 MB), downloaded when a clip needs it; $MONTAGE_BWE_MODELS names a folder holding them
// (as apbwe-8k-16k and so on), $MONTAGE_BWE_MODEL_URL a mirror.
const ModelPack& bweModel(const BweCheckpoint& c);
bool bandwidthExtensionAvailable();  // built with ONNX Runtime

// Where the speech's spectrum falls away (Hz): the top of its band, from the frames of the recording within 30 dB of
// its loudest. rate / 2 when nothing is missing.
double speechCutoff(const std::vector<float>& mono, int rate);
// The checkpoints that take speech with this cut-off up to the top of `rate` (as far as 24 kHz), in turn. Empty when
// nothing is missing (the cut-off within a tenth of the top), or too little is there to work from (under 800 Hz).
std::vector<BweCheckpoint> bwePlan(double cutoff, int rate);

// The network on the STFT of frames [first, first + frames) of speech at c.high (n_fft 1024, a 320-sample Hann
// window every 80 samples, centred on frame t's sample 80 t, the ends reflected), each array 513 bins by `frames` (bin
// major): log(|X| + 1e-4) and the phase in, the wide band's out. It is given the sound a chunk at a time, in order.
using BweNetwork = std::function<bool(const BweCheckpoint& c, int64_t first, int frames, const std::vector<float>& logAmp,
                                      const std::vector<float>& phase, std::vector<float>& logAmpOut, std::vector<float>& phaseOut,
                                      std::string* error)>;
// AP-BWE on ONNX Runtime (the checkpoint's model must be installed).
BweNetwork bweModelNetwork();

struct BweResult {
    double cutoff = 0;                // found (or given)
    std::vector<BweCheckpoint> plan;  // the checkpoints used
};

// Restores mono speech at `rate`; `out` lines up with `in` and has its length. `amount` 0..100 scales the new band,
// `cutoff` 0 finds it. With nothing to restore `out` is `in`. False (with `error`) when a model is missing or failed,
// or when cancelled.
bool extendBandwidth(const std::vector<float>& in, int rate, std::vector<float>& out, double amount = 100, double cutoff = 0,
                     const BweNetwork& network = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr,
                     BweResult* result = nullptr);

// Resamples with a windowed sinc (Kaiser, 32 zero crossings a side), the band kept to 97 % of the lower rate's top.
std::vector<float> resampleSinc(const std::vector<float>& in, int from, int to);

}  // namespace montage
