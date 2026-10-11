// Montage — speech in a voice cloned from a few seconds of someone speaking (DaVinci Resolve 21's Speech Generator,
// Descript's Overdub), in 23 languages, with Chatterbox Multilingual (Resemble AI, MIT) on ONNX Runtime. The
// reference recording is turned into a speaker's conditioning once; the text, tagged with its language, is read by a
// Llama-style model into speech tokens, which a flow-matching decoder and vocoder turn into 24 kHz sound in that
// voice. Everything runs on this computer. Cloning someone's voice needs their consent: the app asks for it.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ModelFiles.h"

namespace montage {

// The model (1.6 GB: speech encoder, token embedding, the 4-bit language model, decoder, tokenizer and the Cangjie
// table Chinese is read through), downloaded on first use; $MONTAGE_VOICE_CLONE_MODEL names another folder,
// $MONTAGE_VOICE_CLONE_MODEL_URL a mirror.
const ModelPack& voiceCloneModel();
bool voiceCloneAvailable();  // built with ONNX Runtime

struct CloneLanguage {
    const char* code;  // ISO 639-1, as the model tags text ("en", "fr"...)
    const char* name;
};
const std::vector<CloneLanguage>& voiceCloneLanguages();
const CloneLanguage* findCloneLanguage(const std::string& codeOrName);

constexpr int kCloneSampleRate = 24000;

// The text tokenizer (the model's tokenizer.json: characters merged by BPE, its added tokens, spaces as [SPACE]),
// with the language preparation the model expects: Korean syllables as jamo, Chinese characters as Cangjie codes.
class CloneTokenizer {
public:
    bool load(const std::string& tokenizerJson, const std::string& cangjieJson = {}, std::string* error = nullptr);
    bool loaded() const { return !vocab_.empty(); }
    // The text's ids between the model's markers: <EXAGGERATION> <s> [lang]text </s> <START_SPEECH> <START_SPEECH>.
    std::vector<int64_t> encode(const std::string& text, const std::string& language) const;
    // The text as the model reads it before tokenizing ("[ko]" and jamo, "[zh]" and Cangjie codes...).
    std::string prepare(const std::string& text, const std::string& language) const;
    int64_t id(const std::string& token) const;  // -1 if not in the vocabulary

private:
    std::vector<int64_t> bpe(const std::string& word) const;
    std::map<std::string, int64_t> vocab_;
    std::map<std::pair<std::string, std::string>, int> ranks_;
    std::vector<std::pair<std::string, int64_t>> added_;  // longest first
    std::map<uint32_t, std::string> cangjie_;             // character -> code (with its index among same codes)
    int64_t unk_ = 1;
};

// Punctuation as the model was trained on: one space between words, a capital first letter, unusual punctuation
// replaced (ellipses and colons as commas, curly quotes straight), and a full stop if nothing ends the sentence.
std::string normalizeCloneText(const std::string& text);

// Text in pieces the model reads at once (up to about `limit` characters), split after sentences, then clauses,
// then words.
std::vector<std::string> splitCloneText(const std::string& text, int limit = 260);

struct CloneOptions {
    std::string language = "en";
    double exaggeration = 0.5;  // 0.25 flat .. 2 very expressive (the model's emotion control)
};

// A reference recording made ready to speak with: mono at kCloneSampleRate, its quietest edges trimmed and at most
// 10 seconds (the model's reference length) taken from the loudest stretch.
std::vector<float> cloneReference(const std::vector<float>& mono, int rate);

// Speech for `text` in the voice of `reference` (mono at `referenceRate`: a few seconds of one person speaking
// cleanly, 5 to 10 is best): mono samples at kCloneSampleRate. Long text is spoken a piece at a time.
bool cloneSpeech(const std::string& text, const std::vector<float>& reference, int referenceRate, const CloneOptions& options,
                 std::vector<float>& out, std::string* error = nullptr, const std::function<void(double)>& progress = {},
                 const std::atomic<bool>* cancel = nullptr);

// A voice sample from a file: its sound from `from` to `to` seconds (`to` at or before `from`: to the end), as mono at
// kCloneSampleRate (with `channels`, those channels, as a clip picks them). Read from the sound decoded for playback at
// `rate` (MediaPool), decoding it if it is not yet.
bool voiceSampleFrom(const std::string& path, double from, double to, std::vector<float>& mono, std::string* error = nullptr,
                     const std::vector<int>& channels = {}, int rate = 48000);

// The sound of a file from `from` to `to` seconds as voiceSampleFrom reads it, as mono at kCloneSampleRate, but whole
// (nothing trimmed or cut to ten seconds): speech to convert.
bool voiceSoundFrom(const std::string& path, double from, double to, std::vector<float>& mono, std::string* error = nullptr,
                    const std::vector<int>& channels = {}, int rate = 48000);

// Speech said again in another voice (speech to speech, as Resolve's Voice Convert, for ADR in the actor's voice): the
// words, timing and delivery of `speech` (mono at `speechRate`) in the voice of `reference` (a few seconds of one person
// speaking, as for cloneSpeech). Mono at kCloneSampleRate, exactly as long as the speech, so it stays in sync. The
// speech is converted a piece of at most about ten seconds at a time (voiceConversionCuts).
bool convertVoice(const std::vector<float>& speech, int speechRate, const std::vector<float>& reference, int referenceRate,
                  std::vector<float>& out, std::string* error = nullptr, const std::function<void(double)>& progress = {},
                  const std::atomic<bool>* cancel = nullptr);
// Where speech is cut into pieces for convertVoice: sample offsets from 0 to its end, each piece at most `most` seconds,
// cut at the quietest 20 ms in its last three seconds.
std::vector<size_t> voiceConversionCuts(const std::vector<float>& mono, int rate, double most = 9.5);

// Speech without the near-silence the model leaves before and after it (a few milliseconds kept).
void trimSpeech(std::vector<float>& speech, int rate);

// Speech made `factor` times as long (0.5: twice as fast), its pitch kept (WSOLA).
std::vector<float> stretchSpeech(const std::vector<float>& speech, int rate, double factor);

// cloneSpeech, trimmed, then `speed` times as fast, and faster (up to 1.6 times in all) where it would not fit in `room`
// seconds (0: any room). With `exact`, made `room` seconds long instead (within 0.7 to 1.4 times its own length).
bool cloneSpeechToFit(const std::string& text, const std::vector<float>& reference, int referenceRate, const CloneOptions& options,
                      double speed, double room, bool exact, std::vector<float>& out, std::string* error = nullptr,
                      const std::atomic<bool>* cancel = nullptr);

// Writes cloned speech as a 16-bit WAV at kCloneSampleRate, with `note` (where the voice came from, and that it is
// generated) in its INFO comment, so the file says what it is wherever it goes.
bool writeClonedSpeechWav(const std::string& path, const std::vector<float>& samples, const std::string& note, std::string* error = nullptr);

}  // namespace montage
