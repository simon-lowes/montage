// Montage — voiceovers from text (like Resolve's Speech Generator or
// CapCut's text to speech) with Kokoro-82M (Hexgrad, Apache-2.0) on ONNX
// Runtime: English text becomes phonemes through misaki's dictionaries
// (core/G2p.h), and the model speaks them in one of ten voices, American
// or British. Everything runs on this computer.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "ModelFiles.h"

namespace montage {

// The model, its voices and misaki's dictionaries (172 MB), downloaded on
// first use; $MONTAGE_TTS_MODEL names another folder, $MONTAGE_TTS_MODEL_URL a mirror.
const ModelPack& ttsModel();
bool ttsAvailable();  // built with ONNX Runtime

struct TtsVoice {
    std::string id, name;
    bool british = false;
    bool female = false;
};
const std::vector<TtsVoice>& ttsVoices();
const TtsVoice* findTtsVoice(const std::string& id);

constexpr int kTtsSampleRate = 24000;

// Phonemes for text, with the voice's accent (UK dictionaries for British voices).
bool textToPhonemes(const std::string& text, bool british, std::string& out, std::string* error = nullptr);

// Speech for text: mono samples at kTtsSampleRate. `speed` 0.5..2. Long text is spoken a
// sentence at a time.
bool synthesizeSpeech(const std::string& text, const std::string& voice, double speed, std::vector<float>& out,
                      std::string* error = nullptr, const std::function<void(double)>& progress = {},
                      const std::atomic<bool>* cancel = nullptr);

// 16-bit mono WAV at kTtsSampleRate.
bool writeSpeechWav(const std::string& path, const std::vector<float>& samples, std::string* error = nullptr);

}  // namespace montage
