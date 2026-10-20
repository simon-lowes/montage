// Montage — a small language model on this computer, for words about the footage: chapters, summaries, show notes,
// story notes and audio description drafts (render/FootageWords.h). IBM's Granite 4.0 Micro (Apache-2.0, 3 billion
// parameters, 4-bit) run by llama.cpp (MIT); downloaded on first use (2.1 GB). Answers are held to a grammar (GBNF)
// where they must parse, and decoding is greedy, so the same question gets the same answer every time.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "ModelFiles.h"

namespace montage {

// $MONTAGE_LANGUAGE_MODEL names another folder, $MONTAGE_LANGUAGE_MODEL_URL a mirror.
const ModelPack& languageModel();
bool languageModelAvailable();  // built with llama.cpp

struct WriteRequest {
    std::string system;   // who the model is and how it answers (a chat model's system message)
    std::string prompt;   // the question
    std::string grammar;  // GBNF the answer follows (rule "root"); "" = free text
    int maxTokens = 512;
};

// A model, loaded once and shared; questions take turns.
class LanguageModel {
public:
    // The pack's model, or the GGUF file at `path`.
    static std::shared_ptr<LanguageModel> load(const std::string& path = {}, std::string* error = nullptr);
    ~LanguageModel();
    // The answer; empty with `error` when it fails or is cancelled. `progress` goes to a half as the question is read
    // and on as the answer is written (the share of maxTokens).
    std::string write(const WriteRequest& r, std::string* error = nullptr, const std::function<void(double)>& progress = {},
                      const std::atomic<bool>* cancel = nullptr) const;
    // Tokens in `text` (to keep questions inside the context).
    int tokenCount(const std::string& text) const;
    // The most tokens a question and its answer may hold together.
    int contextTokens() const;

    struct Impl;

private:
    LanguageModel();
    std::unique_ptr<Impl> d_;
};

}  // namespace montage
