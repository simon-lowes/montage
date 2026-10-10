#include "TranscriptEdit.h"
#include "Adr.h"

#include <cctype>

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <algorithm>
#include <cmath>

namespace montage {

std::string sequenceTranscriptLanguage(const Project& p, const Sequence& seq) {
    std::string language;
    bool mixed = false;
    for (const Track& t : seq.audioTracks)
        for (const Clip& c : t.clips) {
            const MediaItem* m = p.findMedia(c.mediaId);
            if (!m || !m->transcript || m->transcript->language.empty()) continue;
            if (language.empty()) language = m->transcript->language;
            else if (language != m->transcript->language) mixed = true;
        }
    return mixed ? std::string() : language;
}

std::vector<TranscriptWord> sequenceTranscriptWords(const Project& p, const Sequence& seq) {
    const double fps = seq.fpsValue() > 0 ? seq.fpsValue() : 30.0;
    // Every transcribed word heard in the cut, in timeline seconds.
    auto collect = [&](const std::vector<Track>& tracks, bool audio) {
        std::vector<TranscriptWord> words;
        const bool anySolo = audio && std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.solo; });
        for (const Track& tr : tracks) {
            if (tr.muted || (anySolo && !tr.solo)) continue;
            for (const Clip& c : tr.clips) {
                if (!c.enabled || c.isGenerator() || c.speed <= 0) continue;
                const MediaItem* m = p.findMedia(c.mediaId);
                if (!m || !m->transcript) continue;
                const double srcA = c.sourceIn, srcB = c.sourceIn + c.sourceExtent();  // source frames shown
                auto toTimeline = [&](double srcFrame) {
                    if (c.ramped()) return double(c.start) + c.localForSource(srcFrame);  // Time Remapping
                    const double rel = c.reverse ? (srcB - srcFrame) : (srcFrame - srcA);
                    return double(c.start) + rel / c.speed;
                };
                for (const auto& s : m->transcript->segments)
                    for (const auto& w : s.words) {
                        const double mid = (w.start + w.end) * 0.5 * fps;
                        if (mid < srcA || mid >= srcB) continue;
                        double a = toTimeline(w.start * fps), b = toTimeline(w.end * fps);
                        if (a > b) std::swap(a, b);
                        a = std::clamp(a, double(c.start), double(c.end()));
                        b = std::clamp(b, a, double(c.end()));
                        words.push_back({a / fps, b / fps, w.text, w.probability, speakerName(*m->transcript, s.speaker)});
                    }
            }
        }
        std::stable_sort(words.begin(), words.end(),
                         [](const TranscriptWord& x, const TranscriptWord& y) { return x.start < y.start; });
        // The same words from linked or stacked copies of a clip count once.
        std::vector<TranscriptWord> unique;
        for (const auto& w : words) {
            bool dup = false;
            for (auto it = unique.rbegin(); it != unique.rend() && w.start - it->start < 0.15; ++it)
                if (it->text == w.text) dup = true;
            if (!dup) unique.push_back(w);
        }
        return unique;
    };
    std::vector<TranscriptWord> words = collect(seq.audioTracks, true);
    if (words.empty()) words = collect(seq.videoTracks, false);

    return words;
}

std::vector<FrameRange> mergeRanges(std::vector<FrameRange> ranges) {
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [](const FrameRange& r) { return r.second <= r.first; }),
                 ranges.end());
    std::sort(ranges.begin(), ranges.end());
    std::vector<FrameRange> out;
    for (const auto& r : ranges) {
        if (!out.empty() && r.first <= out.back().second) out.back().second = std::max(out.back().second, r.second);
        else out.push_back(r);
    }
    return out;
}

void rippleCaptions(Sequence& s, FrameTime a, FrameTime b) {
    if (b <= a) return;
    const FrameTime len = b - a;
    for (CaptionTrack& t : s.captionTracks) {
        std::vector<Caption> kept;
        for (Caption c : t.captions) {
            if (c.end <= a) {
                kept.push_back(c);
                continue;
            }
            if (c.start >= b) {
                c.start -= len;
                c.end -= len;
                kept.push_back(c);
                continue;
            }
            // Overlaps the range: keep what is outside it.
            const FrameTime before = std::max<FrameTime>(0, a - c.start), after = std::max<FrameTime>(0, c.end - b);
            if (before + after <= 0) continue;
            c.start = std::min(c.start, a);
            c.end = c.start + before + after;
            kept.push_back(c);
        }
        t.captions = std::move(kept);
        normalizeCaptions(t.captions);
    }
}

edit::Result rippleDeleteRanges(Project& p, Sequence& s, std::vector<FrameRange> ranges, FrameTime smoothCut) {
    ranges = mergeRanges(std::move(ranges));
    if (ranges.empty()) return edit::Result::fail("Nothing to delete");
    const auto tracks = allTracks(s);
    // From the last range back, so earlier positions stay valid.
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        auto r = edit::extractRange(p, s, it->first, it->second, tracks);
        if (!r.ok) return r;
        rippleCaptions(s, it->first, it->second);
        rippleAdrCues(s, it->first, it->second);
    }
    edit::Result res;
    for (const auto& r : ranges) res.applied += r.second - r.first;
    if (smoothCut > 0) {
        // Where each range was, after the earlier ones closed up.
        FrameTime removed = 0;
        for (const auto& r : ranges) {
            const FrameTime at = r.first - removed;
            removed += r.second - r.first;
            for (size_t ti = 0; ti < s.videoTracks.size(); ++ti) {
                const Track& t = s.videoTracks[ti];
                if (t.locked) continue;
                for (size_t k = 0; k + 1 < t.clips.size(); ++k)
                    if (t.clips[k].end() == at && t.clips[k + 1].start == at && !t.clips[k].isGenerator() &&
                        !t.clips[k + 1].isGenerator()) {
                        edit::addTransition(p, s, t.clips[k].id, edit::Edge::Out, "smooth_cut",
                                            std::min({smoothCut, t.clips[k].duration, t.clips[k + 1].duration}));
                        break;
                    }
            }
        }
    }
    return res;
}

namespace {

// A word as compared: lower case, letters only (any script).
QString fillerKey(const std::string& word) {
    QString w = QString::fromStdString(word).toLower();
    static const QRegularExpression nonLetters(QStringLiteral("[^\\p{L}]"));
    w.remove(nonLetters);
    return w;
}

struct FillerLanguage {
    const char* code;
    std::vector<const char*> hesitations;  // sounds, never words in this language
    std::vector<const char*> discourse;    // words and phrases that fill when set off ("like", "you know")
    std::vector<const char*> safe;         // of the hesitations, those that are no word in any language either
};

const std::vector<FillerLanguage>& fillerLanguages() {
    static const std::vector<FillerLanguage> all = {
        {"en", {"um", "umm", "ummm", "uh", "uhh", "uhm", "er", "erm", "err", "ah", "ahh", "eh", "hmm", "hm", "mm", "mmm", "mhm"},
         {"like", "you know", "i mean", "basically", "actually", "literally", "sort of", "kind of", "you see", "okay so", "right"},
         {"um", "umm", "ummm", "uh", "uhh", "uhm", "erm", "ahh", "hmm", "hm", "mm", "mmm", "mhm"}},
        {"es", {"eh", "ehh", "em", "emm", "mm", "mmm", "hmm"}, {"o sea", "este", "pues", "bueno", "digamos", "vale", "sabes", "tipo"}, {"ehh", "emm"}},
        {"fr", {"euh", "euhh", "heu", "hum", "bah", "ben", "mm", "hmm"}, {"genre", "du coup", "en fait", "tu vois", "quoi", "voilà", "bon"},
         {"euh", "euhh", "heu"}},
        {"de", {"äh", "ähm", "äähm", "öh", "öhm", "hm", "hmm", "mm", "eh"}, {"also", "halt", "quasi", "sozusagen", "irgendwie", "ne", "naja", "genau"},
         {"äh", "ähm", "äähm", "öh", "öhm"}},
        {"it", {"ehm", "eh", "ehh", "mm", "mmm", "hmm"}, {"cioè", "tipo", "allora", "praticamente", "insomma", "diciamo", "comunque"}, {"ehm"}},
        {"pt", {"hã", "ahn", "éé", "hum", "hmm", "mm", "eh"}, {"tipo", "né", "então", "sabe", "assim", "pois"}, {"ahn", "éé"}},
        {"nl", {"eh", "ehm", "uh", "uhm", "hmm", "mm"}, {"zeg maar", "eigenlijk", "nou", "dus", "weet je"}, {"ehm"}},
        {"sv", {"öh", "öhm", "eh", "äh", "hmm", "mm"}, {"liksom", "typ", "alltså", "ba", "asså"}, {"öh", "öhm"}},
        {"da", {"øh", "øhm", "æh", "hmm", "mm"}, {"altså", "ligesom", "sådan", "ikke"}, {"øh", "øhm", "æh"}},
        {"no", {"eh", "ehm", "øh", "hmm", "mm"}, {"liksom", "altså", "på en måte", "ikke sant"}, {"øh"}},
        {"pl", {"yyy", "yy", "eee", "ee", "mmm", "hmm"}, {"no", "jakby", "wiesz", "znaczy", "po prostu", "tak jakby"}, {"yyy", "eee"}},
        {"cs", {"ehm", "eee", "hmm", "mm"}, {"jako", "prostě", "vlastně", "takže", "no"}, {"eee"}},
        {"ru", {"э", "ээ", "эээ", "эм", "мм", "хм", "ммм"}, {"ну", "типа", "как бы", "короче", "это самое", "вот", "значит"},
         {"э", "ээ", "эээ", "эм", "мм", "хм", "ммм"}},
        {"tr", {"ıı", "ııı", "ee", "eee", "hmm", "mm"}, {"şey", "yani", "işte", "hani"}, {"ııı", "eee"}},
        {"ja", {"えー", "えーと", "えっと", "ええと", "あー", "うーん", "んー"}, {"あの", "あのー", "その", "なんか", "まあ"},
         {"えー", "えーと", "えっと", "ええと", "うーん", "んー"}},
        {"zh", {"嗯", "呃", "额", "啊", "唔"}, {"那个", "就是", "然后", "这个"}, {"嗯", "呃", "唔"}},
        {"ko", {"음", "어", "어어", "으음", "음음"}, {"그", "저", "뭐", "그러니까", "약간"}, {"으음", "음음"}},
    };
    return all;
}

const FillerLanguage* fillerLanguage(const std::string& code) {
    for (const FillerLanguage& l : fillerLanguages())
        if (code.size() >= 2 && code.compare(0, 2, l.code) == 0) return &l;
    return nullptr;
}

// The words of a phrase as compared.
std::vector<QString> phraseKeys(const std::string& phrase) {
    std::vector<QString> keys;
    for (const QString& part : QString::fromStdString(phrase).split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (const QString k = fillerKey(part.toStdString()); !k.isEmpty()) keys.push_back(k);
    return keys;
}

bool endsWithComma(const std::string& w) {
    for (auto it = w.rbegin(); it != w.rend(); ++it) {
        if (*it == ',' || *it == '.' || *it == '!' || *it == '?' || *it == ';' || *it == ':') return true;
        if (*it != ' ' && *it != '"' && *it != '\'') return false;
    }
    return false;
}

}  // namespace

bool isFillerWord(const std::string& word, const std::string& language) {
    const QString w = fillerKey(word);
    if (w.isEmpty()) return false;
    auto in = [&](const std::vector<const char*>& list) {
        return std::any_of(list.begin(), list.end(), [&](const char* f) { return w == QString::fromUtf8(f); });
    };
    if (const FillerLanguage* l = fillerLanguage(language)) return in(l->hesitations);
    // The language unknown: the hesitations that are no word anywhere.
    for (const FillerLanguage& l : fillerLanguages())
        if (in(l.safe)) return true;
    return false;
}

std::vector<bool> fillerWordMask(const std::vector<TranscriptWord>& words, const FillerOptions& o) {
    std::vector<bool> mask(words.size(), false);
    std::vector<QString> keys;
    keys.reserve(words.size());
    for (const TranscriptWord& w : words) keys.push_back(fillerKey(w.text));
    for (size_t i = 0; i < words.size(); ++i) mask[i] = isFillerWord(words[i].text, o.language);
    // Phrases: the editor's own always, the language's discourse fillers only when set off (a comma or a pause either side).
    auto mark = [&](const std::string& phrase, bool setOff) {
        const std::vector<QString> p = phraseKeys(phrase);
        if (p.empty()) return;
        for (size_t i = 0; i + p.size() <= words.size(); ++i) {
            bool match = true;
            for (size_t k = 0; k < p.size() && match; ++k) match = keys[i + k] == p[k];
            if (!match) continue;
            const size_t last = i + p.size() - 1;
            if (setOff) {
                const bool before = i == 0 || endsWithComma(words[i - 1].text) || words[i].start - words[i - 1].end >= 0.3;
                const bool after = last + 1 == words.size() || endsWithComma(words[last].text) || words[last + 1].start - words[last].end >= 0.3;
                if (!before || !after) continue;
            }
            for (size_t k = i; k <= last; ++k) mask[k] = true;
        }
    };
    for (const std::string& c : o.custom) mark(c, false);
    if (o.discourse) {
        if (const FillerLanguage* l = fillerLanguage(o.language)) {
            for (const char* d : l->discourse) mark(d, true);
        } else {
            for (const FillerLanguage& l : fillerLanguages())
                for (const char* d : l.discourse) mark(d, true);
        }
    }
    return mask;
}

std::vector<FrameRange> fillerWordRanges(const std::vector<TranscriptWord>& words, double fps, const FillerOptions& o) {
    const std::vector<bool> mask = fillerWordMask(words, o);
    std::vector<FrameRange> out;
    for (size_t i = 0; i < words.size(); ++i) {
        if (!mask[i]) continue;
        size_t j = i;
        while (j + 1 < words.size() && mask[j + 1]) ++j;  // a phrase, or fillers in a row, as one cut
        double end = words[j].end;
        // Take the short breath after the filler too, so the next word starts cleanly.
        if (j + 1 < words.size() && words[j + 1].start - end < 0.25) end = words[j + 1].start;
        out.emplace_back(FrameTime(std::llround(words[i].start * fps)), FrameTime(std::llround(end * fps)));
        i = j;
    }
    return mergeRanges(out);
}

namespace {

// Lower case, letters, digits and apostrophes only.
std::string plainWord(const std::string& w) {
    std::string out;
    for (unsigned char c : w)
        if (std::isalnum(c) || c == '\'' || c >= 0x80) out += char(std::tolower(c));
    return out;
}

bool endsSentence(const std::string& w) {
    for (auto it = w.rbegin(); it != w.rend(); ++it) {
        if (*it == '.' || *it == '!' || *it == '?') return true;
        if (std::isalnum(static_cast<unsigned char>(*it))) return false;
    }
    return false;
}

}  // namespace

std::vector<FrameRange> retakeRanges(const std::vector<TranscriptWord>& words, double fps, int minWords, int maxWords,
                                     std::vector<std::pair<size_t, size_t>>* takes) {
    std::vector<std::string> w;
    for (const TranscriptWord& t : words) w.push_back(plainWord(t.text));
    const size_t n = w.size(), m = size_t(std::max(2, minWords));
    auto same = [&](size_t a, size_t b) {
        if (b + m > n) return false;
        bool content = false;
        for (size_t k = 0; k < m; ++k) {
            if (w[a + k].empty() || w[a + k] != w[b + k]) return false;
            content |= w[a + k].size() >= 4;
        }
        return content;
    };
    // An attempt broken off: no sentence finished in it, or a filler or an apology in it.
    auto unfinished = [&](size_t from, size_t to) {
        bool finished = false;
        for (size_t k = from; k < to; ++k) {
            if (isFillerWord(words[k].text) || w[k] == "sorry" || w[k] == "again" || (w[k] == "let" && k + 1 < to && w[k + 1] == "me"))
                return true;
            finished |= endsSentence(words[k].text);
        }
        return !finished;
    };
    std::vector<FrameRange> out;
    size_t k = 0;
    while (k < n) {
        // The earliest attempt within reach that this one restarts, chained back through attempts in between.
        size_t first = k;
        for (size_t i = k > size_t(maxWords) ? k - size_t(maxWords) : 0; i < k; ++i)
            if (i + m <= k && same(i, k) && unfinished(i, k)) {
                first = i;
                break;
            }
        if (first < k) {
            if (takes) takes->push_back({first, k});
            out.emplace_back(FrameTime(std::llround(words[first].start * fps)), FrameTime(std::llround(words[k].start * fps)));
            k += m;
        } else {
            ++k;
        }
    }
    return mergeRanges(out);
}

std::vector<FrameRange> pauseRanges(const std::vector<TranscriptWord>& words, double fps, double minPause, double keep) {
    std::vector<FrameRange> out;
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        const double gap = words[i + 1].start - words[i].end;
        if (gap <= minPause) continue;
        const FrameTime a = FrameTime(std::ceil((words[i].end + keep / 2) * fps));
        const FrameTime b = FrameTime(std::floor((words[i + 1].start - keep / 2) * fps));
        if (b > a) out.emplace_back(a, b);
    }
    return mergeRanges(out);
}

}  // namespace montage
