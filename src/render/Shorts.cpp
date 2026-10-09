#include "Shorts.h"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <set>

#include "core/Captions.h"
#include "core/EditOps.h"
#include "core/Transcript.h"
#include "render/ClipAnalysis.h"
#include "render/Highlights.h"

namespace montage {

namespace {

bool cancelled(const std::atomic<bool>* c) { return c && c->load(); }

// Lower case, letters, digits and apostrophes (curly ones made straight): "Here's" -> "here's".
std::string key(const std::string& w) {
    QString out;
    for (QChar c : QString::fromStdString(w)) {
        if (c == QChar(0x2019)) c = QLatin1Char('\'');
        if (c.isLetterOrNumber() || c == QLatin1Char('\'')) out += c.toLower();
    }
    while (out.startsWith(QLatin1Char('\''))) out.remove(0, 1);
    while (out.endsWith(QLatin1Char('\''))) out.chop(1);
    return out.toStdString();
}

std::vector<std::string> keys(const std::string& text) {
    std::vector<std::string> out;
    for (const QString& part : QString::fromStdString(text).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (std::string k = key(part.toStdString()); !k.empty()) out.push_back(std::move(k));
    return out;
}

// Whether the word ends a sentence: . ? ! … (and the CJK forms), past closing quotes and brackets; not "Mr." or "e.g.".
bool endsSentence(const std::string& word) {
    QString q = QString::fromStdString(word).trimmed();
    static const QString closers = QStringLiteral("\"')]”’»");
    while (!q.isEmpty() && closers.contains(q.back())) q.chop(1);
    if (q.isEmpty()) return false;
    static const QString enders = QStringLiteral(".?!…。？！");
    if (!enders.contains(q.back())) return false;
    static const std::set<std::string> abbreviations = {"mr", "mrs", "ms", "dr", "prof", "st", "vs", "eg", "ie", "jr", "sr"};
    return !(q.back() == QLatin1Char('.') && abbreviations.count(key(q.toStdString())));
}

bool endsQuestion(const std::string& word) {
    QString q = QString::fromStdString(word).trimmed();
    while (!q.isEmpty() && QStringLiteral("\"')]”’»").contains(q.back())) q.chop(1);
    return q.endsWith(QLatin1Char('?')) || q.endsWith(QChar(0xff1f));
}

bool isNumberWord(const std::string& k) {
    static const std::set<std::string> numbers = {"one",   "two",     "three",   "four",     "five",    "six",     "seven",
                                                  "eight", "nine",    "ten",     "eleven",   "twelve",  "twenty",  "thirty",
                                                  "fifty", "hundred", "thousand", "million", "billion", "half",    "double",
                                                  "first", "percent"};
    if (numbers.count(k)) return true;
    return std::any_of(k.begin(), k.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Words too common to say what something is about.
bool isStopWord(const std::string& k) {
    static const std::set<std::string> stop = {
        "the",  "a",    "an",   "and",   "or",    "but",  "of",   "to",   "in",    "on",    "at",    "for",  "with", "about",
        "is",   "are",  "was",  "were",  "be",    "been", "it",   "its",  "this",  "that",  "these", "those", "i",   "you",
        "we",   "they", "he",   "she",   "my",    "your", "our",  "their", "what", "how",   "why",   "when", "who",  "do",
        "does", "did",  "have", "has",   "had",   "can",  "will", "would", "should", "could", "not",  "so",   "as",   "by",
        "from", "into", "than", "then",  "there", "here", "just", "really", "very", "all",   "any",   "some", "more", "most"};
    return k.size() < 3 || stop.count(k);
}

// "stories" and "story", "editing" and "edit" alike, roughly.
std::string stem(std::string k) {
    for (const char* suffix : {"ing", "ies", "es", "ed", "s"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (k.size() > n + 3 && k.compare(k.size() - n, n, suffix) == 0) {
            k.resize(k.size() - n);
            if (std::string(suffix) == "ies") k += 'y';
            break;
        }
    }
    return k;
}

struct Word {
    double start, end;
    std::string text;
    int speaker;
};

}  // namespace

double hookScore(const std::string& sentence) {
    const std::vector<std::string> ks = keys(sentence);
    if (ks.empty()) return 0;
    std::string joined = " ";
    for (const std::string& k : ks) joined += k + " ";
    auto has = [&](const char* phrase) { return joined.find(std::string(" ") + phrase + " ") != std::string::npos; };
    double s = 0;
    QString trimmed = QString::fromStdString(sentence).trimmed();
    if (endsQuestion(trimmed.toStdString())) s += 0.35;
    static const char* const openers[] = {"did you know", "here's", "here is", "the secret", "the truth", "the problem", "the reason",
                                          "what if",      "imagine", "nobody", "no one",    "stop",      "the one thing", "most people",
                                          "you need",     "you should", "you won't", "let me tell you", "i was wrong", "the mistake",
                                          "the biggest",  "the best", "the worst", "never"};
    for (const char* o : openers)
        if (has(o)) {
            s += 0.3;
            break;
        }
    if (has("you") || has("your") || has("you're") || has("yourself")) s += 0.2;
    if (std::any_of(ks.begin(), ks.end(), isNumberWord)) s += 0.15;
    static const std::set<std::string> strong = {"best",   "worst",    "most",     "least",     "biggest",  "only",   "every",
                                                 "always", "never",    "secret",   "mistake",   "mistakes", "wrong",  "crazy",
                                                 "insane", "incredible", "amazing", "surprising", "shocking", "free",  "fastest",
                                                 "easiest", "cheapest", "simple",   "hardest",   "truth",    "actually"};
    if (std::any_of(ks.begin(), ks.end(), [](const std::string& k) { return strong.count(k) > 0; })) s += 0.2;
    if (ks.size() >= 4 && ks.size() <= 18) s += 0.1;
    if (ks.size() > 30) s -= 0.15;
    // Starting mid-thought, or with a hesitation.
    static const std::set<std::string> connectives = {"and", "so", "but", "because", "or", "also", "then", "which", "plus", "anyway"};
    static const std::set<std::string> backReferences = {"it", "that", "they", "he", "she", "those", "there", "them"};
    if (connectives.count(ks[0])) s -= 0.3;
    else if (backReferences.count(ks[0])) s -= 0.1;
    if (isFillerWord(ks[0])) s -= 0.2;
    return std::clamp(s, 0.0, 1.0);
}

std::vector<ShortMoment> findShorts(const Project& p, const std::vector<Id>& media, const ShortsOptions& o,
                                    const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    std::vector<ShortMoment> all;
    std::set<std::string> topic;
    for (const std::string& k : keys(o.topic))
        if (!isStopWord(k)) topic.insert(stem(k));
    bool anyTranscript = false;
    for (size_t mi = 0; mi < media.size(); ++mi) {
        if (cancelled(cancel)) return {};
        const MediaItem* m = p.findMedia(media[mi]);
        if (!m || m->subclipOf || !m->transcript || m->transcript->empty()) continue;
        anyTranscript = true;
        std::vector<Word> words;
        std::vector<TranscriptWord> plain;
        for (const TranscriptSegment& seg : m->transcript->segments)
            for (const TranscriptWord& w : seg.words) {
                if (key(w.text).empty() && !endsSentence(w.text)) continue;
                words.push_back({w.start, w.end, w.text, seg.speaker});
                plain.push_back(w);
            }
        const size_t n = words.size();
        if (n < 2) continue;
        FillerOptions fo = o.fillers;
        if (fo.language.empty()) fo.language = m->transcript->language;
        const std::vector<bool> filler = fillerWordMask(plain, fo);
        // Liveliness, against the footage's own range (its 95th percentile is 1).
        std::vector<double> curve;
        if (o.liveliness && m->kind == MediaKind::Video) {
            curve = highlightCurve(p, *m, {}, cancel);
            if (!curve.empty()) {
                std::vector<double> sorted = curve;
                std::sort(sorted.begin(), sorted.end());
                const double top = std::max(1e-6, sorted[std::min(sorted.size() - 1, size_t(double(sorted.size()) * 0.95))]);
                for (double& v : curve) v = std::min(1.0, v / top);
            }
        }
        if (cancelled(cancel)) return {};
        // Sentences: to a word ending one, or a long silence.
        std::vector<std::pair<size_t, size_t>> sentences;
        for (size_t i = 0, first = 0; i < n; ++i)
            if (endsSentence(words[i].text) || i + 1 == n || words[i + 1].start - words[i].end >= 1.2) {
                sentences.push_back({first, i});
                first = i + 1;
            }
        auto text = [&](size_t a, size_t b) {
            std::string out;
            for (size_t k = a; k <= b; ++k) out += (out.empty() ? "" : " ") + words[k].text;
            return out;
        };
        auto clean = [&](size_t before, size_t after) {  // a pause, a new speaker, or the edge of the media between them
            return words[after].start - words[before].end >= 0.4 ||
                   (words[before].speaker >= 0 && words[after].speaker >= 0 && words[before].speaker != words[after].speaker);
        };
        for (size_t si = 0; si < sentences.size(); ++si) {
            const size_t first = sentences[si].first;
            const double t0 = words[first].start;
            const std::string hookLine = text(first, sentences[si].second);
            const double hook = hookScore(hookLine);
            const bool startClean = first == 0 || clean(first - 1, first);
            for (size_t sj = si; sj < sentences.size(); ++sj) {
                const size_t last = sentences[sj].second;
                const double t1 = words[last].end, length = t1 - t0;
                if (length > o.maxSeconds) break;
                if (length < o.minSeconds) continue;
                int fillers = 0;
                double spoken = 0;
                std::set<int> speakers;
                std::set<std::string> hits;
                for (size_t k = first; k <= last; ++k) {
                    fillers += filler[k] ? 1 : 0;
                    spoken += std::max(0.0, words[k].end - words[k].start);
                    if (words[k].speaker >= 0) speakers.insert(words[k].speaker);
                    if (!topic.empty())
                        if (const std::string s = stem(key(words[k].text)); topic.count(s)) hits.insert(s);
                }
                const double count = double(last - first + 1);
                double score = 2 * hook - 3 * fillers / count;
                if (!topic.empty()) score += 2 * double(hits.size()) / double(topic.size());
                if (!curve.empty()) {
                    double acc = 0;
                    size_t windows = 0;
                    for (size_t w = size_t(t0 / kHighlightStep); w < curve.size() && double(w) * kHighlightStep < t1; ++w, ++windows) acc += curve[w];
                    if (windows) score += acc / double(windows);
                }
                if (startClean) score += 0.2;
                if (last + 1 == n || clean(last, last + 1)) score += 0.1;
                if (endsQuestion(words[last].text)) score -= 0.3;  // left hanging
                if (speakers.size() > 2) score -= 0.2 * double(speakers.size() - 2);
                if (const double talk = spoken / std::max(1e-6, length); talk < 0.5) score -= 0.5 - talk;  // mostly silence
                ShortMoment sm;
                sm.media = m->id;
                sm.firstWord = first;
                sm.lastWord = last;
                // A breath either side, never past halfway to the words around it (so neighbouring shorts never share a frame).
                sm.in = std::max(first > 0 ? (words[first - 1].end + t0) / 2 : 0.0, t0 - 0.12);
                sm.out = t1 + 0.3;
                if (last + 1 < n) sm.out = std::min(sm.out, (t1 + words[last + 1].start) / 2);
                if (m->duration > 0) sm.out = std::min(sm.out, m->duration);
                sm.in = std::min(sm.in, t0);
                sm.out = std::max(sm.out, t1);
                sm.score = score;
                sm.hook = hook;
                sm.hookLine = hookLine;
                sm.text = text(first, last);
                all.push_back(std::move(sm));
            }
        }
        if (progress) progress(double(mi + 1) / double(media.size()));
    }
    if (!anyTranscript) {
        if (error) *error = "Transcribe the media first: shorts are found from what is said";
        return {};
    }
    std::stable_sort(all.begin(), all.end(), [](const ShortMoment& a, const ShortMoment& b) { return a.score > b.score; });
    std::vector<ShortMoment> picked;
    for (const ShortMoment& c : all) {
        if (int(picked.size()) >= o.count) break;
        const bool overlaps = std::any_of(picked.begin(), picked.end(), [&](const ShortMoment& q) {
            return q.media == c.media && c.in < q.out && q.in < c.out;
        });
        if (!overlaps) picked.push_back(c);
    }
    if (picked.empty() && error)
        *error = "Nothing said fits between " + std::to_string(int(o.minSeconds)) + " and " + std::to_string(int(o.maxSeconds)) + " seconds";
    return picked;
}

Id makeShortSequence(Project& p, const ShortMoment& sm, const ShortBuild& b, const std::string& name, const std::atomic<bool>* cancel,
                     std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return Id(0);
    };
    const MediaItem* m = p.findMedia(sm.media);
    if (!m) return fail("No such media");
    if (sm.out <= sm.in) return fail("The moment is empty");
    const CaptionLook* look = b.captionLook.empty() ? nullptr : findCaptionLook(b.captionLook);
    if (!b.captionLook.empty() && !look) return fail("No caption look called \"" + b.captionLook + "\"");
    const std::string language = m->transcript ? m->transcript->language : std::string();
    // The frame: the active sequence's (or the footage's), at the platform's shape, keeping its shorter side.
    Sequence like;
    like.width = 1920, like.height = 1080;
    Rational fps{30, 1};
    const Sequence* active = p.active();
    if (active) {
        like.width = active->width, like.height = active->height, fps = active->fps;
    } else if (m->width > 0) {
        like.width = m->width, like.height = m->height;
        if (m->fps.valid()) fps = m->fps;
    }
    int w = like.width, h = like.height;
    reframeSize(like, b.aspectW, b.aspectH, w, h);
    const bool tall = h > w;
    Sequence s = makeSequence(p, name, w, h, fps, 1, 1);
    if (active) {
        s.sampleRate = active->sampleRate;
        s.colorSpace = active->colorSpace;
        s.hdrPeakNits = active->hdrPeakNits;
    }
    if (!layoutHighlights(p, s, {HighlightMoment{sm.media, sm.in, sm.out, sm.score}}).ok) return fail("The moment could not be placed");
    // Hesitations and long silences out.
    if (b.removeFillers || b.removePauses) {
        const std::vector<TranscriptWord> words = sequenceTranscriptWords(p, s);
        std::vector<FrameRange> cut;
        FillerOptions fo = b.fillers;
        if (fo.language.empty()) fo.language = language;
        if (b.removeFillers)
            for (const FrameRange& r : fillerWordRanges(words, s.fpsValue(), fo)) cut.push_back(r);
        if (b.removePauses)
            for (const FrameRange& r : pauseRanges(words, s.fpsValue(), 0.5, 0.25)) cut.push_back(r);
        if (!cut.empty()) rippleDeleteRanges(p, s, cut, b.smoothCut);
    }
    // Framed round the subject (or centred), filling the frame.
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips) {
            std::vector<ReframeKey> path;
            if (!b.reframe || !analyzeClipReframe(p, s, c, b.reframeSpeed, path, {}, cancel, nullptr) || path.empty()) path = {{0, 0.5, 0.5}};
            if (cancelled(cancel)) return fail("Cancelled");
            applyReframe(p, s, c, path);
        }
    // Sound without pictures (a podcast) gets an audiogram to look at.
    if (s.videoTracks[0].clips.empty() && s.duration() > 0) {
        Clip viz = makeGeneratorClip(p, "audio_viz", s.duration());
        viz.start = 0;
        if (tall) viz.generator.params["style"] = Param(3.0);  // a circle suits a tall frame
        edit::overwrite(p, s, {TrackKind::Video, 0}, viz);
    }
    if (look) {
        CaptionRules rules;
        rules.lineChars = tall ? 22 : 32;
        rules.maxLines = 2;
        rules.maxSeconds = 3.5;
        CaptionTrack track;
        track.id = p.newId();
        track.name = "Captions";
        if (!language.empty()) track.language = language;
        track.style = look->style;
        if (tall) track.style.position = std::min(track.style.position, 0.75);  // clear of the platforms' buttons and description
        track.captions = captionsFromTranscripts(p, s, rules);
        if (!track.captions.empty()) s.captionTracks.push_back(std::move(track));
    }
    if (b.hookTitle && !sm.hookLine.empty() && s.duration() > 0) {
        const FrameTime length = std::min<FrameTime>(s.duration(), FrameTime(std::lround(3.0 * s.fpsValue())));
        Clip title = makeGeneratorClip(p, "title", length);
        title.name = "Hook";
        title.start = 0;
        title.generator.strings["text"] = wrapCaptionText(sm.hookLine, tall ? 20 : 36, 4);
        title.generator.params["size"] = Param(std::round(h * (tall ? 0.034 : 0.06)));
        title.generator.params["pos_y"] = Param(-h * 0.28);
        title.generator.params["box_opacity"] = Param(80.0);
        title.generator.params["box_padding"] = Param(std::round(h * 0.012));
        title.generator.params["shadow"] = Param(0.0);
        title.generator.params["anim_in"] = Param(6.0);  // Pop
        title.generator.params["anim_in_dur"] = Param(0.35);
        title.generator.params["anim_out"] = Param(1.0);  // Fade
        title.generator.params["anim_out_dur"] = Param(0.3);
        while (s.videoTracks.size() < 2) edit::addTrack(p, s, TrackKind::Video);
        edit::overwrite(p, s, {TrackKind::Video, 1}, title);
    }
    MediaItem item;
    item.id = p.newId();
    item.kind = MediaKind::Sequence;
    item.name = s.name;
    item.sequenceId = s.id;
    item.hasVideo = item.hasAudio = true;
    item.width = s.width;
    item.height = s.height;
    item.fps = s.fps;
    const Id id = s.id;
    p.sequences.push_back(std::move(s));
    p.media.push_back(std::move(item));
    return id;
}

}  // namespace montage
