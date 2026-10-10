// Montage — translating captions and transcripts on this computer, with
// Opus-MT (Helsinki-NLP, CC-BY-4.0: credit the OPUS-MT project) in
// Xenova's ONNX exports, run on ONNX Runtime. Each language pair is its own
// model (about 270 MB, downloaded the first time it is used); pairs Opus-MT
// does not cover go through English.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ModelFiles.h"

namespace montage {

struct TranslationLanguage {
    std::string code;  // ISO 639-1
    std::string name;  // in English
};
// Every language there is a model to or from English for.
const std::vector<TranslationLanguage>& translationLanguages();
std::string translationLanguageName(const std::string& code);

// The model for one direction ($MONTAGE_TRANSLATION_MODELS names a folder
// holding the pairs' folders instead), or nullptr if Opus-MT has none.
const ModelPack* translationModel(const std::string& from, const std::string& to);
// The models a translation needs, in order: one, or two through English. Empty if it cannot be done.
std::vector<const ModelPack*> translationRoute(const std::string& from, const std::string& to);
bool translatorAvailable();  // built with ONNX Runtime

// Translates each text (a caption, a paragraph) from one language to another.
bool translateTexts(const std::vector<std::string>& in, const std::string& from, const std::string& to, std::vector<std::string>& out,
                    const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                    std::string* error = nullptr);

// ---- Building blocks (exposed for tests) ------------------------------------

// A SentencePiece unigram model read from its .spm file: text to pieces,
// the most likely split of each word (Viterbi over the pieces' scores).
class SentencePiece {
public:
    bool load(const std::string& path, std::string* error = nullptr);
    std::vector<std::string> encode(const std::string& text) const;
    size_t size() const { return pieces_.size(); }

private:
    std::map<std::string, float> pieces_;  // normal pieces and their scores
    float minScore_ = 0;
    int maxChars_ = 1;
};

}  // namespace montage
