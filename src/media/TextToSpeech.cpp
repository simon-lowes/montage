#include "TextToSpeech.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include "core/G2p.h"
#include "core/Zip.h"

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
const char* kKokoro = "https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/1939ad2a8e416c0acfeecc08a694d14ef25f2231/";
}

const std::vector<TtsVoice>& ttsVoices() {
    static const std::vector<TtsVoice> voices{
        {"af_heart", "Heart (US)", false, true},     {"af_bella", "Bella (US)", false, true},     {"af_sarah", "Sarah (US)", false, true},
        {"am_michael", "Michael (US)", false, false}, {"am_adam", "Adam (US)", false, false},     {"am_puck", "Puck (US)", false, false},
        {"bf_emma", "Emma (UK)", true, true},         {"bf_isabella", "Isabella (UK)", true, true}, {"bm_george", "George (UK)", true, false},
        {"bm_lewis", "Lewis (UK)", true, false}};
    return voices;
}

const TtsVoice* findTtsVoice(const std::string& id) {
    for (const TtsVoice& v : ttsVoices())
        if (v.id == id || QString::fromStdString(v.name).startsWith(QString::fromStdString(id), Qt::CaseInsensitive)) return &v;
    return nullptr;
}

const ModelPack& ttsModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "tts";
        p.title = "speech model";
        p.directoryEnv = "MONTAGE_TTS_MODEL";
        p.urlEnv = "MONTAGE_TTS_MODEL_URL";
        const std::string k = kKokoro;
        p.files = {
            {"kokoro-v1.0-fp16.onnx", k + "onnx/model_fp16.onnx", "ba4527a874b42b21e35f468c10d326fdff3c7fc8cac1f85e9eb6c0dfc35c334a", 163234740},
            {"tokenizer.json", k + "tokenizer.json", "77a02c8e164413299b4b4c403b14f8e0e1c1b727db4d46a09d6327b861060a34", 3497},
            // misaki's release (Apache-2.0) for its pronunciation dictionaries.
            {"misaki-0.7.4-py3-none-any.whl",
             "https://files.pythonhosted.org/packages/06/e9/f092172a37c0994bf1e57e0d2f3b16bd18f63b98744d6039f18a055c2ad5/"
             "misaki-0.7.4-py3-none-any.whl",
             "f9cf1afc0a2c0e77dadf02ae8203d3a7c312ee67b235d3b658c08367919b17c1", 3559559},
        };
        static const std::map<std::string, std::string> shas{
            {"af_heart", "d583ccff3cdca2f7fae535cb998ac07e9fcb90f09737b9a41fa2734ec44a8f0b"},
            {"af_bella", "f69d836209b78eb8c66e75e3cda491e26ea838a3674257e9d4e5703cbaf55c8b"},
            {"af_sarah", "4409fbc125afabacc615d94db5398d847006a737b0247d6892b7a9a0007a2f0a"},
            {"am_michael", "1d1f21dd8da39c30705cd4c75d039d265e9bc4a2a93ed09bc9e1b1225eb95ba1"},
            {"am_adam", "162b035ed91cfc48b6046982184c645f72edcdd1b82843347f605d7bf7b15716"},
            {"am_puck", "fcf73c989033e9233e0b98713eca600c8c74dcc1614b37009d5450ff4a2274a0"},
            {"bf_emma", "669fe0647f9dd04fcab92f1439a40eeb4c8b4ab1f82e4996fe3d918ce4a63b73"},
            {"bf_isabella", "3754352c4aaa46d17f27654ab7518d65b62ad6163a0f55a5f4330c2da2c4e94f"},
            {"bm_george", "c4b235a4c1f2cd3b939fed08b899ce9385638b763f7b73a59616c4fc9bd6c9bc"},
            {"bm_lewis", "b8f671cef828c30e66fdf0b0756a76bba58f6bb3398cbbf27058642acbcedb97"}};
        for (const TtsVoice& v : ttsVoices()) p.files.push_back({v.id + ".bin", k + "voices/" + v.id + ".bin", shas.at(v.id), 522240});
        return p;
    }();
    return pack;
}

namespace {

std::shared_ptr<const Lexicon> lexicon(bool british, std::string* error) {
    static std::mutex m;
    static std::shared_ptr<const Lexicon> cached[2];
    std::lock_guard lock(m);
    if (cached[british]) return cached[british];
    const ModelPack& pack = ttsModel();
    ZipReader wheel;
    if (!wheel.openFile(pack.path(pack.files[2]))) {
        if (error) *error = "The speech model is not downloaded";
        return nullptr;
    }
    std::string gold, silver;
    const std::string prefix = british ? "misaki/data/gb_" : "misaki/data/us_";
    if (!wheel.read(prefix + "gold.json", gold, error) || !wheel.read(prefix + "silver.json", silver, error)) return nullptr;
    cached[british] = Lexicon::fromJson(gold, silver, british, error);
    return cached[british];
}

}  // namespace

bool textToPhonemes(const std::string& text, bool british, std::string& out, std::string* error) {
    auto lex = lexicon(british, error);
    if (!lex) return false;
    out = lex->phonemize(text);
    return true;
}

bool writeSpeechWav(const std::string& path, const std::vector<float>& samples, std::string* error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(samples.size() * 2);
    f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(kTtsSampleRate), u32(kTtsSampleRate * 2), u16(2),
        u16(16), f.write("data", 4), u32(bytes);
    for (float s : samples) {
        const int16_t v = int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767));
        f.write(reinterpret_cast<const char*>(&v), 2);
    }
    return bool(f);
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool ttsAvailable() { return false; }
bool synthesizeSpeech(const std::string&, const std::string&, double, std::vector<float>&, std::string* error,
                      const std::function<void(double)>&, const std::atomic<bool>*) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool ttsAvailable() { return true; }

namespace {

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-tts"};
    std::unique_ptr<Ort::Session> session;
    std::map<QChar, int64_t> vocab;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = ttsModel();
    if (!pack.installed()) {
        if (error) *error = "The speech model is not downloaded";
        return nullptr;
    }
    auto model = std::make_shared<Model>();
    QFile tok(QString::fromStdString(pack.path(pack.files[1])));
    if (!tok.open(QIODevice::ReadOnly)) {
        if (error) *error = "The speech model's vocabulary is missing";
        return nullptr;
    }
    const QJsonObject vocab = QJsonDocument::fromJson(tok.readAll()).object().value("model").toObject().value("vocab").toObject();
    for (auto it = vocab.begin(); it != vocab.end(); ++it)
        if (it.key().size() == 1) model->vocab[it.key()[0]] = it.value().toInteger();
    if (model->vocab.size() < 50) {
        if (error) *error = "The speech model's vocabulary is damaged";
        return nullptr;
    }
    try {
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(pack.path(pack.files[0])).toStdWString();
#else
        const std::string path = pack.path(pack.files[0]);
#endif
        model->session = std::make_unique<Ort::Session>(model->env, path.c_str(), so);
        *cached = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The speech model could not be loaded: ") + e.what();
        return nullptr;
    }
}

// Text in pieces of at most `limit` phoneme tokens, split after sentences, then clauses, then words.
std::vector<QString> chunks(const QString& phonemes, int limit) {
    std::vector<QString> out;
    QString rest = phonemes.trimmed();
    while (rest.size() > limit) {
        int cut = -1;
        for (const QString& marks : {QStringLiteral(".!?"), QStringLiteral(";:—…"), QStringLiteral(","), QStringLiteral(" ")}) {
            for (int i = limit - 1; i > limit / 3; --i)
                if (marks.contains(rest[i])) {
                    cut = i + 1;
                    break;
                }
            if (cut > 0) break;
        }
        if (cut <= 0) cut = limit;
        out.push_back(rest.left(cut).trimmed());
        rest = rest.mid(cut).trimmed();
    }
    if (!rest.isEmpty()) out.push_back(rest);
    return out;
}

}  // namespace

bool synthesizeSpeech(const std::string& text, const std::string& voiceId, double speed, std::vector<float>& out, std::string* error,
                      const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    out.clear();
    const TtsVoice* voice = findTtsVoice(voiceId);
    if (!voice) {
        if (error) *error = "Unknown voice \"" + voiceId + "\"";
        return false;
    }
    auto model = loadModel(error);
    if (!model) return false;
    // The voice: a style vector for each length of input (510 x 256).
    std::vector<float> styles(510 * 256);
    {
        QFile f(QString::fromStdString(ttsModel().directory() + "/" + voice->id + ".bin"));
        if (!f.open(QIODevice::ReadOnly) || f.read(reinterpret_cast<char*>(styles.data()), qint64(styles.size() * 4)) != qint64(styles.size() * 4)) {
            if (error) *error = "The voice " + voice->name + " is missing";
            return false;
        }
    }
    std::string phonemes;
    if (!textToPhonemes(text, voice->british, phonemes, error)) return false;
    // A paragraph break is a longer pause.
    const std::vector<QString> pieces = chunks(QString::fromStdString(phonemes), 400);
    if (pieces.empty()) {
        if (error) *error = "Nothing to say";
        return false;
    }
    float sp = float(std::clamp(speed, 0.5, 2.0));
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        for (size_t n = 0; n < pieces.size(); ++n) {
            if (cancel && cancel->load()) {
                if (error) *error = "Cancelled";
                return false;
            }
            std::vector<int64_t> ids{0};
            for (QChar c : pieces[n])
                if (auto it = model->vocab.find(c); it != model->vocab.end()) ids.push_back(it->second);
            ids.push_back(0);
            const size_t tokens = ids.size() - 2;
            if (tokens == 0) continue;
            std::vector<float> style(styles.begin() + std::ptrdiff_t(std::min<size_t>(tokens, 509) * 256),
                                     styles.begin() + std::ptrdiff_t(std::min<size_t>(tokens, 509) * 256 + 256));
            const int64_t idShape[2] = {1, int64_t(ids.size())}, styleShape[2] = {1, 256}, speedShape[1] = {1};
            Ort::Value inputs[3] = {Ort::Value::CreateTensor<int64_t>(cpu, ids.data(), ids.size(), idShape, 2),
                                    Ort::Value::CreateTensor<float>(cpu, style.data(), style.size(), styleShape, 2),
                                    Ort::Value::CreateTensor<float>(cpu, &sp, 1, speedShape, 1)};
            const char* inNames[] = {"input_ids", "style", "speed"};
            const char* outNames[] = {"waveform"};
            auto result = model->session->Run(Ort::RunOptions{nullptr}, inNames, inputs, 3, outNames, 1);
            const float* w = result[0].GetTensorData<float>();
            const size_t count = result[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (!out.empty()) out.insert(out.end(), size_t(kTtsSampleRate * 0.12), 0.0f);  // between pieces
            out.insert(out.end(), w, w + count);
            if (progress) progress(double(n + 1) / pieces.size());
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Speech synthesis failed: ") + e.what();
        return false;
    }
    if (out.empty()) {
        if (error) *error = "Nothing to say";
        return false;
    }
    return true;
}

#endif

}  // namespace montage
