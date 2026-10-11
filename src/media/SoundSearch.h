// Montage — searching sound by what it is ("applause", "a door slamming", "rain on a window", "a crowd cheering"),
// as Premiere's and Resolve's sound-effect search and Final Cut's audio analysis do. Ten-second windows of each
// recording and the words of a query are embedded in one space by CLAP (LAION, Apache-2.0; Xenova's ONNX export),
// so the windows nearest a query are the ones it describes. Runs on this computer; the model is downloaded on first
// use and each recording is indexed once, the index kept with the project.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ModelFiles.h"
#include "core/VisualIndex.h"

namespace montage {

// $MONTAGE_SOUND_MODEL names another folder, $MONTAGE_SOUND_MODEL_URL a mirror.
const ModelPack& soundModel();
bool soundSearchAvailable();  // built with ONNX Runtime

constexpr int kClapRate = 48000;
constexpr int kClapWindow = kClapRate * 10;  // what the model hears at once
constexpr int kClapFrames = 1001, kClapMels = 64;

// CLAP's input for one window (mono at kClapRate; shorter repeated to fill it, as its feature extractor pads):
// a log-mel spectrogram (Slaney mel scale and norm, 64 bands from 50 Hz to 14 kHz, 1024-point FFT every 480
// samples, centred, power in dB), kClapFrames x kClapMels, row-major.
std::vector<float> clapFeatures(const std::vector<float>& mono);

// RoBERTa's byte-level BPE (vocab.json, merges.txt), as CLAP's text side reads a query: <s> ... </s>.
class ClapTokenizer {
public:
    bool load(const std::string& vocabJson, const std::string& mergesTxt);
    bool loaded() const { return !vocab_.empty(); }
    std::vector<int64_t> encode(const std::string& text) const;

private:
    std::vector<int64_t> word(const std::string& piece) const;
    std::map<std::string, int64_t> vocab_;
    std::map<std::pair<std::string, std::string>, int> ranks_;
    std::string byteChar_[256];
};

// The model, loaded once per process and shared (its sessions are thread safe).
class SoundModel {
public:
    static std::shared_ptr<SoundModel> load(std::string* error = nullptr);
    ~SoundModel();
    // Unit-length embeddings (512 values); empty on failure.
    std::vector<float> text(const std::string& query, std::string* error = nullptr) const;
    std::vector<float> audio(const std::vector<float>& features, std::string* error = nullptr) const;
    const ClapTokenizer& tokenizer() const;

    struct Impl;

private:
    SoundModel();
    std::unique_ptr<Impl> d_;
};

// Embeds 10-second windows every `step` seconds (0: every 5 s, every 2 s for recordings under 30 s) through a
// file's sound, as `out` (each sample's time is its window's middle). Long files are read a block at a time.
// `duration` (seconds; 0 if unknown) only paces the progress.
bool indexSound(const std::string& path, double duration, VisualIndex& out, double step = 0, const std::function<void(double)>& progress = {},
                const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

}  // namespace montage
