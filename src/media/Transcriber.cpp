#include "Transcriber.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef MONTAGE_WITH_WHISPER
#include <whisper.h>
#endif

#include "Decoder.h"
#include "Diarizer.h"

namespace montage {

const std::vector<WhisperModel>& whisperModels() {
    static const std::vector<WhisperModel> models = {
        {"tiny.en", "Tiny (English, fastest)", 77704715, true},
        {"base.en", "Base (English)", 147964211, true},
        {"small.en", "Small (English, accurate)", 487614201, true},
        {"tiny", "Tiny (99 languages)", 77691713, false},
        {"base", "Base (99 languages)", 147951465, false},
        {"small", "Small (99 languages)", 487601967, false},
        {"large-v3-turbo-q5_0", "Large v3 Turbo, compact (99 languages, best quality per size)", 574041195, false},
        {"large-v3-turbo", "Large v3 Turbo (99 languages, most accurate)", 1624555275, false},
    };
    return models;
}

std::string whisperModelsDirectory() {
    if (const char* env = std::getenv("MONTAGE_WHISPER_MODELS"); env && *env) return env;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.montage";
    return (dir + "/whisper-models").toStdString();
}

std::string whisperModelPath(const std::string& nameOrPath) {
    if (QFileInfo::exists(QString::fromStdString(nameOrPath)) && QFileInfo(QString::fromStdString(nameOrPath)).isFile())
        return nameOrPath;
    const QString file = QString::fromStdString(whisperModelsDirectory()) + "/ggml-" + QString::fromStdString(nameOrPath) + ".bin";
    return QFileInfo::exists(file) ? file.toStdString() : std::string();
}

std::string whisperModelUrl(const std::string& name) {
    // $MONTAGE_WHISPER_MODEL_URL replaces the download folder (a mirror, or file:// in tests).
    std::string base = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main";
    if (const char* env = std::getenv("MONTAGE_WHISPER_MODEL_URL"); env && *env) base = env;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/ggml-" + name + ".bin";
}

bool isWhisperModelFile(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray magic = f.read(4);
    return magic == QByteArray("lmgg", 4);  // GGML_FILE_MAGIC 0x67676d6c, little-endian
}

#ifndef MONTAGE_WITH_WHISPER

bool transcribeSamples(const std::vector<float>&, const TranscribeOptions&, Transcript&, const TranscribeProgress&,
                       const std::atomic<bool>*, std::string* error) {
    if (error) *error = "This build of Montage has no speech recognition";
    return false;
}

#else

namespace {

// Loading a model takes a moment; keep the last one.
std::mutex gModelMutex;
whisper_context* gContext = nullptr;
std::string gContextPath;

void quietLog(ggml_log_level, const char*, void*) {}

struct Callbacks {
    const TranscribeProgress* progress;
    const std::atomic<bool>* cancel;
};

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\n");
    return s.substr(first, last - first + 1);
}

}  // namespace

bool transcribeSamples(const std::vector<float>& mono16k, const TranscribeOptions& options, Transcript& out,
                       const TranscribeProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const std::string modelPath = whisperModelPath(options.model);
    if (modelPath.empty()) return fail("Speech model not found: " + options.model + " (download it first)");
    if (mono16k.size() < 1600) return fail("Not enough audio to transcribe");

    std::lock_guard lock(gModelMutex);  // one transcription at a time per process
    whisper_log_set(&quietLog, nullptr);
    if (!gContext || gContextPath != modelPath) {
        if (gContext) whisper_free(gContext);
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        gContext = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
        gContextPath = gContext ? modelPath : std::string();
        if (!gContext) return fail("Could not load the speech model " + modelPath);
    }
    whisper_context* ctx = gContext;

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = options.threads > 0 ? options.threads
                                           : int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
    params.translate = options.translate;
    // English-only models cannot detect a language; asking them to guesses nonsense.
    const bool multilingual = whisper_is_multilingual(ctx) != 0;
    params.language = !multilingual ? "en" : options.language.empty() ? "auto" : options.language.c_str();
    params.detect_language = false;
    params.token_timestamps = true;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    Callbacks cb{&progress, cancel};
    params.progress_callback = [](whisper_context*, whisper_state*, int percent, void* user) {
        auto* c = static_cast<Callbacks*>(user);
        if (*c->progress) (*c->progress)(std::clamp(percent / 100.0, 0.0, 1.0));
    };
    params.progress_callback_user_data = &cb;
    params.abort_callback = [](void* user) {
        auto* c = static_cast<Callbacks*>(user);
        return c->cancel && c->cancel->load();
    };
    params.abort_callback_user_data = &cb;

    if (whisper_full(ctx, params, mono16k.data(), int(mono16k.size())) != 0)
        return fail(cancel && cancel->load() ? "Transcription cancelled" : "Transcription failed");
    if (cancel && cancel->load()) return fail("Transcription cancelled");

    Transcript t;
    t.model = QFileInfo(QString::fromStdString(modelPath)).completeBaseName().remove(QStringLiteral("ggml-")).toStdString();
    const int lang = multilingual ? whisper_full_lang_id(ctx) : whisper_lang_id("en");
    if (lang >= 0) t.language = options.translate ? "en" : whisper_lang_str(lang);
    const whisper_token eot = whisper_token_eot(ctx);
    const int segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < segments; ++i) {
        TranscriptSegment seg;
        seg.start = double(whisper_full_get_segment_t0(ctx, i)) / 100.0;
        seg.end = double(whisper_full_get_segment_t1(ctx, i)) / 100.0;
        seg.text = trim(whisper_full_get_segment_text(ctx, i));
        const int tokens = whisper_full_n_tokens(ctx, i);
        for (int j = 0; j < tokens; ++j) {
            const whisper_token_data d = whisper_full_get_token_data(ctx, i, j);
            if (d.id >= eot) continue;  // special and timestamp tokens
            const std::string text = whisper_full_get_token_text(ctx, i, j);
            if (text.empty()) continue;
            const double t0 = std::max(seg.start, double(d.t0) / 100.0), t1 = std::max(t0, double(d.t1) / 100.0);
            // A token starting with a space begins a new word.
            if (seg.words.empty() || text.front() == ' ') {
                TranscriptWord w;
                w.text = trim(text);
                w.start = t0;
                w.end = t1;
                w.probability = d.p;
                if (!w.text.empty()) seg.words.push_back(w);
            } else {
                seg.words.back().text += text;
                seg.words.back().end = std::max(seg.words.back().end, t1);
                seg.words.back().probability = std::min(seg.words.back().probability, d.p);
            }
        }
        if (!seg.text.empty()) t.segments.push_back(std::move(seg));
    }
    out = std::move(t);
    return true;
}

#endif  // MONTAGE_WITH_WHISPER

bool transcribeMedia(const std::string& path, const TranscribeOptions& options, Transcript& out,
                     const TranscribeProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    AudioBufferPtr audio = decodeAudio(path, 16000, error, cancel);
    if (!audio || audio->samples.empty()) {
        if (error && error->empty()) *error = "The media has no audio";
        return false;
    }
    std::vector<float> mono(size_t(audio->frames()));
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (audio->samples[i * 2] + audio->samples[i * 2 + 1]);
    if (!options.speakers) return transcribeSamples(mono, options, out, progress, cancel, error);
    // Words first (most of the time), then who says them.
    auto part = [&](double from, double to) -> TranscribeProgress {
        if (!progress) return {};
        return [=](double f) { progress(from + (to - from) * f); };
    };
    Transcript t;
    if (!transcribeSamples(mono, options, t, part(0, 0.85), cancel, error)) return false;
    DiarizeOptions d;
    d.speakers = options.speakerCount;
    std::vector<SpeakerTurn> turns;
    if (!diarize(mono, d, turns, part(0.85, 1), cancel, error)) return false;
    applySpeakers(t, turns);
    out = std::move(t);
    return true;
}

}  // namespace montage
