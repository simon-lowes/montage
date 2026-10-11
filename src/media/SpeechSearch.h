// Montage — searching what is said by meaning (Premiere 26's media intelligence search, Descript's search by topic):
// transcripts' passages and a query are embedded by multi-qa-MiniLM-L6-cos-v1 (sentence-transformers, Apache-2.0;
// trained for questions and the passages that answer them; 4-bit ONNX export) and the nearest passages are the
// moments, with a little extra for the query's own words. Runs locally on ONNX Runtime; the model (55 MB) is
// downloaded on first use. Passage embeddings are kept for the session, so searching again is immediate.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ModelFiles.h"
#include "core/Model.h"
#include "core/SpokenSearch.h"

namespace montage {

// $MONTAGE_SENTENCE_MODEL names another folder, $MONTAGE_SENTENCE_MODEL_URL a mirror.
const ModelPack& sentenceModel();
bool speechSearchAvailable();  // built with ONNX Runtime

class SentenceModel {
public:
    static std::shared_ptr<SentenceModel> load(std::string* error = nullptr);
    ~SentenceModel();
    // BERT's uncased WordPiece tokens of `text` with [CLS] and [SEP] (at most 256).
    std::vector<int64_t> tokens(const std::string& text) const;
    // Unit-length embeddings (384 values), one per text; empty on failure.
    std::vector<std::vector<float>> embed(const std::vector<std::string>& texts, std::string* error = nullptr) const;
    struct Impl;

private:
    SentenceModel();
    std::unique_ptr<Impl> d_;
};

struct SpokenSearchOptions {
    size_t max = 20;
    float minScore = 0.12f;
    std::vector<Id> media;  // only these (empty: every transcribed media item)
};

// The moments of the project's transcripts that say what `query` asks about, best first. Empty with `error` when
// the model is missing or nothing is transcribed.
std::vector<SpokenHit> searchSpoken(const Project& p, const std::string& query, const SpokenSearchOptions& o = {},
                                    std::string* error = nullptr, const std::function<void(double)>& progress = {},
                                    const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
