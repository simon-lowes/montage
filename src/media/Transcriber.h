// Montage — local speech-to-text with whisper.cpp (MIT). Runs on the CPU,
// or Metal on macOS; nothing leaves the machine.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Transcript.h"

namespace montage {

struct WhisperModel {
    std::string name;  // "base.en"
    std::string label;
    int64_t bytes = 0;  // download size
    bool englishOnly = false;
};

// The ggml whisper models Montage can download, smallest first.
const std::vector<WhisperModel>& whisperModels();
// Where downloaded models live ($MONTAGE_WHISPER_MODELS or the app data folder).
std::string whisperModelsDirectory();
// Local file for a model ("" if not downloaded). Accepts a name or a path.
std::string whisperModelPath(const std::string& nameOrPath);
// Download location of a model.
std::string whisperModelUrl(const std::string& name);
// True if the file starts like a ggml whisper model (catches error pages saved as models).
bool isWhisperModelFile(const std::string& path);

struct TranscribeOptions {
    std::string model;               // name or path
    std::string language = "auto";   // ISO 639-1 code or "auto"
    bool translate = false;          // translate to English
    int threads = 0;                 // 0 = automatic
};

using TranscribeProgress = std::function<void(double fraction)>;

// Transcribes 16 kHz mono samples. Times are seconds from the first sample.
bool transcribeSamples(const std::vector<float>& mono16k, const TranscribeOptions& options, Transcript& out,
                       const TranscribeProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                       std::string* error = nullptr);

// Decodes a media file's audio and transcribes it (media time).
bool transcribeMedia(const std::string& path, const TranscribeOptions& options, Transcript& out,
                     const TranscribeProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                     std::string* error = nullptr);

}  // namespace montage
