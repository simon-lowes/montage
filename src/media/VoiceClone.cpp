#include "VoiceClone.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "MediaPool.h"
#include "audio/TimeStretch.h"
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

// The ONNX export of Chatterbox Multilingual (onnx-community, MIT), at a pinned revision.
const char* kChatterbox = "https://huggingface.co/onnx-community/chatterbox-multilingual-ONNX/resolve/452d3f434aa592098f1eedac9099f33642ab2da5/";

constexpr int64_t kStartSpeech = 6561, kStopSpeech = 6562, kExaggerationToken = 6563, kBos = 255, kEos = 0;

// The UTF-8 sequence starting at s[i]: its length in bytes.
size_t utf8Length(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    return std::min(n, s.size() - i);
}

}  // namespace

const ModelPack& voiceCloneModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "voice-clone";
        p.title = "voice cloning model";
        p.directoryEnv = "MONTAGE_VOICE_CLONE_MODEL";
        p.urlEnv = "MONTAGE_VOICE_CLONE_MODEL_URL";
        const std::string b = kChatterbox;
        p.files = {
            {"speech_encoder.onnx", b + "onnx/speech_encoder.onnx", "8f1c8a0f89b77bf9cd5dd8f2e034eb2c79dc00fe70d41196b28c257643b00ccb", 1184608},
            {"speech_encoder.onnx_data", b + "onnx/speech_encoder.onnx_data", "92f8f290fc9720e169bc2412c507209e20b03f6564bc3243739e25c56f7dfb8f",
             591274880},
            {"embed_tokens.onnx", b + "onnx/embed_tokens.onnx", "f785819ca4f6271262d5bb8971d62796c3a909e3b031982c113dbe83a4c3b854", 13286},
            {"embed_tokens.onnx_data", b + "onnx/embed_tokens.onnx_data", "2a15f7dd73b2ee47f6edf87740324011594b5a528ed6471ae55e327ed6cad68c",
             68390912},
            {"language_model_q4.onnx", b + "onnx/language_model_q4.onnx", "7f8cdca83b2493536cbf3acf421199808a3d68736f55f4eabd20ef8a99da4313", 227911},
            {"language_model_q4.onnx_data", b + "onnx/language_model_q4.onnx_data",
             "e79ab8784122a501718868b9631ff46e151c552d9b24e50f25d721f375e3526c", 353621248},
            {"conditional_decoder.onnx", b + "onnx/conditional_decoder.onnx", "1656d0d31332bae1854839959a3139300ebb67c178651dfa3f8c5fbfa5351351",
             6350448},
            {"conditional_decoder.onnx_data", b + "onnx/conditional_decoder.onnx_data",
             "51d58345a272747665ec9d5bb61e01835258a940e321a288582ac4c18cf01b5a", 533970816},
            {"tokenizer.json", b + "tokenizer.json", "29d48c4a178f6af3ad5130097c34744639e9294847b38a7b912c8c68027cb819", 71798},
            {"Cangjie5_TC.json", b + "Cangjie5_TC.json", "7073fd9de919443ae88e0bd2449917a65fe54898a4413ed1edcc4b67f28bce8c", 1920163},
        };
        return p;
    }();
    return pack;
}

const std::vector<CloneLanguage>& voiceCloneLanguages() {
    static const std::vector<CloneLanguage> langs{
        {"ar", "Arabic"},  {"da", "Danish"}, {"de", "German"},   {"el", "Greek"},      {"en", "English"}, {"es", "Spanish"},
        {"fi", "Finnish"}, {"fr", "French"}, {"he", "Hebrew"},   {"hi", "Hindi"},      {"it", "Italian"}, {"ja", "Japanese"},
        {"ko", "Korean"},  {"ms", "Malay"},  {"nl", "Dutch"},    {"no", "Norwegian"},  {"pl", "Polish"},  {"pt", "Portuguese"},
        {"ru", "Russian"}, {"sv", "Swedish"}, {"sw", "Swahili"}, {"tr", "Turkish"},    {"zh", "Chinese"}};
    return langs;
}

const CloneLanguage* findCloneLanguage(const std::string& codeOrName) {
    const QString want = QString::fromStdString(codeOrName).trimmed();
    if (want.isEmpty()) return nullptr;
    for (const CloneLanguage& l : voiceCloneLanguages())
        if (want.compare(QLatin1String(l.code), Qt::CaseInsensitive) == 0 || want.compare(QLatin1String(l.name), Qt::CaseInsensitive) == 0)
            return &l;
    // "pt-BR", "en_GB": the language part.
    const QString base = want.section(QRegularExpression(QStringLiteral("[-_]")), 0, 0);
    if (base != want) return findCloneLanguage(base.toStdString());
    return nullptr;
}

// ---------------------------------------------------------------------------
// Tokenizer

bool CloneTokenizer::load(const std::string& tokenizerJson, const std::string& cangjieJson, std::string* error) {
    vocab_.clear();
    ranks_.clear();
    added_.clear();
    cangjie_.clear();
    QFile f(QString::fromStdString(tokenizerJson));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "The voice model's tokenizer is missing";
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject model = root.value("model").toObject();
    const QJsonObject vocab = model.value("vocab").toObject();
    for (auto it = vocab.begin(); it != vocab.end(); ++it) vocab_[it.key().toStdString()] = it.value().toInteger();
    int rank = 0;
    for (const auto& m : model.value("merges").toArray()) {
        std::string a, b;
        if (m.isString()) {
            const QString s = m.toString();
            const int space = s.indexOf(' ');
            if (space <= 0) continue;
            a = s.left(space).toStdString();
            b = s.mid(space + 1).toStdString();
        } else {
            const QJsonArray pair = m.toArray();
            if (pair.size() != 2) continue;
            a = pair.at(0).toString().toStdString();
            b = pair.at(1).toString().toStdString();
        }
        ranks_.emplace(std::make_pair(a, b), rank++);
    }
    for (const auto& t : root.value("added_tokens").toArray()) {
        const QJsonObject o = t.toObject();
        const std::string content = o.value("content").toString().toStdString();
        if (!content.empty()) added_.emplace_back(content, o.value("id").toInteger());
    }
    std::stable_sort(added_.begin(), added_.end(), [](const auto& x, const auto& y) { return x.first.size() > y.first.size(); });
    if (const auto u = vocab_.find("[UNK]"); u != vocab_.end()) unk_ = u->second;
    if (vocab_.size() < 100) {
        vocab_.clear();
        if (error) *error = "The voice model's tokenizer is damaged";
        return false;
    }
    // Chinese is read as Cangjie codes: each character's code (its last entry in the table), with its place among the
    // characters sharing that code when it is not the first.
    if (!cangjieJson.empty()) {
        QFile c(QString::fromStdString(cangjieJson));
        if (c.open(QIODevice::ReadOnly)) {
            std::map<uint32_t, std::string> code;
            std::map<std::string, std::vector<uint32_t>> byCode;
            for (const auto& e : QJsonDocument::fromJson(c.readAll()).array()) {
                const QStringList parts = e.toString().split('\t');
                if (parts.size() < 2) continue;
                const QList<uint> cps = parts[0].toUcs4();
                if (cps.size() != 1) continue;
                const std::string cj = parts[1].toStdString();
                code[cps[0]] = cj;
                byCode[cj].push_back(cps[0]);
            }
            for (const auto& [cp, cj] : code) {
                const auto& list = byCode[cj];
                const auto at = std::find(list.begin(), list.end(), cp) - list.begin();
                cangjie_[cp] = at > 0 ? cj + std::to_string(at) : cj;
            }
        }
    }
    return true;
}

int64_t CloneTokenizer::id(const std::string& token) const {
    const auto it = vocab_.find(token);
    if (it != vocab_.end()) return it->second;
    for (const auto& [content, id] : added_)
        if (content == token) return id;
    return -1;
}

std::string CloneTokenizer::prepare(const std::string& text, const std::string& language) const {
    QString out;
    auto put = [&out](uint cp) {
        const char32_t c = char32_t(cp);
        out += QString::fromUcs4(&c, 1);
    };
    // As the model was trained (Chatterbox's MTLTokenizer): lower case, then NFKD (accents and Japanese voicing marks
    // apart, as the vocabulary holds them).
    const QString in = QString::fromStdString(text).toLower().normalized(QString::NormalizationForm_KD);
    if (language == "ko") {
        // Hangul syllables as their jamo (lead, vowel, tail).
        for (const uint cp : in.toUcs4()) {
            if (cp >= 0xAC00 && cp <= 0xD7A3) {
                const uint base = cp - 0xAC00;
                put(0x1100 + base / (21 * 28));
                put(0x1161 + (base % (21 * 28)) / 28);
                if (base % 28) put(0x11A7 + base % 28);
            } else {
                put(cp);
            }
        }
        out = out.trimmed();
    } else if (language == "zh" && !cangjie_.empty()) {
        for (const uint cp : in.toUcs4()) {
            const auto it = QChar::category(char32_t(cp)) == QChar::Letter_Other ? cangjie_.find(cp) : cangjie_.end();
            if (it == cangjie_.end()) {
                put(cp);
                continue;
            }
            for (const char c : it->second) out += QStringLiteral("[cj_%1]").arg(QChar(c));
            out += QStringLiteral("[cj_.]");
        }
    } else {
        out = in;
    }
    return "[" + language + "]" + out.toStdString();
}

std::vector<int64_t> CloneTokenizer::bpe(const std::string& word) const {
    std::vector<std::string> sym;
    for (size_t i = 0; i < word.size();) {
        const size_t n = utf8Length(word, i);
        sym.push_back(word.substr(i, n));
        i += n;
    }
    while (sym.size() > 1) {
        int best = -1;
        for (size_t i = 0; i + 1 < sym.size(); ++i)
            if (const auto r = ranks_.find({sym[i], sym[i + 1]}); r != ranks_.end() && (best < 0 || r->second < best)) best = r->second;
        if (best < 0) break;
        std::vector<std::string> next;
        for (size_t i = 0; i < sym.size(); ++i) {
            if (i + 1 < sym.size()) {
                const auto r = ranks_.find({sym[i], sym[i + 1]});
                if (r != ranks_.end() && r->second == best) {
                    next.push_back(sym[i] + sym[i + 1]);
                    ++i;
                    continue;
                }
            }
            next.push_back(sym[i]);
        }
        sym = std::move(next);
    }
    std::vector<int64_t> ids;
    for (const std::string& s : sym) {
        const auto it = vocab_.find(s);
        ids.push_back(it == vocab_.end() ? unk_ : it->second);
    }
    return ids;
}

std::vector<int64_t> CloneTokenizer::encode(const std::string& text, const std::string& language) const {
    // As the model's tokenizer.json: spaces become [SPACE], added tokens are taken whole (longest first), the rest is
    // split into runs of word characters and of other marks, and each run merged by BPE.
    std::string s;
    for (const char c : prepare(text, language)) s += c == ' ' ? std::string("[SPACE]") : std::string(1, c);
    static const QRegularExpression words(QStringLiteral("\\w+|[^\\w\\s]+"), QRegularExpression::UseUnicodePropertiesOption);
    std::vector<int64_t> ids{kExaggerationToken, kBos};
    std::string pending;
    auto flush = [&] {
        if (pending.empty()) return;
        auto it = words.globalMatch(QString::fromStdString(pending));
        while (it.hasNext()) {
            const std::vector<int64_t> w = bpe(it.next().captured(0).toStdString());
            ids.insert(ids.end(), w.begin(), w.end());
        }
        pending.clear();
    };
    for (size_t i = 0; i < s.size();) {
        bool taken = false;
        for (const auto& [content, id] : added_)
            if (s.compare(i, content.size(), content) == 0) {
                flush();
                ids.push_back(id);
                i += content.size();
                taken = true;
                break;
            }
        if (taken) continue;
        const size_t n = utf8Length(s, i);
        pending.append(s, i, n);
        i += n;
    }
    flush();
    ids.insert(ids.end(), {kEos, kStartSpeech, kStartSpeech});
    return ids;
}

// ---------------------------------------------------------------------------
// Text

std::string normalizeCloneText(const std::string& text) {
    QString t = QString::fromStdString(text).simplified();
    if (t.isEmpty()) return {};
    if (t[0].isLower()) t[0] = t[0].toUpper();
    static const std::pair<const char*, const char*> replace[] = {
        {"...", ", "}, {"…", ", "}, {":", ","}, {" - ", ", "}, {";", ", "}, {"—", "-"}, {"–", "-"},
        {" ,", ","},   {"“", "\""}, {"”", "\""}, {"‘", "'"}, {"’", "'"}};
    for (const auto& [from, to] : replace) t.replace(QString::fromUtf8(from), QString::fromUtf8(to));
    t = t.simplified();  // ("; " made ",  ")
    static const QStringList enders{QStringLiteral("."),      QStringLiteral("!"),      QStringLiteral("?"),     QStringLiteral("-"),
                                    QStringLiteral(","),      QString::fromUtf8("、"), QString::fromUtf8("，"), QString::fromUtf8("。"),
                                    QString::fromUtf8("？"), QString::fromUtf8("！")};
    if (std::none_of(enders.begin(), enders.end(), [&](const QString& e) { return t.endsWith(e); })) t += '.';
    return t.toStdString();
}

std::vector<std::string> splitCloneText(const std::string& text, int limit) {
    std::vector<std::string> out;
    QString rest = QString::fromStdString(text).trimmed();
    limit = std::max(limit, 20);
    while (rest.size() > limit) {
        int cut = -1;
        for (const QString& marks : {QString::fromUtf8(".!?。！？"), QString::fromUtf8(",;，、"), QStringLiteral(" ")}) {
            for (int i = limit - 1; i > limit / 3; --i)
                if (marks.contains(rest[i])) {
                    cut = i + 1;
                    break;
                }
            if (cut > 0) break;
        }
        if (cut <= 0) cut = limit;
        out.push_back(rest.left(cut).trimmed().toStdString());
        rest = rest.mid(cut).trimmed();
    }
    if (!rest.isEmpty()) out.push_back(rest.toStdString());
    return out;
}

// ---------------------------------------------------------------------------
// The reference

static std::vector<float> resampleForVoice(const std::vector<float>& mono, int rate) {
    if (mono.empty() || rate <= 0) return {};
    if (rate == kCloneSampleRate) return mono;
    // Windowed-sinc resampling, low-passed below the lower Nyquist.
    const double ratio = double(kCloneSampleRate) / rate, cutoff = std::min(1.0, ratio);
    const int taps = 16;
    std::vector<float> x(size_t(double(mono.size()) * ratio));
    for (size_t i = 0; i < x.size(); ++i) {
        const double at = double(i) / ratio;
        const long centre = long(std::floor(at));
        double acc = 0, norm = 0;
        for (long k = centre - taps + 1; k <= centre + taps; ++k) {
            if (k < 0 || k >= long(mono.size())) continue;
            const double d = (at - double(k)) * cutoff;
            const double sinc = std::abs(d) < 1e-9 ? 1.0 : std::sin(M_PI * d) / (M_PI * d);
            const double w = 0.5 + 0.5 * std::cos(M_PI * (at - double(k)) / taps);
            acc += mono[size_t(k)] * sinc * w;
            norm += sinc * w;
        }
        x[i] = norm != 0 ? float(acc / norm) : 0.0f;
    }
    return x;
}

std::vector<float> cloneReference(const std::vector<float>& mono, int rate) {
    std::vector<float> x = resampleForVoice(mono, rate);
    if (x.empty()) return x;
    // Quiet edges off (below -40 dB of the peak, in 20 ms windows).
    float peak = 0;
    for (const float v : x) peak = std::max(peak, std::fabs(v));
    if (peak <= 0) return {};
    const size_t win = kCloneSampleRate / 50;
    auto loud = [&](size_t at) {
        float m = 0;
        for (size_t i = at; i < std::min(x.size(), at + win); ++i) m = std::max(m, std::fabs(x[i]));
        return m > peak * 0.01f;
    };
    size_t from = 0, to = x.size();
    while (from < to && !loud(from)) from += win;
    while (to > from + win && !loud(to - win)) to -= win;
    x = std::vector<float>(x.begin() + std::ptrdiff_t(from), x.begin() + std::ptrdiff_t(to));
    // At most ten seconds: the loudest stretch (most speech).
    const size_t most = size_t(kCloneSampleRate) * 10;
    if (x.size() > most) {
        double sum = 0, best = -1;
        size_t at = 0;
        for (size_t i = 0; i < most; ++i) sum += double(x[i]) * x[i];
        best = sum;
        const size_t step = kCloneSampleRate / 10;
        for (size_t start = step; start + most <= x.size(); start += step) {
            for (size_t i = start - step; i < start; ++i) sum -= double(x[i]) * x[i];
            for (size_t i = start + most - step; i < start + most; ++i) sum += double(x[i]) * x[i];
            if (sum > best) best = sum, at = start;
        }
        x = std::vector<float>(x.begin() + std::ptrdiff_t(at), x.begin() + std::ptrdiff_t(at + most));
    }
    return x;
}

bool voiceSampleFrom(const std::string& path, double from, double to, std::vector<float>& mono, std::string* error,
                     const std::vector<int>& channels, int rate) {
    if (!voiceSoundFrom(path, from, to, mono, error, channels, rate)) return false;
    mono = cloneReference(mono, kCloneSampleRate);
    if (mono.empty()) {
        if (error) *error = "The voice sample is silent";
        return false;
    }
    return true;
}

bool voiceSoundFrom(const std::string& path, double from, double to, std::vector<float>& mono, std::string* error,
                    const std::vector<int>& channels, int rate) {
    mono.clear();
    const AudioBufferPtr sound = MediaPool::instance().audio(audioKey(path, channels), rate);
    if (!sound || sound->frames() == 0) {
        if (error) *error = "No sound in " + path;
        return false;
    }
    const int64_t frames = sound->frames();
    const int64_t a = std::clamp<int64_t>(int64_t(std::llround(std::max(0.0, from) * sound->sampleRate)), 0, frames);
    const int64_t b = to > from ? std::clamp<int64_t>(int64_t(std::llround(to * sound->sampleRate)), a, frames) : frames;
    if (b <= a) {
        if (error) *error = "The voice sample is outside the recording";
        return false;
    }
    const size_t ch = size_t(std::max(1, sound->channels));
    std::vector<float> mixed(size_t(b - a));
    for (size_t i = 0; i < mixed.size(); ++i) {
        float v = 0;
        for (size_t c = 0; c < ch; ++c) v += sound->samples[(size_t(a) + i) * ch + c];
        mixed[i] = v / float(ch);
    }
    mono = resampleForVoice(mixed, sound->sampleRate);
    return true;
}

std::vector<size_t> voiceConversionCuts(const std::vector<float>& mono, int rate, double most) {
    std::vector<size_t> cuts{0};
    if (rate <= 0 || most <= 0) {
        cuts.push_back(mono.size());
        return cuts;
    }
    const size_t limit = std::max<size_t>(1, size_t(most * rate)), look = std::min(limit / 2, size_t(3 * rate));
    const size_t win = std::max<size_t>(1, size_t(rate / 50)), hop = std::max<size_t>(1, win / 2);
    size_t at = 0;
    while (mono.size() - at > limit) {
        // The quietest 20 ms in the piece's last few seconds.
        size_t best = at + limit;
        double least = -1;
        for (size_t w = at + limit - look; w + win <= at + limit; w += hop) {
            double e = 0;
            for (size_t i = w; i < w + win; ++i) e += double(mono[i]) * mono[i];
            if (least < 0 || e < least) least = e, best = w + win / 2;
        }
        cuts.push_back(best);
        at = best;
    }
    cuts.push_back(mono.size());
    return cuts;
}

void trimSpeech(std::vector<float>& speech, int rate) {
    float peak = 0;
    for (const float v : speech) peak = std::max(peak, std::fabs(v));
    if (peak <= 0 || rate <= 0) return;
    const size_t win = size_t(std::max(1, rate / 100)), keep = size_t(rate / 50);  // 10 ms windows, 20 ms kept
    auto loud = [&](size_t at) {
        float m = 0;
        for (size_t i = at; i < std::min(speech.size(), at + win); ++i) m = std::max(m, std::fabs(speech[i]));
        return m > peak * 0.02f;  // -34 dB
    };
    size_t from = 0, to = speech.size();
    while (from < to && !loud(from)) from += win;
    while (to > from + win && !loud(to - win)) to -= win;
    from = from > keep ? from - keep : 0;
    to = std::min(speech.size(), to + keep);
    if (from >= to) return;
    speech = std::vector<float>(speech.begin() + std::ptrdiff_t(from), speech.begin() + std::ptrdiff_t(to));
}

std::vector<float> stretchSpeech(const std::vector<float>& speech, int rate, double factor) {
    if (speech.empty() || rate <= 0 || !(factor > 0) || std::abs(factor - 1) < 1e-3) return speech;
    AudioBuffer in;
    in.sampleRate = rate;
    in.channels = 1;
    in.samples = speech;
    const int hop = stretchHop(rate);
    const int64_t outFrames = int64_t(std::llround(double(speech.size()) * factor));
    std::vector<double> positions(size_t(outFrames / hop + 2));
    for (size_t j = 0; j < positions.size(); ++j) positions[j] = double(j) * hop / factor;
    AudioBuffer out;
    wsolaStretch(in, positions, hop, outFrames, out);
    return out.samples;
}

bool cloneSpeechToFit(const std::string& text, const std::vector<float>& reference, int referenceRate, const CloneOptions& options,
                      double speed, double room, bool exact, std::vector<float>& out, std::string* error, const std::atomic<bool>* cancel) {
    if (!cloneSpeech(text, reference, referenceRate, options, out, error, {}, cancel)) return false;
    trimSpeech(out, kCloneSampleRate);
    const double said = double(out.size()) / kCloneSampleRate;
    if (said <= 0) return true;
    double factor = 1 / std::clamp(speed, 0.5, 2.0);  // length, not speed
    if (exact && room > 0) {
        // Filling the room: within 1.6 times as fast and 1.4 times as slow. Longer than that would run over what follows.
        factor = std::clamp(room / said, 1 / 1.6, 1.4);
        if (said * factor > room * 1.0005) {  // (only rounding past it)
            if (error)
                *error = QStringLiteral("The words take %1 s to say even 1.6 times as fast, more than the %2 s they replace")
                             .arg(said * factor, 0, 'f', 1)
                             .arg(room, 0, 'f', 1)
                             .toStdString();
            out.clear();
            return false;
        }
    } else if (room > 0 && said * factor > room * 1.02)
        factor = std::min(factor, std::max(1 / 1.6, room / said));
    out = stretchSpeech(out, kCloneSampleRate, factor);
    return true;
}

bool writeClonedSpeechWav(const std::string& path, const std::vector<float>& samples, const std::string& note, std::string* error) {
    QByteArray b;
    auto u32 = [&](uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { b.append(reinterpret_cast<const char*>(&v), 2); };
    // LIST/INFO: ICMT (the note) and ISFT (what made it), each NUL-terminated and padded to an even size.
    auto info = [](const char* id, const std::string& text) {
        std::string t = text + '\0';
        if (t.size() % 2) t += '\0';
        std::string chunk(id, 4);
        const uint32_t n = uint32_t(t.size());
        chunk.append(reinterpret_cast<const char*>(&n), 4);
        return chunk + t;
    };
    const std::string list = "INFO" + info("ICMT", note) + info("ISFT", "Montage (Chatterbox Multilingual voice cloning)");
    const uint32_t bytes = uint32_t(samples.size() * 2);
    b.append("RIFF", 4), u32(uint32_t(4 + 24 + 8 + list.size() + 8 + bytes)), b.append("WAVEfmt ", 8), u32(16), u16(1), u16(1),
        u32(kCloneSampleRate), u32(kCloneSampleRate * 2), u16(2), u16(16);
    b.append("LIST", 4), u32(uint32_t(list.size())), b.append(list.data(), qsizetype(list.size()));
    b.append("data", 4), u32(bytes);
    for (const float s : samples) u16(uint16_t(int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767))));
    // Through QFile: a UTF-8 path opens on Windows too.
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(b) != b.size()) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Speech

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool voiceCloneAvailable() { return false; }

bool cloneSpeech(const std::string&, const std::vector<float>&, int, const CloneOptions&, std::vector<float>& out, std::string* error,
                 const std::function<void(double)>&, const std::atomic<bool>*) {
    out.clear();
    if (error) *error = "This build of Montage cannot clone voices: it was built without ONNX Runtime";
    return false;
}

bool convertVoice(const std::vector<float>&, int, const std::vector<float>&, int, std::vector<float>& out, std::string* error,
                  const std::function<void(double)>&, const std::atomic<bool>*) {
    out.clear();
    if (error) *error = "This build of Montage cannot convert voices: it was built without ONNX Runtime";
    return false;
}

#else

bool voiceCloneAvailable() { return true; }

namespace {

struct CloneModel {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-voice-clone"};
    std::unique_ptr<OrtModel> encoder, embed, lm, decoder;
    std::vector<std::string> lmInputs, lmOutputs;
    CloneTokenizer tokenizer;
};

std::shared_ptr<CloneModel> loadCloneModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<CloneModel>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = voiceCloneModel();
    if (!pack.installed()) {
        if (error) *error = "The voice cloning model is not downloaded";
        return nullptr;
    }
    auto model = std::make_shared<CloneModel>();
    const std::string dir = pack.directory();
    if (!model->tokenizer.load(dir + "/tokenizer.json", dir + "/Cangjie5_TC.json", error)) return nullptr;
    try {
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // The decoder's graph is large (about 24,000 nodes, its flow-matching steps unrolled): sequential execution's
        // memory planner takes over a minute to load it, parallel execution's a few seconds. Two branches at a time, the
        // other cores within each node, runs it about as fast as sequential execution does.
        Ort::SessionOptions parallel;
        parallel.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        parallel.SetExecutionMode(ExecutionMode::ORT_PARALLEL);
        parallel.SetInterOpNumThreads(2);
        const unsigned cores = std::thread::hardware_concurrency();
        parallel.SetIntraOpNumThreads(cores > 1 ? int(cores) - 1 : 1);
        auto open = [&](const char* name, const Ort::SessionOptions& options) {
#ifdef _WIN32
            const std::wstring path = QString::fromStdString(dir + "/" + name).toStdWString();
#else
            const std::string path = dir + "/" + name;
#endif
            return openModel(model->env, path.c_str(), options, ModelRuns::OnProcessor);  // (a token at a time)
        };
        model->encoder = open("speech_encoder.onnx", so);
        model->embed = open("embed_tokens.onnx", so);
        model->lm = open("language_model_q4.onnx", so);
        model->decoder = open("conditional_decoder.onnx", parallel);
        Ort::AllocatorWithDefaultOptions al;
        for (size_t i = 0; i < model->lm->GetInputCount(); ++i) model->lmInputs.emplace_back(model->lm->GetInputNameAllocated(i, al).get());
        for (size_t i = 0; i < model->lm->GetOutputCount(); ++i) model->lmOutputs.emplace_back(model->lm->GetOutputNameAllocated(i, al).get());
        if (model->lmInputs.size() < 3 || model->lmOutputs.size() != model->lmInputs.size() - 1) {
            if (error) *error = "The voice cloning model is not the one expected";
            return nullptr;
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The voice cloning model could not be loaded: ") + e.what();
        return nullptr;
    }
    *cached = model;
    return model;
}

}  // namespace

bool cloneSpeech(const std::string& text, const std::vector<float>& reference, int referenceRate, const CloneOptions& options,
                 std::vector<float>& out, std::string* error, const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    out.clear();
    const CloneLanguage* lang = findCloneLanguage(options.language);
    if (!lang) {
        if (error) *error = "Voice cloning does not speak \"" + options.language + "\"";
        return false;
    }
    const std::string said = normalizeCloneText(text);
    if (said.empty()) {
        if (error) *error = "Nothing to say";
        return false;
    }
    if (std::string(lang->code) == "ja") {
        // The model reads Japanese in kana; its vocabulary has no kanji (Chatterbox converts them with pykakasi).
        for (const uint cp : QString::fromStdString(said).toUcs4())
            if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF)) {
                if (error) *error = "Japanese is spoken from kana: write the text in hiragana or katakana (the voice model cannot read kanji)";
                return false;
            }
    }
    std::vector<float> ref = cloneReference(reference, referenceRate);
    if (ref.size() < size_t(kCloneSampleRate)) {
        if (error) *error = "The voice sample is too short: it needs at least a second of speech (5 to 10 is best)";
        return false;
    }
    auto model = loadCloneModel(error);
    if (!model) return false;
    auto cancelled = [&] {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return true;
        }
        return false;
    };
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::AllocatorWithDefaultOptions alloc;
        // The speaker, once: conditioning for the language model, the reference's speech tokens, and the decoder's
        // speaker embedding and features.
        const int64_t refShape[2] = {1, int64_t(ref.size())};
        Ort::Value refIn = Ort::Value::CreateTensor<float>(cpu, ref.data(), ref.size(), refShape, 2);
        const char* encIn[] = {"audio_values"};
        const char* encOut[] = {"audio_features", "audio_tokens", "speaker_embeddings", "speaker_features"};
        auto speaker = model->encoder->Run(Ort::RunOptions{nullptr}, encIn, &refIn, 1, encOut, 4);
        const std::vector<int64_t> condShape = speaker[0].GetTensorTypeAndShapeInfo().GetShape();
        const float* cond = speaker[0].GetTensorData<float>();
        const size_t condLen = size_t(condShape.at(1)), dim = size_t(condShape.at(2));
        const int64_t* promptData = speaker[1].GetTensorData<int64_t>();
        const std::vector<int64_t> prompt(promptData, promptData + speaker[1].GetTensorTypeAndShapeInfo().GetElementCount());
        const std::vector<int64_t> embShape = speaker[2].GetTensorTypeAndShapeInfo().GetShape();
        const std::vector<int64_t> featShape = speaker[3].GetTensorTypeAndShapeInfo().GetShape();
        const float exaggeration = float(std::clamp(options.exaggeration, 0.0, 2.0));
        const int64_t exShape[1] = {1};
        auto embed = [&](std::vector<int64_t>& ids, std::vector<int64_t>& positions) {
            const int64_t shape[2] = {1, int64_t(ids.size())};
            float ex = exaggeration;
            Ort::Value in[3] = {Ort::Value::CreateTensor<int64_t>(cpu, ids.data(), ids.size(), shape, 2),
                                Ort::Value::CreateTensor<int64_t>(cpu, positions.data(), positions.size(), shape, 2),
                                Ort::Value::CreateTensor<float>(cpu, &ex, 1, exShape, 1)};
            const char* inNames[] = {"input_ids", "position_ids", "exaggeration"};
            const char* outNames[] = {"inputs_embeds"};
            auto r = model->embed->Run(Ort::RunOptions{nullptr}, inNames, in, 3, outNames, 1);
            const float* e = r[0].GetTensorData<float>();
            return std::vector<float>(e, e + r[0].GetTensorTypeAndShapeInfo().GetElementCount());
        };
        std::vector<const char*> lmIn, lmOut;
        for (const std::string& n : model->lmInputs) lmIn.push_back(n.c_str());
        for (const std::string& n : model->lmOutputs) lmOut.push_back(n.c_str());
        // Syllabic scripts say more per character: longer to speak, so shorter pieces and more room for each.
        const std::string code = lang->code;
        const int perChar = code == "zh" ? 10 : code == "ko" ? 8 : code == "ja" ? 6 : 4;
        const std::vector<std::string> pieces = splitCloneText(said, perChar > 4 ? 90 : 260);
        for (size_t n = 0; n < pieces.size(); ++n) {
            if (cancelled()) return false;
            std::vector<int64_t> ids = model->tokenizer.encode(pieces[n], lang->code);
            std::vector<int64_t> positions(ids.size());
            for (size_t k = 0; k < ids.size(); ++k) positions[k] = ids[k] >= kStartSpeech ? 0 : int64_t(k) - 1;
            // The first step reads the speaker and the text; each after it one new speech token, with the cache.
            std::vector<float> text = embed(ids, positions);
            std::vector<float> step(cond, cond + condLen * dim);
            step.insert(step.end(), text.begin(), text.end());
            std::vector<int64_t> mask(step.size() / dim, 1);
            std::vector<Ort::Value> past;
            const int64_t emptyShape[4] = {1, 16, 0, 64};
            for (size_t k = 2; k < model->lmInputs.size(); ++k) past.push_back(Ort::Value::CreateTensor<float>(alloc, emptyShape, 4));
            std::vector<int64_t> spoken{kStartSpeech};
            std::set<int64_t> seen{kStartSpeech};
            // About 25 tokens a second of speech: room for slow speech, and a stop if the model never ends.
            const int most = std::min(1000, 80 + perChar * int(QString::fromStdString(pieces[n]).size()));
            for (int i = 0; i < most; ++i) {
                if (cancelled()) return false;
                const int64_t embShape3[3] = {1, int64_t(step.size() / dim), int64_t(dim)};
                const int64_t maskShape[2] = {1, int64_t(mask.size())};
                std::vector<Ort::Value> in;
                in.push_back(Ort::Value::CreateTensor<float>(cpu, step.data(), step.size(), embShape3, 3));
                in.push_back(Ort::Value::CreateTensor<int64_t>(cpu, mask.data(), mask.size(), maskShape, 2));
                for (Ort::Value& v : past) in.push_back(std::move(v));
                auto r = model->lm->Run(Ort::RunOptions{nullptr}, lmIn.data(), in.data(), in.size(), lmOut.data(), lmOut.size());
                const std::vector<int64_t> logitShape = r[0].GetTensorTypeAndShapeInfo().GetShape();
                const size_t vocab = size_t(logitShape.back()), rows = size_t(logitShape.at(1));
                std::vector<float> logits(r[0].GetTensorData<float>() + (rows - 1) * vocab, r[0].GetTensorData<float>() + rows * vocab);
                // Greedy, with the model's repetition penalty (1.2) on what has been said.
                for (const int64_t t : seen)
                    if (t >= 0 && size_t(t) < vocab) logits[size_t(t)] = logits[size_t(t)] < 0 ? logits[size_t(t)] * 1.2f : logits[size_t(t)] / 1.2f;
                const int64_t next = int64_t(std::max_element(logits.begin(), logits.end()) - logits.begin());
                spoken.push_back(next);
                seen.insert(next);
                if (next == kStopSpeech) break;
                past.clear();
                for (size_t k = 1; k < r.size(); ++k) past.push_back(std::move(r[k]));
                std::vector<int64_t> one{next}, at{int64_t(i) + 1};
                step = embed(one, at);
                mask.push_back(1);
                if (progress) progress((double(n) + 0.9 * double(i + 1) / most) / double(pieces.size()));
            }
            std::vector<int64_t> tokens(prompt);
            for (size_t k = 1; k < spoken.size(); ++k)
                if (spoken[k] != kStopSpeech && spoken[k] < kStartSpeech) tokens.push_back(spoken[k]);
            if (tokens.size() == prompt.size()) continue;  // nothing said
            const int64_t tokShape[2] = {1, int64_t(tokens.size())};
            Ort::Value dec[3] = {Ort::Value::CreateTensor<int64_t>(cpu, tokens.data(), tokens.size(), tokShape, 2),
                                 Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(speaker[2].GetTensorData<float>()),
                                                                 speaker[2].GetTensorTypeAndShapeInfo().GetElementCount(), embShape.data(),
                                                                 embShape.size()),
                                 Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(speaker[3].GetTensorData<float>()),
                                                                 speaker[3].GetTensorTypeAndShapeInfo().GetElementCount(), featShape.data(),
                                                                 featShape.size())};
            const char* decIn[] = {"speech_tokens", "speaker_embeddings", "speaker_features"};
            const char* decOut[] = {"waveform"};
            auto wav = model->decoder->Run(Ort::RunOptions{nullptr}, decIn, dec, 3, decOut, 1);
            const float* w = wav[0].GetTensorData<float>();
            const size_t count = wav[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (!out.empty()) out.insert(out.end(), size_t(kCloneSampleRate / 10), 0.0f);  // between pieces
            out.insert(out.end(), w, w + count);
            if (progress) progress(double(n + 1) / double(pieces.size()));
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Voice cloning failed: ") + e.what();
        return false;
    }
    if (out.empty()) {
        if (error) *error = "Nothing was said";
        return false;
    }
    return true;
}

bool convertVoice(const std::vector<float>& speech, int speechRate, const std::vector<float>& reference, int referenceRate,
                  std::vector<float>& out, std::string* error, const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    out.clear();
    const std::vector<float> ref = cloneReference(reference, referenceRate);
    if (ref.size() < size_t(kCloneSampleRate)) {
        if (error) *error = "The voice sample is too short: it needs at least a second of speech (5 to 10 is best)";
        return false;
    }
    const std::vector<float> x = resampleForVoice(speech, speechRate);
    float peak = 0;
    for (const float v : x) peak = std::max(peak, std::fabs(v));
    if (x.empty() || peak <= 1e-4f) {
        if (error) *error = "There is no speech to convert";
        return false;
    }
    auto model = loadCloneModel(error);
    if (!model) return false;
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* encIn[] = {"audio_values"};
        const char* encOut[] = {"audio_features", "audio_tokens", "speaker_embeddings", "speaker_features"};
        auto encode = [&](std::vector<float>& audio) {
            const int64_t shape[2] = {1, int64_t(audio.size())};
            Ort::Value in = Ort::Value::CreateTensor<float>(cpu, audio.data(), audio.size(), shape, 2);
            return model->encoder->Run(Ort::RunOptions{nullptr}, encIn, &in, 1, encOut, 4);
        };
        // The new voice: its speech tokens, which lead the decoder in, and its speaker embedding and features.
        std::vector<float> refCopy = ref;
        auto speaker = encode(refCopy);
        const int64_t* promptData = speaker[1].GetTensorData<int64_t>();
        const std::vector<int64_t> prompt(promptData, promptData + speaker[1].GetTensorTypeAndShapeInfo().GetElementCount());
        const std::vector<int64_t> embShape = speaker[2].GetTensorTypeAndShapeInfo().GetShape();
        const std::vector<int64_t> featShape = speaker[3].GetTensorTypeAndShapeInfo().GetShape();
        // What is said, as speech tokens (the words, timing and delivery without the voice), a piece of at most about ten
        // seconds at a time (all the encoder hears), each piece said again in the new voice and kept to its own length.
        const std::vector<size_t> cuts = voiceConversionCuts(x, kCloneSampleRate);
        out.reserve(x.size());
        for (size_t n = 0; n + 1 < cuts.size(); ++n) {
            if (cancel && cancel->load()) {
                if (error) *error = "Cancelled";
                out.clear();
                return false;
            }
            const size_t a = cuts[n], b = cuts[n + 1], length = b - a;
            float loudest = 0;
            for (size_t i = a; i < b; ++i) loudest = std::max(loudest, std::fabs(x[i]));
            if (loudest <= peak * 0.01f) {  // (silence stays silence)
                out.insert(out.end(), length, 0.0f);
                if (progress) progress(double(b) / double(x.size()));
                continue;
            }
            std::vector<float> piece(x.begin() + std::ptrdiff_t(a), x.begin() + std::ptrdiff_t(b));
            piece.resize(std::max(piece.size(), size_t(kCloneSampleRate)), 0.0f);  // (at least a second for the encoder)
            auto said = encode(piece);
            std::vector<int64_t> tokens(prompt);
            const int64_t* t = said[1].GetTensorData<int64_t>();
            tokens.insert(tokens.end(), t, t + said[1].GetTensorTypeAndShapeInfo().GetElementCount());
            const int64_t tokShape[2] = {1, int64_t(tokens.size())};
            Ort::Value dec[3] = {Ort::Value::CreateTensor<int64_t>(cpu, tokens.data(), tokens.size(), tokShape, 2),
                                 Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(speaker[2].GetTensorData<float>()),
                                                                 speaker[2].GetTensorTypeAndShapeInfo().GetElementCount(), embShape.data(),
                                                                 embShape.size()),
                                 Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(speaker[3].GetTensorData<float>()),
                                                                 speaker[3].GetTensorTypeAndShapeInfo().GetElementCount(), featShape.data(),
                                                                 featShape.size())};
            const char* decIn[] = {"speech_tokens", "speaker_embeddings", "speaker_features"};
            const char* decOut[] = {"waveform"};
            auto wav = model->decoder->Run(Ort::RunOptions{nullptr}, decIn, dec, 3, decOut, 1);
            const float* w = wav[0].GetTensorData<float>();
            const size_t count = std::min(length, size_t(wav[0].GetTensorTypeAndShapeInfo().GetElementCount()));
            out.insert(out.end(), w, w + count);
            out.insert(out.end(), length - count, 0.0f);  // (tokens come 25 a second: up to 40 ms short)
            if (progress) progress(double(b) / double(x.size()));
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Voice conversion failed: ") + e.what();
        out.clear();
        return false;
    }
    return true;
}

#endif

}  // namespace montage
