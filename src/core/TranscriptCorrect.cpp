#include "TranscriptCorrect.h"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <limits>
#include <map>
#include <memory>

namespace montage {

namespace {

const std::string kContinued = "\x01";  // a later word of the same correction

struct Place {
    size_t segment, word;
};

std::vector<Place> places(const Transcript& t) {
    std::vector<Place> out;
    for (size_t s = 0; s < t.segments.size(); ++s)
        for (size_t w = 0; w < t.segments[s].words.size(); ++w) out.push_back({s, w});
    return out;
}

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    for (const QString& part : QString::fromStdString(text).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts))
        out.push_back(part.toStdString());
    return out;
}

// Lower case, letters and digits only (any script).
std::string key(const std::string& w) {
    QString out;
    for (const QChar c : QString::fromStdString(w))
        if (c.isLetterOrNumber()) out += c.toLower();
    return out.toStdString();
}

// What follows a word's last letter or digit ("," of "Smyth,").
std::string trailing(const std::string& w) {
    const QString q = QString::fromStdString(w);
    int i = int(q.size());
    while (i > 0 && !q.at(i - 1).isLetterOrNumber()) --i;
    return q.mid(i).toStdString();
}

void retext(TranscriptSegment& s) {
    std::string text;
    for (const TranscriptWord& w : s.words) text += (text.empty() ? "" : " ") + w.text;
    s.text = text;
}

// Words [first, last] become `words` over their span; the first new word's original is `heard` ("" = none).
bool replaceRange(Transcript& t, size_t first, size_t last, const std::vector<std::string>& words, const std::string& heard) {
    const std::vector<Place> at = places(t);
    if (words.empty() || first > last || last >= at.size()) return false;
    const TranscriptWord& a = t.segments[at[first].segment].words[at[first].word];
    const TranscriptWord& b = t.segments[at[last].segment].words[at[last].word];
    const double t0 = a.start, t1 = std::max(a.start, b.end);
    // Split the span by the words' lengths.
    std::vector<double> lengths;
    double total = 0;
    for (const std::string& w : words) total += lengths.emplace_back(std::max<double>(1, double(QString::fromStdString(w).size())));
    std::vector<TranscriptWord> made;
    double cursor = t0;
    for (size_t i = 0; i < words.size(); ++i) {
        TranscriptWord w;
        w.text = words[i];
        w.start = cursor;
        cursor = i + 1 == words.size() ? t1 : cursor + (t1 - t0) * lengths[i] / total;
        w.end = cursor;
        w.probability = 1;
        w.original = heard.empty() ? std::string() : i == 0 ? heard : kContinued;
        made.push_back(std::move(w));
    }
    // Out with the old words (from the last, so earlier places hold), in with the new where the first was.
    const Place home = at[first];
    for (size_t k = last + 1; k-- > first;) {
        auto& ws = t.segments[at[k].segment].words;
        ws.erase(ws.begin() + std::ptrdiff_t(at[k].word));
    }
    auto& ws = t.segments[home.segment].words;
    ws.insert(ws.begin() + std::ptrdiff_t(home.word), made.begin(), made.end());
    for (size_t s = at[first].segment; s <= at[last].segment; ++s) retext(t.segments[s]);
    return true;
}

size_t levenshtein(const std::string& a, const std::string& b) {
    const std::u32string x = QString::fromStdString(a).toStdU32String(), y = QString::fromStdString(b).toStdU32String();
    std::vector<size_t> prev(y.size() + 1), cur(y.size() + 1);
    for (size_t j = 0; j <= y.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= x.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= y.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[y.size()];
}

}  // namespace

bool correctWords(Transcript& t, size_t first, size_t last, const std::string& text) {
    const std::vector<Place> at = places(t);
    const std::vector<std::string> words = splitWords(text);
    if (words.empty() || first > last || last >= at.size()) return false;
    // A correction is corrected whole: widen to the start and end of any it cuts into.
    auto original = [&](size_t k) -> const std::string& { return t.segments[at[k].segment].words[at[k].word].original; };
    while (first > 0 && original(first) == kContinued) --first;
    while (last + 1 < at.size() && original(last + 1) == kContinued) ++last;
    // What was heard there: corrected words count as what they replaced.
    std::string heard;
    for (size_t k = first; k <= last; ++k) {
        const TranscriptWord& w = t.segments[at[k].segment].words[at[k].word];
        if (w.original == kContinued) continue;
        heard += (heard.empty() ? "" : " ") + (w.original.empty() ? w.text : w.original);
    }
    return replaceRange(t, first, last, words, heard);
}

bool revertCorrection(Transcript& t, size_t index) {
    const std::vector<Place> at = places(t);
    if (index >= at.size()) return false;
    auto word = [&](size_t k) -> const TranscriptWord& { return t.segments[at[k].segment].words[at[k].word]; };
    size_t g0 = index;
    while (g0 > 0 && word(g0).original == kContinued) --g0;
    if (word(g0).original.empty() || word(g0).original == kContinued) return false;
    size_t g1 = g0;
    while (g1 + 1 < at.size() && word(g1 + 1).original == kContinued) ++g1;
    const std::string heard = word(g0).original;
    return replaceRange(t, g0, g1, splitWords(heard), {});
}

int revertAllCorrections(Transcript& t) {
    int n = 0;
    for (bool again = true; again;) {
        again = false;
        const std::vector<Place> at = places(t);
        for (size_t k = 0; k < at.size(); ++k) {
            const std::string& o = t.segments[at[k].segment].words[at[k].word].original;
            if (!o.empty() && o != kContinued && revertCorrection(t, k)) {
                ++n;
                again = true;
                break;
            }
        }
    }
    return n;
}

int replaceInTranscripts(Project& p, const std::string& find, const std::string& replace) {
    std::vector<std::string> phrase;
    for (const std::string& w : splitWords(find))
        if (const std::string k = key(w); !k.empty()) phrase.push_back(k);
    if (phrase.empty() || splitWords(replace).empty()) return 0;
    int total = 0;
    for (MediaItem& m : p.media) {
        if (!m.transcript || m.subclipOf) continue;
        Transcript t = *m.transcript;
        std::vector<std::string> keys;
        for (const Place& pl : places(t)) keys.push_back(key(t.segments[pl.segment].words[pl.word].text));
        std::vector<size_t> hits;
        for (size_t i = 0; i + phrase.size() <= keys.size(); ++i)
            if (std::equal(phrase.begin(), phrase.end(), keys.begin() + std::ptrdiff_t(i))) {
                hits.push_back(i);
                i += phrase.size() - 1;
            }
        // From the last, so earlier indexes hold.
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
            const std::vector<Place> at = places(t);
            const TranscriptWord& end = t.segments[at[*it + phrase.size() - 1].segment].words[at[*it + phrase.size() - 1].word];
            const std::string tail = trailing(end.text), typed = trailing(replace).empty() ? replace + tail : replace;
            if (correctWords(t, *it, *it + phrase.size() - 1, typed)) ++total;
        }
        if (!hits.empty()) m.transcript = std::make_shared<Transcript>(std::move(t));
    }
    return total;
}

std::vector<VocabularySuggestion> vocabularySuggestions(const Project& p, const std::vector<std::string>& vocabulary) {
    struct Scored {
        VocabularySuggestion s;
        size_t distance;
    };
    std::vector<Scored> found;
    for (const MediaItem& m : p.media) {
        if (!m.transcript || m.subclipOf) continue;
        const Transcript& t = *m.transcript;
        const std::vector<Place> at = places(t);
        auto word = [&](size_t k) -> const TranscriptWord& { return t.segments[at[k].segment].words[at[k].word]; };
        for (const std::string& term : vocabulary) {
            const std::vector<std::string> termWords = splitWords(term);
            std::string termKey;
            for (const std::string& w : termWords) termKey += key(w);
            const size_t n = termWords.size();
            if (termKey.size() < 4 || n == 0) continue;  // short words match too much
            const size_t reach = std::max<size_t>(1, termKey.size() / 3);
            for (size_t i = 0; i + n <= at.size(); ++i) {
                std::string heardKey, heard;
                for (size_t k = i; k < i + n; ++k) heardKey += key(word(k).text), heard += (heard.empty() ? "" : " ") + word(k).text;
                if (heardKey.empty() || heardKey == termKey) continue;
                const size_t d = levenshtein(heardKey, termKey);
                if (d > reach) continue;
                found.push_back({{m.id, i, i + n - 1, heard, term}, d});
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Scored& a, const Scored& b) { return a.distance < b.distance; });
    std::vector<VocabularySuggestion> out;
    for (const Scored& f : found) out.push_back(f.s);
    return out;
}

int applyVocabularySuggestions(Project& p, const std::vector<VocabularySuggestion>& suggestions) {
    std::map<Id, std::vector<VocabularySuggestion>> byMedia;
    for (const VocabularySuggestion& s : suggestions) byMedia[s.media].push_back(s);
    int n = 0;
    for (auto& [id, list] : byMedia) {
        MediaItem* m = p.findMedia(id);
        if (!m || !m->transcript) continue;
        // From the last word back, so earlier indexes hold.
        std::stable_sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        auto t = std::make_shared<Transcript>(*m->transcript);
        size_t floor = std::numeric_limits<size_t>::max();
        for (const VocabularySuggestion& s : list) {
            if (s.last >= floor) continue;  // overlaps one already made
            const std::string tail = trailing(s.heard);
            if (correctWords(*t, s.first, s.last, trailing(s.term).empty() ? s.term + tail : s.term)) ++n, floor = s.first;
        }
        m->transcript = t;
    }
    return n;
}

std::string vocabularyPrompt(const std::vector<std::string>& vocabulary) {
    std::string terms;
    for (const std::string& v : vocabulary)
        if (!splitWords(v).empty()) terms += (terms.empty() ? "" : ", ") + QString::fromStdString(v).simplified().toStdString();
    return terms.empty() ? std::string() : "Glossary: " + terms + ".";
}

}  // namespace montage
