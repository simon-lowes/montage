#include "SpeechSearch.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QString>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& sentenceModel() {
    static const ModelPack pack = [] {
        const std::string base = "https://huggingface.co/Xenova/multi-qa-MiniLM-L6-cos-v1/resolve/3c96c1df3fec9a98a5de76e80877c8bec18498f6/";
        ModelPack p;
        p.id = "minilm-qa";
        p.title = "speech search model";
        p.directoryEnv = "MONTAGE_SENTENCE_MODEL";
        p.urlEnv = "MONTAGE_SENTENCE_MODEL_URL";
        p.files = {
            // 4-bit weights with float maths, as for CLIP (no integer matrix kernels).
            {"model_q4.onnx", base + "onnx/model_q4.onnx", "e798e950284f27b1cc028ab16b4175b997126dd3091304e4355e318c24c89bbc", 54578772},
            {"vocab.txt", base + "vocab.txt", "07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3", 231508},
        };
        return p;
    }();
    return pack;
}

namespace {

constexpr int64_t kCls = 101, kSep = 102, kUnk = 100;
constexpr size_t kMaxTokens = 256;

// ---- BERT's uncased WordPiece --------------------------------------------------------
// Text is cleaned (control characters dropped, whitespace normalised), lower-cased with accents stripped (NFD,
// combining marks removed), CJK characters and punctuation split off, then each word is taken greedily as the
// longest pieces in the vocabulary ("##" continuing a word).
struct WordPiece {
    std::unordered_map<std::string, int64_t> vocab;

    bool load(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        int64_t id = 0;
        for (const QByteArray& line : f.readAll().split('\n')) {
            QByteArray l = line;
            if (l.endsWith('\r')) l.chop(1);
            if (!l.isEmpty() || id == 0) vocab.emplace(l.toStdString(), id);
            ++id;
        }
        return vocab.size() > 1000;
    }

    static bool punctuation(char32_t c) {
        if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) return true;
        const QChar::Category cat = QChar::category(c);
        return cat >= QChar::Punctuation_Connector && cat <= QChar::Punctuation_Other;
    }
    static bool cjk(char32_t c) {
        return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2A6DF) || (c >= 0x2A700 && c <= 0x2B73F) ||
               (c >= 0x2B740 && c <= 0x2B81F) || (c >= 0x2B820 && c <= 0x2CEAF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
    }

    std::vector<std::string> basic(const std::string& text) const {
        // Lower case, then accents off.
        const QString lowered = QString::fromStdString(text).toLower().normalized(QString::NormalizationForm_D);
        std::vector<std::string> words;
        std::u32string cur;
        auto flush = [&] {
            if (!cur.empty()) words.push_back(QString::fromUcs4(cur.data(), qsizetype(cur.size())).toStdString());
            cur.clear();
        };
        const QList<uint> ucs = lowered.toUcs4();
        for (const uint u : ucs) {
            const char32_t c = char32_t(u);
            if (c == 0 || c == 0xFFFD) continue;
            const QChar::Category cat = QChar::category(c);
            if (cat == QChar::Mark_NonSpacing) continue;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || cat == QChar::Separator_Space) {
                flush();
                continue;
            }
            if (cat == QChar::Other_Control || cat == QChar::Other_Format) continue;
            if (punctuation(c) || cjk(c)) {
                flush();
                cur.push_back(c);
                flush();
                continue;
            }
            cur.push_back(c);
        }
        flush();
        return words;
    }

    void piece(const std::string& word, std::vector<int64_t>& out) const {
        const QList<uint> ucs = QString::fromStdString(word).toUcs4();
        const std::u32string chars(ucs.begin(), ucs.end());
        if (chars.size() > 100) {
            out.push_back(kUnk);
            return;
        }
        std::vector<int64_t> pieces;
        size_t start = 0;
        while (start < chars.size()) {
            size_t end = chars.size();
            int64_t found = -1;
            while (end > start) {
                std::string sub = QString::fromUcs4(chars.data() + start, qsizetype(end - start)).toStdString();
                if (start > 0) sub = "##" + sub;
                const auto it = vocab.find(sub);
                if (it != vocab.end()) {
                    found = it->second;
                    break;
                }
                --end;
            }
            if (found < 0) {
                out.push_back(kUnk);
                return;
            }
            pieces.push_back(found);
            start = end;
        }
        out.insert(out.end(), pieces.begin(), pieces.end());
    }

    std::vector<int64_t> encode(const std::string& text) const {
        std::vector<int64_t> ids{kCls};
        for (const std::string& w : basic(text)) {
            piece(w, ids);
            if (ids.size() >= kMaxTokens - 1) break;
        }
        if (ids.size() > kMaxTokens - 1) ids.resize(kMaxTokens - 1);
        ids.push_back(kSep);
        return ids;
    }
};

// Passage embeddings by model and text, for the session.
std::mutex cacheLock;
std::unordered_map<std::string, std::vector<float>>& cache() {
    static auto* c = new std::unordered_map<std::string, std::vector<float>>();
    return *c;
}
std::string cacheKey(const std::string& text) {
    return QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha1).toHex().toStdString();
}

}  // namespace

#ifdef MONTAGE_WITH_ONNXRUNTIME
bool speechSearchAvailable() { return true; }
struct SentenceModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-sentences"};
    std::unique_ptr<Ort::Session> session;
    WordPiece wordPiece;
};
#else
bool speechSearchAvailable() { return false; }
struct SentenceModel::Impl {
    WordPiece wordPiece;
};
#endif

SentenceModel::SentenceModel() : d_(std::make_unique<Impl>()) {}
SentenceModel::~SentenceModel() = default;

std::shared_ptr<SentenceModel> SentenceModel::load(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<SentenceModel>();
    std::lock_guard lock(m);
    if (*cached) return *cached;
    const ModelPack& pack = sentenceModel();
    if (!pack.installed()) {
        if (error) *error = "The speech search model is not downloaded";
        return nullptr;
    }
    std::shared_ptr<SentenceModel> model(new SentenceModel);
    if (!model->d_->wordPiece.load(QString::fromStdString(pack.path(pack.files[1])))) {
        if (error) *error = "The speech search model's vocabulary could not be read";
        return nullptr;
    }
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(error)) return nullptr;
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(pack.path(pack.files[0])).toStdWString();
#else
        const std::string path = pack.path(pack.files[0]);
#endif
        model->d_->session = std::make_unique<Ort::Session>(model->d_->env, path.c_str(), so);
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The speech search model could not be loaded: ") + e.what();
        return nullptr;
    }
#else
    if (error) *error = "This build of Montage cannot search speech by meaning (it was built without ONNX Runtime)";
    return nullptr;
#endif
    *cached = model;
    return model;
}

std::vector<int64_t> SentenceModel::tokens(const std::string& text) const { return d_->wordPiece.encode(text); }

std::vector<std::vector<float>> SentenceModel::embed(const std::vector<std::string>& texts, std::string* error) const {
    std::vector<std::vector<float>> out;
#ifdef MONTAGE_WITH_ONNXRUNTIME
    try {
        // In batches of similar length (sorted), padded, the mean taken over each text's own tokens.
        std::vector<std::vector<int64_t>> toks;
        for (const std::string& t : texts) toks.push_back(tokens(t));
        std::vector<size_t> order(texts.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return toks[a].size() < toks[b].size(); });
        out.assign(texts.size(), {});
        constexpr size_t kBatch = 16;
        for (size_t b0 = 0; b0 < order.size(); b0 += kBatch) {
            const size_t n = std::min(kBatch, order.size() - b0);
            size_t len = 0;
            for (size_t k = 0; k < n; ++k) len = std::max(len, toks[order[b0 + k]].size());
            std::vector<int64_t> ids(n * len, 0), mask(n * len, 0), types(n * len, 0);
            for (size_t k = 0; k < n; ++k) {
                const auto& t = toks[order[b0 + k]];
                for (size_t j = 0; j < t.size(); ++j) ids[k * len + j] = t[j], mask[k * len + j] = 1;
            }
            const int64_t shape[2] = {int64_t(n), int64_t(len)};
            auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::vector<Ort::Value> in;
            in.push_back(Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape, 2));
            in.push_back(Ort::Value::CreateTensor<int64_t>(mem, mask.data(), mask.size(), shape, 2));
            in.push_back(Ort::Value::CreateTensor<int64_t>(mem, types.data(), types.size(), shape, 2));
            const char* inNames[] = {"input_ids", "attention_mask", "token_type_ids"};
            const char* outNames[] = {"last_hidden_state"};
            auto res = d_->session->Run(Ort::RunOptions{nullptr}, inNames, in.data(), 3, outNames, 1);
            const float* h = res[0].GetTensorData<float>();
            const auto dims = res[0].GetTensorTypeAndShapeInfo().GetShape();
            const size_t dim = size_t(dims.back());
            for (size_t k = 0; k < n; ++k) {
                std::vector<float> v(dim, 0.0f);
                const size_t count = toks[order[b0 + k]].size();
                for (size_t j = 0; j < count; ++j)
                    for (size_t d = 0; d < dim; ++d) v[d] += h[(k * len + j) * dim + d];
                double norm = 0;
                for (float& x : v) x /= float(count), norm += double(x) * x;
                const float inv = norm > 0 ? float(1 / std::sqrt(norm)) : 0.0f;
                for (float& x : v) x *= inv;
                out[order[b0 + k]] = std::move(v);
            }
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
        out.clear();
    }
#else
    (void)texts;
    if (error) *error = "No ONNX Runtime";
#endif
    return out;
}

std::vector<SpokenHit> searchSpoken(const Project& p, const std::string& query, const SpokenSearchOptions& o, std::string* error,
                                    const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<SpokenHit>{};
    };
    if (QString::fromStdString(query).trimmed().isEmpty()) return fail("Say what to look for");
    // The passages of every transcribed media item asked about.
    struct Item {
        Id media;
        SpokenPassage passage;
    };
    std::vector<Item> items;
    for (const MediaItem& m : p.media) {
        if (!m.transcript || m.subclipOf) continue;
        if (!o.media.empty() && std::find(o.media.begin(), o.media.end(), m.id) == o.media.end()) continue;
        for (SpokenPassage& sp : spokenPassages(*m.transcript)) items.push_back({m.id, std::move(sp)});
    }
    if (items.empty()) return fail("Transcribe the footage first: speech is searched by its transcript");
    std::string err;
    auto model = SentenceModel::load(&err);
    if (!model) return fail(err);
    const std::vector<std::vector<float>> q = model->embed({query}, &err);
    if (q.empty() || q[0].empty()) return fail(err.empty() ? "The query could not be read" : err);
    // Embeddings not yet made, in batches (kept for next time).
    std::vector<size_t> missing;
    std::vector<std::string> keys(items.size());
    {
        std::lock_guard lock(cacheLock);
        for (size_t i = 0; i < items.size(); ++i) {
            keys[i] = cacheKey(items[i].passage.text);
            if (!cache().count(keys[i])) missing.push_back(i);
        }
    }
    constexpr size_t kChunk = 64;
    for (size_t c0 = 0; c0 < missing.size(); c0 += kChunk) {
        if (cancel && cancel->load()) return fail("Cancelled");
        std::vector<std::string> texts;
        for (size_t k = c0; k < std::min(missing.size(), c0 + kChunk); ++k) texts.push_back(items[missing[k]].passage.text);
        const auto embs = model->embed(texts, &err);
        if (embs.size() != texts.size()) return fail(err.empty() ? "The transcripts could not be read" : err);
        std::lock_guard lock(cacheLock);
        for (size_t k = 0; k < embs.size(); ++k) cache()[keys[missing[c0 + k]]] = embs[k];
        if (progress) progress(double(std::min(missing.size(), c0 + kChunk)) / double(missing.size()));
    }
    std::vector<SpokenHit> hits;
    {
        std::lock_guard lock(cacheLock);
        for (size_t i = 0; i < items.size(); ++i) {
            const std::vector<float>& e = cache()[keys[i]];
            double dot = 0;
            for (size_t d = 0; d < e.size() && d < q[0].size(); ++d) dot += double(e[d]) * q[0][d];
            SpokenHit h;
            h.media = items[i].media;
            h.start = items[i].passage.start, h.end = items[i].passage.end;
            h.text = items[i].passage.text;
            h.score = float(dot) + 0.1f * sharedWords(query, h.text);
            hits.push_back(std::move(h));
        }
    }
    if (progress) progress(1.0);
    return bestSpokenHits(std::move(hits), o.max, o.minScore);
}

}  // namespace montage
