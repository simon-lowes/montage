#include "SdhDraft.h"

#include <QImage>
#include <limits>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "core/CaptionTools.h"
#include "core/TranscriptEdit.h"
#include "media/Faces.h"
#include "media/SoundSearch.h"
#include "media/TextReader.h"
#include "render/Compositor.h"
#include "render/DialogueLoudness.h"

namespace montage {

namespace {

bool cancelled(const std::atomic<bool>* c) { return c && c->load(); }

// The sequence's sound over [from, to) seconds, mono at CLAP's rate.
std::vector<float> monoClap(const Project& p, const Sequence& s, AudioMixer& mixer, double from, double to) {
    const int sr = s.sampleRate > 0 ? s.sampleRate : 48000;
    const int64_t a = int64_t(std::llround(std::max(0.0, from) * sr)), n = std::max<int64_t>(0, int64_t(std::llround(to * sr)) - a);
    std::vector<float> stereo(static_cast<size_t>(n) * 2), mono(static_cast<size_t>(n));
    mixer.reset();
    for (int64_t pos = 0; pos < n; pos += sr) {
        const int count = int(std::min<int64_t>(sr, n - pos));
        mixer.mix(p, s, a + pos, count, stereo.data() + pos * 2);
    }
    for (int64_t i = 0; i < n; ++i) mono[size_t(i)] = 0.5f * (stereo[size_t(i) * 2] + stereo[size_t(i) * 2 + 1]);
    if (sr == kClapRate || mono.empty()) return mono;
    // Another rate: linearly, which is plenty to tell sounds apart.
    const size_t m = size_t(std::llround(double(mono.size()) * kClapRate / sr));
    std::vector<float> out(m);
    for (size_t i = 0; i < m; ++i) {
        const double x = double(i) * sr / kClapRate;
        const size_t k = std::min(mono.size() - 1, size_t(x));
        const double t = x - double(k);
        out[i] = float(mono[k] * (1 - t) + mono[std::min(mono.size() - 1, k + 1)] * t);
    }
    return out;
}

float dot(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) d += a[i] * b[i];
    return d;
}

// Text embeddings, remembered for the process (the descriptions rarely change).
std::vector<float> textEmbedding(const SoundModel& model, const std::string& prompt) {
    static std::mutex lock;
    static std::map<std::string, std::vector<float>> cache;
    {
        std::lock_guard<std::mutex> g(lock);
        if (auto it = cache.find(prompt); it != cache.end()) return it->second;
    }
    std::vector<float> e = model.text(prompt);
    if (e.empty()) return e;  // (tried again next time)
    std::lock_guard<std::mutex> g(lock);
    cache[prompt] = e;
    return e;
}

std::vector<float> audioEmbedding(const SoundModel& model, const std::vector<float>& mono) {
    if (mono.empty()) return {};
    std::vector<float> clip(mono.begin(), mono.begin() + std::ptrdiff_t(std::min<size_t>(mono.size(), kClapWindow)));
    return model.audio(clapFeatures(clip));
}

}  // namespace

std::vector<std::pair<double, double>> loudStretches(const Project& p, const Sequence& s, const std::vector<std::pair<double, double>>& speech,
                                                     double loudDb, double minSeconds, const std::atomic<bool>* cancel) {
    const int sr = s.sampleRate > 0 ? s.sampleRate : 48000;
    const double fps = s.fpsValue() > 0 ? s.fpsValue() : 30.0;
    const int64_t total = int64_t(std::llround(double(s.duration()) / fps * sr));
    const int win = std::max(1, sr / 20);  // 50 ms
    const double floor = std::pow(10.0, loudDb / 20);
    AudioMixer mixer;
    std::vector<char> loud;
    std::vector<float> buf;
    double energy = 0;
    int fill = 0;
    for (int64_t pos = 0; pos < total; pos += sr) {
        if (cancelled(cancel)) return {};
        const int count = int(std::min<int64_t>(sr, total - pos));
        buf.assign(size_t(count) * 2, 0.0f);
        mixer.mix(p, s, pos, count, buf.data());
        for (int k = 0; k < count; ++k) {
            const double m = 0.5 * (double(buf[size_t(k) * 2]) + buf[size_t(k) * 2 + 1]);
            energy += m * m;
            if (++fill == win) loud.push_back(std::sqrt(energy / win) >= floor), energy = 0, fill = 0;
        }
    }
    if (fill > 0) loud.push_back(std::sqrt(energy / fill) >= floor);
    // Speech is not a sound to caption.
    const double step = double(win) / sr;
    for (const auto& [a, b] : speech)
        for (size_t w = size_t(std::max(0.0, std::floor(a / step))); w < loud.size() && double(w) * step < b; ++w) loud[w] = 0;
    std::vector<std::pair<double, double>> out;
    const size_t bridge = size_t(std::lround(0.3 / step));
    for (size_t i = 0; i < loud.size();) {
        if (!loud[i]) {
            ++i;
            continue;
        }
        size_t j = i + 1, last = i;
        while (j < loud.size() && j - last <= bridge) {
            if (loud[j]) last = j;
            ++j;
        }
        // A bridge never crosses speech.
        const double a = double(i) * step, b = double(last + 1) * step;
        if (b - a >= minSeconds) out.push_back({a, b});
        i = last + 1;
    }
    // Split where speech falls inside a bridged gap.
    std::vector<std::pair<double, double>> split;
    for (auto [a, b] : out) {
        for (const auto& [sa, sb] : speech)
            if (sa > a && sa < b) {
                if (sa - a >= minSeconds) split.push_back({a, sa});
                a = std::min(b, sb);
            }
        if (b - a >= minSeconds) split.push_back({a, b});
    }
    return split;
}

bool draftSdhCaptions(const Project& p, const Sequence& s, const SdhDraftOptions& o, SdhDraft& out,
                      const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    auto report = [&](double f) {
        if (progress) progress(std::clamp(f, 0.0, 1.0));
    };
    out = SdhDraft{};
    out.language = sequenceTranscriptLanguage(p, s);
    const double fps = s.fpsValue() > 0 ? s.fpsValue() : 30.0;
    SdhOptions so = o.sdh;
    so.lineChars = o.rules.lineChars;
    so.maxLines = o.rules.maxLines;
    if (const CaptionTrack* t = captionTrackFor(s)) so.look = t->style;
    out.look = so.look;
    so.aspect = s.height > 0 ? double(s.width) / s.height : 16.0 / 9.0;

    std::vector<SdhLine> lines = sdhDialogue(p, s, o.rules);
    std::shared_ptr<SoundModel> clap;
    if ((o.sounds || so.lyrics) && soundSearchAvailable() && soundModel().installed()) {
        std::string why;
        clap = SoundModel::load(&why);
        if (!clap) out.notes.push_back("The sound model could not be loaded (" + why + "): no sound cues or songs");
    } else if (o.sounds || so.lyrics) {
        out.notes.push_back("The sound model (What's Heard) is not installed: no sound cues, and songs are not marked");
    }
    report(0.02);

    // What is heard outside speech: the transcripts' words are speech, and so is every caption.
    if (o.sounds && clap) {
        SpeechGate gate = SpeechGate::Transcripts;
        std::vector<std::pair<double, double>> speech = timelineSpeech(p, s, gate);
        for (const SdhLine& l : lines) speech.push_back({double(l.caption.start) / fps, double(l.caption.end) / fps});
        std::sort(speech.begin(), speech.end());
        const auto stretches = loudStretches(p, s, speech, o.loudDb, o.minSoundSeconds, cancel);
        if (cancelled(cancel)) return fail("Cancelled");
        report(0.15);
        // The descriptions.
        std::vector<SoundCaption> vocabulary;
        std::vector<std::vector<float>> texts;
        for (const std::string& e : soundCaptionsFor(p)) {
            if (cancelled(cancel)) return fail("Cancelled");
            SoundCaption c = parseSoundCaption(e);
            if (c.caption.empty()) continue;
            std::vector<float> t = textEmbedding(*clap, c.prompt);
            if (t.empty()) continue;
            vocabulary.push_back(std::move(c));
            texts.push_back(std::move(t));
        }
        report(0.25);
        // Each stretch in windows of up to 3 s, 1.5 s apart (a short one whole).
        double heardSoFar = 0, toHear = 0;
        for (const auto& [a, b] : stretches) toHear += b - a;
        AudioMixer mixer;
        for (const auto& [a, b] : stretches) {
            for (double w = a; w < b - 1e-6; w += 1.5) {
                if (cancelled(cancel)) return fail("Cancelled");
                const double e = std::min(b, w + 3.0);
                if (w > a && e - w < 1.0) break;  // the last bit is in the window before
                // A short sound is heard with the quiet around it (up to 3 s, never into speech), so the model, which
                // repeats what it hears to fill its 10 s, hears it a few times with quiet between rather than dozens
                // of times back to back.
                double from = w, to = e;
                if (to - from < 3.0) {
                    double before = 0, after = std::numeric_limits<double>::max();
                    for (const auto& [sa, sb] : speech) {
                        if (sb <= w) before = std::max(before, sb);
                        if (sa >= e) after = std::min(after, sa);
                    }
                    from = std::max(before, w - 0.25);
                    to = std::min(after, std::max(e, from + 3.0));
                }
                const std::vector<float> emb = audioEmbedding(*clap, monoClap(p, s, mixer, from, to));
                if (emb.empty()) continue;
                int best = -1;
                float bestScore = -2;
                for (size_t k = 0; k < texts.size(); ++k)
                    if (const float d = dot(emb, texts[k]); d > bestScore) bestScore = d, best = int(k);
                if (best >= 0 && bestScore >= float(o.threshold)) out.heard.push_back({w, e, vocabulary[size_t(best)].caption, bestScore});
                if (e >= b) break;
            }
            heardSoFar += b - a;
            report(0.25 + 0.45 * heardSoFar / std::max(1e-9, toHear));
        }
    }
    report(0.7);

    // Songs: the dialogue a window at a time (up to 10 s, CLAP's), sung where it sounds more like singing than speech.
    if (so.lyrics && clap && !lines.empty()) {
        const std::vector<float> singing = textEmbedding(*clap, "a person singing a song"),
                                 speaking = textEmbedding(*clap, "a person speaking");
        AudioMixer mixer;
        for (size_t i = 0; i < lines.size();) {
            if (cancelled(cancel)) return fail("Cancelled");
            size_t j = i + 1;
            while (j < lines.size() && double(lines[j].caption.end - lines[i].caption.start) / fps <= 10.0) ++j;
            const double from = double(lines[i].caption.start) / fps, to = std::min(from + 10.0, double(lines[j - 1].caption.end) / fps);
            const std::vector<float> emb = audioEmbedding(*clap, monoClap(p, s, mixer, from, to));
            const bool sung = !emb.empty() && dot(emb, singing) > dot(emb, speaking);
            for (size_t k = i; k < j; ++k) lines[k].sung = sung;
            i = j;
            report(0.7 + 0.1 * double(i) / double(lines.size()));
        }
    }

    // Faces where the speaker changes.
    if (so.speakerLabels && o.faces) {
        if (faceSearchAvailable() && faceModel().installed()) {
            RenderOptions ro;
            ro.scale = std::min(1.0, 640.0 / std::max(1, s.width));
            const std::vector<size_t> changes = sdhSpeakerChanges(lines);
            for (size_t n = 0; n < changes.size(); ++n) {
                if (cancelled(cancel)) return fail("Cancelled");
                SdhLine& l = lines[changes[n]];
                // A few frames in, past a cut on the first word.
                const FrameTime at = std::min(l.caption.end - 1, l.caption.start + FrameTime(std::lround(fps / 4)));
                const Image img = renderProgramFrame(p, s, at, ro);
                const auto faces = cachedFaces(img);
                if (!faces) break;
                // Faces big enough to be someone in the scene (8 % of the frame's height or more).
                l.faces = int(std::count_if(faces->begin(), faces->end(), [](const FaceBox& f) { return f.h >= 0.08f; }));
                report(0.8 + 0.05 * double(n + 1) / double(changes.size()));
            }
        } else {
            out.notes.push_back("The face model is not installed: every change of speaker is labelled");
        }
    }

    out.captions = sdhCaptions(lines, out.heard, {}, s.fps, so, &out.summary);
    if (out.captions.empty()) return fail("Nothing to caption: no transcribed speech is heard in this sequence, and no sounds were found");
    report(0.85);

    // Text in the picture where each caption would sit.
    if (o.readText) {
        if (ocrAvailable() && ocrModel().installed()) {
            std::string why;
            const std::shared_ptr<TextReader> reader = TextReader::load("en", &why);
            if (reader) {
                RenderOptions ro;
                ro.scale = std::min(1.0, 1280.0 / std::max(1, s.width));
                ro.displaySpace = "rec709";
                std::vector<SdhText> text;
                for (size_t i = 0; i < out.captions.size(); ++i) {
                    if (cancelled(cancel)) return fail("Cancelled");
                    const Caption& c = out.captions[i];
                    double x0, y0, x1, y1;
                    if (c.vertical != kCaptionBottom || !sdhCaptionArea(c, so, x0, y0, x1, y1)) continue;
                    const QImage picture = renderProgramFrameRgba8(p, s, c.start + (c.end - c.start) / 2, ro);
                    if (picture.isNull()) continue;
                    // The band the caption would cover, a little taller.
                    TextRegion region{0, std::max(0.0, y0 - 0.03), 1, std::min(1.0, y1 + 0.03)};
                    for (const TextLine& t : reader->read(picture, region))
                        if (t.confidence >= 0.6f && !t.text.empty())
                            text.push_back({double(c.start) / fps, double(c.end) / fps, t.x0, t.y0, t.x1, t.y1});
                    report(0.85 + 0.15 * double(i + 1) / double(out.captions.size()));
                }
                out.summary.raised += sdhRaiseOverText(out.captions, text, s.fps, so);
            } else {
                out.notes.push_back("The text-reading model could not be loaded (" + why + "): captions are not moved off text in the picture");
            }
        } else {
            out.notes.push_back("The text-reading model is not installed: captions are not moved off text in the picture");
        }
    }
    // And off the sequence's own low titles.
    out.summary.raised += raiseCaptionsOverTitles(out.captions, s);
    report(1);
    return true;
}

}  // namespace montage
