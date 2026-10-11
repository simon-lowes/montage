#include "Translator.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

namespace {

struct PairEntry {
    const char* from;
    const char* to;
    const char* repo;
    const char* revision;
    const char* prefix;  // a target-language token some models need first, e.g. ">>ara<<"
    struct File {
        const char* name;
        const char* path;
        const char* sha256;
        int64_t bytes;
    } files[5];
};

const PairEntry kPairs[] = {
#include "TranslationModels.inc"
};

const std::map<std::string, std::string>& names() {
    static const std::map<std::string, std::string> n = {
        {"ar", "Arabic"},    {"cs", "Czech"},      {"da", "Danish"},  {"de", "German"},     {"en", "English"},  {"es", "Spanish"},
        {"fi", "Finnish"},   {"fr", "French"},     {"hi", "Hindi"},   {"hu", "Hungarian"},  {"id", "Indonesian"}, {"it", "Italian"},
        {"ja", "Japanese"},  {"ko", "Korean"},     {"nl", "Dutch"},   {"pl", "Polish"},     {"ro", "Romanian"}, {"ru", "Russian"},
        {"sv", "Swedish"},   {"th", "Thai"},       {"tr", "Turkish"}, {"uk", "Ukrainian"},  {"vi", "Vietnamese"}, {"zh", "Chinese"}};
    return n;
}

const PairEntry* findPair(const std::string& from, const std::string& to) {
    for (const PairEntry& e : kPairs)
        if (from == e.from && to == e.to) return &e;
    return nullptr;
}

const std::map<std::pair<std::string, std::string>, ModelPack>& packs() {
    static const auto all = [] {
        std::map<std::pair<std::string, std::string>, ModelPack> m;
        for (const PairEntry& e : kPairs) {
            ModelPack p;
            p.id = std::string("translate-") + e.from + "-" + e.to;
            p.title = "translation model (" + translationLanguageName(e.from) + " to " + translationLanguageName(e.to) + ")";
            p.parentEnv = "MONTAGE_TRANSLATION_MODELS";
            for (const auto& f : e.files)
                p.files.push_back({f.name, std::string("https://huggingface.co/") + e.repo + "/resolve/" + e.revision + "/" + f.path, f.sha256,
                                   f.bytes});
            m[{e.from, e.to}] = std::move(p);
        }
        return m;
    }();
    return all;
}

// ---- Protobuf, just enough for a SentencePiece model ---------------------------

bool readVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7) {
        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

bool skipField(const uint8_t*& p, const uint8_t* end, int wire) {
    uint64_t v = 0;
    switch (wire) {
        case 0: return readVarint(p, end, v);
        case 1: p += 8; return p <= end;
        case 2:
            if (!readVarint(p, end, v) || v > uint64_t(end - p)) return false;
            p += v;
            return true;
        case 5: p += 4; return p <= end;
        default: return false;
    }
}

std::string utf8(const std::u32string& s, size_t from, size_t len) {
    return QString::fromUcs4(s.data() + from, qsizetype(len)).toStdString();
}

}  // namespace

const std::vector<TranslationLanguage>& translationLanguages() {
    static const std::vector<TranslationLanguage> langs = [] {
        std::vector<TranslationLanguage> l;
        std::vector<std::string> codes = {"en"};
        for (const PairEntry& e : kPairs)
            for (const char* c : {e.from, e.to})
                if (std::find(codes.begin(), codes.end(), c) == codes.end()) codes.push_back(c);
        for (const std::string& c : codes) l.push_back({c, translationLanguageName(c)});
        std::sort(l.begin(), l.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        return l;
    }();
    return langs;
}

std::string translationLanguageName(const std::string& code) {
    auto it = names().find(code);
    return it != names().end() ? it->second : code;
}

const ModelPack* translationModel(const std::string& from, const std::string& to) {
    auto it = packs().find({from, to});
    return it != packs().end() ? &it->second : nullptr;
}

std::vector<const ModelPack*> translationRoute(const std::string& from, const std::string& to) {
    if (from == to) return {};
    if (const ModelPack* direct = translationModel(from, to)) return {direct};
    const ModelPack* a = translationModel(from, "en");
    const ModelPack* b = translationModel("en", to);
    if (a && b) return {a, b};
    return {};
}

// ---- SentencePiece ------------------------------------------------------------

bool SentencePiece::load(const std::string& path, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot read " + path;
        return false;
    }
    const QByteArray data = f.readAll();
    const auto* p = reinterpret_cast<const uint8_t*>(data.constData());
    const uint8_t* end = p + data.size();
    pieces_.clear();
    minScore_ = std::numeric_limits<float>::max();
    maxChars_ = 1;
    while (p < end) {
        uint64_t key = 0;
        if (!readVarint(p, end, key)) break;
        const int field = int(key >> 3), wire = int(key & 7);
        if (field != 1 || wire != 2) {
            if (!skipField(p, end, wire)) break;
            continue;
        }
        uint64_t len = 0;
        if (!readVarint(p, end, len) || len > uint64_t(end - p)) break;
        const uint8_t* q = p;
        const uint8_t* qend = p + len;
        p = qend;
        std::string piece;
        float score = 0;
        uint64_t type = 1;
        while (q < qend) {
            uint64_t k = 0;
            if (!readVarint(q, qend, k)) break;
            const int fld = int(k >> 3), w = int(k & 7);
            if (fld == 1 && w == 2) {
                uint64_t n = 0;
                if (!readVarint(q, qend, n) || n > uint64_t(qend - q)) break;
                piece.assign(reinterpret_cast<const char*>(q), size_t(n));
                q += n;
            } else if (fld == 2 && w == 5) {
                std::memcpy(&score, q, 4);
                q += 4;
            } else if (fld == 3 && w == 0) {
                readVarint(q, qend, type);
            } else if (!skipField(q, qend, w)) {
                break;
            }
        }
        // Normal and user-defined pieces take part in splitting; control, unknown and byte pieces do not.
        if ((type == 1 || type == 4) && !piece.empty()) {
            pieces_[piece] = score;
            minScore_ = std::min(minScore_, score);
            maxChars_ = std::max(maxChars_, int(QString::fromStdString(piece).toUcs4().size()));
        }
    }
    if (pieces_.empty()) {
        if (error) *error = "Not a SentencePiece model: " + path;
        return false;
    }
    return true;
}

std::vector<std::string> SentencePiece::encode(const std::string& text) const {
    std::vector<std::string> out;
    // NFKC, whitespace collapsed; each word gets the "▁" a space becomes.
    const QString norm = QString::fromStdString(text).normalized(QString::NormalizationForm_KC).simplified();
    static const std::string kSpace = "\xE2\x96\x81";  // U+2581
    const float unkScore = minScore_ - 10;
    for (const QString& word : norm.split(' ', Qt::SkipEmptyParts)) {
        const std::u32string s = (QString::fromUtf8("▁") + word).toStdU32String();
        const size_t n = s.size();
        std::vector<float> best(n + 1, -std::numeric_limits<float>::infinity());
        std::vector<int> from(n + 1, -1);
        std::vector<char> unk(n + 1, 0);
        best[0] = 0;
        for (size_t i = 1; i <= n; ++i) {
            for (size_t len = 1; len <= std::min<size_t>(size_t(maxChars_), i); ++len) {
                if (best[i - len] == -std::numeric_limits<float>::infinity()) continue;
                auto it = pieces_.find(utf8(s, i - len, len));
                if (it == pieces_.end()) continue;
                const float v = best[i - len] + it->second;
                if (v > best[i]) best[i] = v, from[i] = int(i - len), unk[i] = 0;
            }
            // A character no piece covers is unknown.
            if (from[i] < 0 && best[i - 1] > -std::numeric_limits<float>::infinity()) {
                best[i] = best[i - 1] + unkScore;
                from[i] = int(i - 1);
                unk[i] = 1;
            }
        }
        std::vector<std::string> pieces;
        for (size_t i = n; i > 0; i = size_t(from[i])) {
            if (unk[i]) {
                if (pieces.empty() || pieces.back() != "<unk>") pieces.push_back("<unk>");
            } else {
                pieces.push_back(utf8(s, size_t(from[i]), i - size_t(from[i])));
            }
        }
        std::reverse(pieces.begin(), pieces.end());
        out.insert(out.end(), pieces.begin(), pieces.end());
    }
    return out;
}

// ---- Translation --------------------------------------------------------------

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool translatorAvailable() { return false; }
bool translateTexts(const std::vector<std::string>&, const std::string&, const std::string&, std::vector<std::string>&,
                    const std::function<void(double)>&, const std::atomic<bool>*, std::string* error) {
    if (error) *error = "This build of Montage cannot translate (it was built without ONNX Runtime)";
    return false;
}

#else

bool translatorAvailable() { return true; }

namespace {

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-translate"};
    std::unique_ptr<Ort::Session> encoder, decoder;
    SentencePiece source;
    std::map<std::string, int64_t> ids;
    std::vector<std::string> pieces;  // by id
    int64_t eos = 0, pad = 0, unk = 1, prefix = -1;
};

std::shared_ptr<Model> loadModel(const std::string& from, const std::string& to, std::string* error) {
    static std::mutex m;
    static auto* cache = new std::vector<std::pair<std::string, std::shared_ptr<Model>>>();  // the last two pairs used
    const std::string key = from + ">" + to;
    std::lock_guard lock(m);
    for (auto& [k, v] : *cache)
        if (k == key) return v;
    if (!ortUsable(error)) return nullptr;
    const ModelPack* pack = translationModel(from, to);
    const PairEntry* entry = findPair(from, to);
    if (!pack || !entry) {
        if (error) *error = "There is no model for " + translationLanguageName(from) + " to " + translationLanguageName(to);
        return nullptr;
    }
    if (!pack->installed()) {
        if (error) *error = "The " + pack->title + " is not downloaded";
        return nullptr;
    }
    auto model = std::make_shared<Model>();
    const auto file = [&](const char* name) {
        for (const ModelFile& f : pack->files)
            if (f.name == name) return pack->path(f);
        return std::string();
    };
    if (!model->source.load(file("source.spm"), error)) return nullptr;
    {
        QFile f(QString::fromStdString(file("vocab.json")));
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = "Cannot read the translation vocabulary";
            return nullptr;
        }
        const QJsonObject vocab = QJsonDocument::fromJson(f.readAll()).object();
        model->pieces.resize(size_t(vocab.size()));
        for (auto it = vocab.begin(); it != vocab.end(); ++it) {
            const int64_t id = it.value().toInteger();
            model->ids[it.key().toStdString()] = id;
            if (id >= 0 && size_t(id) < model->pieces.size()) model->pieces[size_t(id)] = it.key().toStdString();
        }
        if (auto u = model->ids.find("<unk>"); u != model->ids.end()) model->unk = u->second;
    }
    {
        QFile f(QString::fromStdString(file("config.json")));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject c = QJsonDocument::fromJson(f.readAll()).object();
            model->eos = c.value("eos_token_id").toInteger(0);
            model->pad = c.value("decoder_start_token_id").toInteger(c.value("pad_token_id").toInteger(int64_t(model->pieces.size()) - 1));
        }
    }
    if (*entry->prefix) {
        auto it = model->ids.find(entry->prefix);
        if (it != model->ids.end()) model->prefix = it->second;
    }
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency() / 2)));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto open = [&](const char* name) {
#ifdef _WIN32
            const std::wstring path = QString::fromStdString(file(name)).toStdWString();
#else
            const std::string path = file(name);
#endif
            return std::make_unique<Ort::Session>(model->env, path.c_str(), so);
        };
        model->encoder = open("encoder.onnx");
        model->decoder = open("decoder.onnx");
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The translation model could not be loaded: ") + e.what();
        return nullptr;
    }
    cache->insert(cache->begin(), {key, model});
    if (cache->size() > 2) cache->pop_back();
    return model;
}

// One text through one model: greedy decoding (the decoder runs over what it has so far each step).
bool translateOne(Model& m, const std::string& text, std::string& out, std::string* error) {
    out.clear();
    std::vector<int64_t> ids;
    if (m.prefix >= 0) ids.push_back(m.prefix);
    for (const std::string& piece : m.source.encode(text)) {
        auto it = m.ids.find(piece);
        ids.push_back(it != m.ids.end() ? it->second : m.unk);
    }
    if (ids.empty() || (ids.size() == 1 && m.prefix >= 0)) return true;
    // Very long texts are cut where the model's positions end.
    if (ids.size() > 500) ids.resize(500);
    ids.push_back(m.eos);
    const int64_t T = int64_t(ids.size());
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> mask(ids.size(), 1);
        const int64_t inShape[2] = {1, T};
        Ort::Value encIn[2] = {Ort::Value::CreateTensor<int64_t>(cpu, ids.data(), ids.size(), inShape, 2),
                               Ort::Value::CreateTensor<int64_t>(cpu, mask.data(), mask.size(), inShape, 2)};
        const char* encNames[] = {"input_ids", "attention_mask"};
        const char* encOut[] = {"last_hidden_state"};
        auto hidden = m.encoder->Run(Ort::RunOptions{nullptr}, encNames, encIn, 2, encOut, 1);
        std::vector<int64_t> dec{m.pad};
        const size_t maxLen = size_t(std::min<int64_t>(512, 2 * T + 16));
        const char* decNames[] = {"encoder_attention_mask", "input_ids", "encoder_hidden_states"};
        const char* decOut[] = {"logits"};
        while (dec.size() < maxLen) {
            const int64_t dShape[2] = {1, int64_t(dec.size())};
            Ort::Value decIn[3] = {Ort::Value::CreateTensor<int64_t>(cpu, mask.data(), mask.size(), inShape, 2),
                                   Ort::Value::CreateTensor<int64_t>(cpu, dec.data(), dec.size(), dShape, 2), std::move(hidden[0])};
            auto logits = m.decoder->Run(Ort::RunOptions{nullptr}, decNames, decIn, 3, decOut, 1);
            hidden[0] = std::move(decIn[2]);
            const auto shape = logits[0].GetTensorTypeAndShapeInfo().GetShape();
            const int64_t V = shape[2];
            const float* last = logits[0].GetTensorData<float>() + (shape[1] - 1) * V;
            int64_t best = -1;
            float bestV = -std::numeric_limits<float>::infinity();
            for (int64_t v = 0; v < V; ++v)
                if (v != m.pad && last[v] > bestV) bestV = last[v], best = v;
            dec.push_back(best);
            if (best == m.eos) break;
        }
        std::string joined;
        for (size_t i = 1; i < dec.size(); ++i) {
            const int64_t id = dec[i];
            if (id == m.eos || id == m.pad || id < 0 || size_t(id) >= m.pieces.size()) continue;
            const std::string& piece = m.pieces[size_t(id)];
            if (piece.size() > 4 && piece.rfind(">>", 0) == 0) continue;  // a language tag
            joined += piece;
        }
        QString s = QString::fromStdString(joined);
        s.replace(QChar(0x2581), QChar(' '));
        out = s.simplified().toStdString();
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Translation failed: ") + e.what();
        return false;
    }
}

}  // namespace

bool translateTexts(const std::vector<std::string>& in, const std::string& from, const std::string& to, std::vector<std::string>& out,
                    const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    out = in;
    if (from == to) return true;
    const auto route = translationRoute(from, to);
    if (route.empty()) {
        if (error) *error = "There is no model for " + translationLanguageName(from) + " to " + translationLanguageName(to);
        return false;
    }
    std::vector<std::pair<std::string, std::string>> steps;
    if (route.size() == 1) steps = {{from, to}};
    else steps = {{from, "en"}, {"en", to}};
    for (size_t k = 0; k < steps.size(); ++k) {
        auto model = loadModel(steps[k].first, steps[k].second, error);
        if (!model) return false;
        for (size_t i = 0; i < out.size(); ++i) {
            if (cancel && cancel->load()) return false;
            if (progress) progress((double(k) + double(i) / double(std::max<size_t>(1, out.size()))) / double(steps.size()));
            std::string t;
            if (!translateOne(*model, out[i], t, error)) return false;
            out[i] = t;
        }
    }
    if (progress) progress(1);
    return true;
}

#endif

}  // namespace montage
