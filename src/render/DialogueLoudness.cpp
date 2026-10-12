#include "DialogueLoudness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

#include "core/EditOps.h"
#include "core/Surround.h"
#include "core/TranscriptEdit.h"
#include "media/Loudness.h"
#include "render/AutoMix.h"
#include "render/Compositor.h"

namespace montage {

namespace {
constexpr double kWordPad = 0.1;   // seconds either side of a word
constexpr double kWordGap = 0.5;   // pauses shorter than this between words stay speech
constexpr double kStemRange = 20;  // LU under the dialogue's loudness its sound still counts as speech

using Spans = std::vector<std::pair<double, double>>;

Spans merged(Spans spans, double bridge) {
    std::sort(spans.begin(), spans.end());
    Spans out;
    for (const auto& [a, b] : spans) {
        if (b <= a) continue;
        if (!out.empty() && a <= out.back().second + bridge) out.back().second = std::max(out.back().second, b);
        else out.push_back({a, b});
    }
    return out;
}
}  // namespace

const char* speechGateName(SpeechGate g) {
    switch (g) {
        case SpeechGate::Roles: return "roles";
        case SpeechGate::Transcripts: return "transcripts";
        case SpeechGate::Classifier: return "classifier";
        case SpeechGate::Auto: break;
    }
    return "auto";
}

bool parseSpeechGate(const std::string& name, SpeechGate& out) {
    for (SpeechGate g : {SpeechGate::Auto, SpeechGate::Roles, SpeechGate::Transcripts, SpeechGate::Classifier})
        if (name == speechGateName(g)) {
            out = g;
            return true;
        }
    return false;
}

Spans timelineSpeech(const Project& p, const Sequence& s, SpeechGate& gate, const std::vector<bool>& tracks, double from, double to) {
    auto within = [&](double a, double b) { return b > from && (to < 0 || a < to); };
    if (gate == SpeechGate::Classifier) return {};
    const double fps = s.fpsValue() > 0 ? s.fpsValue() : 30.0;
    const bool anySolo = std::any_of(s.audioTracks.begin(), s.audioTracks.end(), [](const Track& t) { return t.solo; });
    auto heard = [&](size_t i) {
        const Track& t = s.audioTracks[i];
        return !t.muted && (!anySolo || t.solo) && (tracks.empty() || (i < tracks.size() && tracks[i]));
    };
    if (gate != SpeechGate::Transcripts) {
        Spans clips;
        if (!edit::roleMuted(s, "Dialogue"))
            for (size_t i = 0; i < s.audioTracks.size(); ++i)
                if (heard(i))
                    for (const Clip& c : s.audioTracks[i].clips)
                        if (c.enabled && c.role == "Dialogue" && within(double(c.start) / fps, double(c.end()) / fps))
                            clips.push_back({double(c.start) / fps, double(c.end()) / fps});
        if (!clips.empty() || gate == SpeechGate::Roles) {
            gate = SpeechGate::Roles;
            return merged(std::move(clips), 0);
        }
    }
    gate = SpeechGate::Transcripts;
    // Only the words heard: tracks left out are muted, clips of a muted role off.
    Sequence heardSeq = s;
    for (size_t i = 0; i < heardSeq.audioTracks.size(); ++i) {
        Track& t = heardSeq.audioTracks[i];
        if (!tracks.empty() && (i >= tracks.size() || !tracks[i])) t.muted = true;
        for (Clip& c : t.clips)
            if (edit::roleMuted(s, c.role)) c.enabled = false;
    }
    // Picture clips' words stand in for the sound's only when the sequence has no sound clips at all (not when the
    // dialogue's are muted or left out).
    if (std::any_of(s.audioTracks.begin(), s.audioTracks.end(), [](const Track& t) { return !t.clips.empty(); })) heardSeq.videoTracks.clear();
    Spans words;
    for (const TranscriptWord& w : sequenceTranscriptWords(p, heardSeq))
        if (within(w.start, w.end)) words.push_back({std::max(0.0, w.start - kWordPad), w.end + kWordPad});
    return merged(std::move(words), kWordGap);
}

DialogueLoudness measureDialogueLoudness(const Project& p, const Sequence& s, const DialogueLoudnessOptions& o,
                                         const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                                         std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return DialogueLoudness{};
    };
    const double fps = s.fpsValue() > 0 ? s.fpsValue() : 30.0;
    const int rate = s.sampleRate > 0 ? s.sampleRate : 48000;
    const FrameTime out = o.out < 0 ? s.duration() : o.out;
    if (out <= o.in) return fail("Nothing to measure");
    const int64_t first = int64_t(std::llround(double(o.in) * rate / fps)), end = int64_t(std::llround(double(out) * rate / fps));

    SpeechGate gate = o.gate;
    const Spans marks = timelineSpeech(p, s, gate, o.tracks, double(o.in) / fps, double(out) / fps);
    if (o.gate == SpeechGate::Auto && gate == SpeechGate::Transcripts && marks.empty()) gate = SpeechGate::Classifier;

    const int nch = o.layout ? std::max(1, layoutChannels(s.audioLayout)) : 2;
    std::vector<double> weights;
    if (nch > 2)
        for (const Speaker& sp : layoutSpeakers(s.audioLayout)) weights.push_back(sp.loudnessWeight);
    weights.resize(size_t(nch), 1.0);
    AudioMixer mixer;
    mixer.setTrackMask(o.tracks);
    LoudnessMeter meter(rate);
    // Roles: the Dialogue clips on their own, to hear where they have something to say.
    Sequence stem;
    AudioMixer stemMixer;
    LoudnessMeter stemMeter(rate);
    if (gate == SpeechGate::Roles) {
        stem = s;
        for (Track& t : stem.audioTracks)
            for (Clip& c : t.clips)
                if (c.role != "Dialogue") c.enabled = false;
        stemMixer.setTrackMask(o.tracks);
    }
    // Listening: a second at a time (ten of the meter's steps), each judged with the seconds either side.
    const int64_t step = std::max<int64_t>(1, int64_t(rate * 0.1));
    const int64_t chunk = step * 10;
    std::deque<std::vector<float>> recent;
    std::vector<char> secondSpeech;
    auto judge = [&] {
        std::vector<float> mono;
        for (const auto& sec : recent) mono.insert(mono.end(), sec.begin(), sec.end());
        return classifyAudio(mono, rate).role == AudioRole::Dialogue;
    };

    std::vector<float> buf(size_t(chunk) * size_t(nch)), stemBuf(size_t(chunk) * 2);
    for (int64_t pos = first; pos < end;) {
        if (cancel && cancel->load()) return fail("Cancelled");
        const int n = int(std::min<int64_t>(chunk, end - pos));
        if (nch > 2) mixer.mixLayout(p, s, pos, n, buf.data());
        else mixer.mix(p, s, pos, n, buf.data());
        meter.addChannels(buf.data(), n, nch, weights.data());
        if (gate == SpeechGate::Roles) {
            stemMixer.mix(p, stem, pos, n, stemBuf.data());
            stemMeter.add(stemBuf.data(), n);
        } else if (gate == SpeechGate::Classifier) {
            std::vector<float> mono(size_t(n), 0.0f);
            double wsum = 0;
            for (int ch = 0; ch < nch; ++ch) wsum += weights[size_t(ch)] > 0 ? 1 : 0;
            for (int i = 0; i < n; ++i) {
                float v = 0;
                for (int ch = 0; ch < nch; ++ch)
                    if (weights[size_t(ch)] > 0) v += buf[size_t(i) * size_t(nch) + size_t(ch)];
                mono[size_t(i)] = float(v / std::max(1.0, wsum));
            }
            recent.push_back(std::move(mono));
            if (recent.size() > 3) recent.pop_front();
            secondSpeech.push_back(0);
            if (recent.size() == 3) {
                const bool speech = judge();
                secondSpeech[secondSpeech.size() - 2] = speech;
                if (secondSpeech.size() == 3) secondSpeech[0] = speech;  // (the first second has no second before it)
                secondSpeech.back() = speech;  // until the next window says otherwise
            }
        }
        pos += n;
        if (progress) progress(double(pos - first) / double(end - first));
    }
    if (gate == SpeechGate::Classifier && !secondSpeech.empty() && recent.size() < 3) {
        const bool speech = judge();  // shorter than three seconds: judged whole
        std::fill(secondSpeech.begin(), secondSpeech.end(), speech);
    }

    // Each 100 ms step is speech or not.
    const size_t steps = meter.steps();
    const LoudnessResult stemWhole = stemMeter.result();
    size_t mark = 0;
    std::vector<char> marked(steps, 0);
    for (size_t i = 0; i < steps; ++i) {
        bool speech = false;
        if (gate == SpeechGate::Roles) {
            // The 400 ms around the step (steps i-1 to i+2) within kStemRange of the dialogue's loudness.
            const size_t last = std::min(i + 2, stemMeter.steps() > 0 ? stemMeter.steps() - 1 : 0);
            const double l = stemMeter.momentaryAt(last);
            speech = stemWhole.valid && l > -70 && l >= stemWhole.integrated - kStemRange;
        } else if (gate == SpeechGate::Transcripts) {
            const double t = (double(first) + (double(i) + 0.5) * double(step)) / rate;
            while (mark < marks.size() && marks[mark].second <= t) ++mark;
            speech = mark < marks.size() && marks[mark].first <= t;
        } else if (gate == SpeechGate::Classifier) {
            speech = i / 10 < secondSpeech.size() && secondSpeech[i / 10];
        }
        meter.markSpeech(i, speech);
        marked[i] = speech;
    }

    DialogueLoudness r;
    r.gate = gate;
    const LoudnessResult whole = meter.result(), dialogue = meter.speechResult();
    r.valid = whole.valid;
    r.programme = whole.valid ? whole.integrated : -70;
    r.truePeak = whole.truePeakDb;
    r.dialogue = dialogue.valid ? dialogue.integrated : -70;
    r.speechShare = meter.speechShare();
    r.minSpeechShare = o.minSpeechShare;
    r.usesProgramme = !dialogue.valid || r.speechShare < o.minSpeechShare;
    // The speech as timeline spans.
    for (size_t i = 0; i < steps; ++i)
        if (marked[i]) {
            const double a = (double(first) + double(i) * double(step)) / rate;
            const double b = std::min(double(end), double(first) + double(i + 1) * double(step)) / rate;
            if (!r.speech.empty() && r.speech.back().second >= a - 1e-9) r.speech.back().second = b;
            else r.speech.push_back({a, b});
        }
    return r;
}

std::string describeDialogueLoudness(const DialogueLoudness& d) {
    if (!d.valid) return "Too quiet to measure";
    static const char* const by[] = {"", "Dialogue clips", "transcripts", "listening"};
    char buf[200];
    if (d.usesProgramme)
        std::snprintf(buf, sizeof buf, "Programme %.1f LKFS (%.0f %% speech, by %s: %s)", d.programme, d.speechShare * 100, by[int(d.gate)],
                      d.speechShare < d.minSpeechShare ? "under the share dialogue gating needs"
                                                       : "no stretch of speech long enough to measure");
    else
        std::snprintf(buf, sizeof buf, "Dialogue %.1f LKFS (estimate: %.0f %% speech, by %s)", d.dialogue, d.speechShare * 100, by[int(d.gate)]);
    return buf;
}

}  // namespace montage
