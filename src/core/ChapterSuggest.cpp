#include "ChapterSuggest.h"

#include <QString>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "TextStats.h"
#include "TranscriptEdit.h"

namespace montage {

namespace {

using Terms = std::map<std::string, double>;

double cosine(const Terms& a, const Terms& b) {
    double dot = 0, na = 0, nb = 0;
    for (const auto& [k, v] : a) {
        na += v * v;
        if (auto it = b.find(k); it != b.end()) dot += v * it->second;
    }
    for (const auto& [k, v] : b) nb += v * v;
    return na > 0 && nb > 0 ? dot / std::sqrt(na * nb) : 0.0;
}

// The words of what is said there: runs of telling words (no common words, no break at punctuation).
struct Phrase {
    std::string key;      // stems, space-separated
    std::string surface;  // as said, first time
};

std::vector<Phrase> phrases(const std::vector<TranscriptWord>& words, size_t first, size_t last) {
    std::vector<Phrase> out;
    std::vector<std::pair<std::string, std::string>> run;  // (stem, word)
    auto flush = [&] {
        for (size_t i = 0; i < run.size(); ++i) {
            if (run[i].first.size() >= 4) out.push_back({run[i].first, run[i].second});
            if (i + 1 < run.size()) out.push_back({run[i].first + " " + run[i + 1].first, run[i].second + " " + run[i + 1].second});
        }
        run.clear();
    };
    for (size_t k = first; k <= last; ++k) {
        const std::string key = wordKey(words[k].text);
        if (key.empty() || isStopWord(key) || std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            flush();
            continue;
        }
        run.push_back({stemWord(key), key});
        const QString w = QString::fromStdString(words[k].text);
        if (!w.isEmpty() && !w.back().isLetterOrNumber()) flush();  // a comma or full stop ends the phrase
    }
    flush();
    return out;
}

std::string titleCase(std::string s) {
    QString q = QString::fromStdString(s);
    if (!q.isEmpty()) q[0] = q[0].toUpper();
    return q.toStdString();
}

}  // namespace

std::vector<SuggestedChapter> suggestChapters(const Project& p, const Sequence& s, const ChapterOptions& o, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<SuggestedChapter>{};
    };
    const double fps = s.fpsValue(), total = double(s.duration()) / fps;
    const std::vector<TranscriptWord> words = sequenceTranscriptWords(p, s);
    if (words.empty()) return fail("Nothing in this sequence has been transcribed");
    if (total < 2 * o.minSeconds) return fail("The cut is too short for chapters of at least " + std::to_string(int(o.minSeconds)) + " seconds");
    const auto sentences = sentenceSpans(words);
    const int k = std::max(1, o.blockSentences);
    if (int(sentences.size()) < 2 * k) return fail("Too little is said to find where the talk moves on");
    // The telling words of each sentence, by stem.
    std::vector<Terms> terms(sentences.size());
    for (size_t i = 0; i < sentences.size(); ++i)
        for (size_t w = sentences[i].first; w <= sentences[i].second; ++w)
            if (const std::string key = wordKey(words[w].text); !key.empty() && !isStopWord(key)) terms[i][stemWord(key)] += 1;
    // How much the sentences either side of each break share, smoothed.
    const size_t gaps = sentences.size() - 1;
    std::vector<double> sim(gaps);
    for (size_t g = 0; g < gaps; ++g) {
        Terms left, right;
        for (size_t i = size_t(std::max<long>(0, long(g) - k + 1)); i <= g; ++i)
            for (const auto& [t, v] : terms[i]) left[t] += v;
        for (size_t i = g + 1; i <= std::min(sentences.size() - 1, g + size_t(k)); ++i)
            for (const auto& [t, v] : terms[i]) right[t] += v;
        sim[g] = cosine(left, right);
    }
    std::vector<double> smooth(gaps);
    for (size_t g = 0; g < gaps; ++g) {
        const double a = sim[g ? g - 1 : g], b = sim[g + 1 < gaps ? g + 1 : g];
        smooth[g] = (a + 2 * sim[g] + b) / 4;
    }
    // Depth: how far the sharing falls at the break from the highs either side.
    std::vector<double> depth(gaps, 0.0);
    for (size_t g = 0; g < gaps; ++g) {
        double lp = smooth[g], rp = smooth[g];
        for (size_t j = g; j > 0 && smooth[j - 1] >= smooth[j]; --j) lp = smooth[j - 1];
        for (size_t j = g; j + 1 < gaps && smooth[j + 1] >= smooth[j]; ++j) rp = smooth[j + 1];
        depth[g] = (lp - smooth[g]) + (rp - smooth[g]);
    }
    double mean = 0;
    for (double d : depth) mean += d;
    mean /= double(gaps);
    // Where each chapter would start: just before its first word.
    auto startAt = [&](size_t g) {
        const double end = words[sentences[g].second].end, next = words[sentences[g + 1].first].start;
        return std::max(end, next - 0.4);
    };
    std::vector<size_t> order(gaps);
    for (size_t g = 0; g < gaps; ++g) order[g] = g;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return depth[a] > depth[b]; });
    const int cap = o.maxChapters > 0 ? o.maxChapters : std::max(3, int(total / 90));
    std::vector<size_t> breaks;
    for (size_t g : order) {
        if (int(breaks.size()) + 1 >= cap) break;
        if (depth[g] < std::max(0.05, mean)) break;
        const double t = startAt(g);
        if (t < o.minSeconds || total - t < o.minSeconds) continue;
        if (std::any_of(breaks.begin(), breaks.end(), [&](size_t b) { return std::fabs(startAt(b) - t) < o.minSeconds; })) continue;
        breaks.push_back(g);
    }
    if (breaks.empty()) return fail("The talk stays on one subject: no place to start a new chapter");
    std::sort(breaks.begin(), breaks.end());
    // The chapters' word ranges, and their titles: the phrase each says more than the others do.
    struct Range {
        size_t first, last;
        FrameTime start;
        double depth;
    };
    std::vector<Range> ranges;
    ranges.push_back({0, sentences[breaks[0]].second, 0, 0});
    for (size_t i = 0; i < breaks.size(); ++i) {
        const size_t g = breaks[i];
        const size_t last = i + 1 < breaks.size() ? sentences[breaks[i + 1]].second : words.size() - 1;
        ranges.push_back({sentences[g + 1].first, last, FrameTime(std::llround(startAt(g) * fps)), depth[g]});
    }
    std::vector<std::map<std::string, std::pair<int, std::string>>> counts(ranges.size());  // phrase -> (times, as said)
    std::map<std::string, int> chaptersWith;
    for (size_t c = 0; c < ranges.size(); ++c) {
        for (const Phrase& ph : phrases(words, ranges[c].first, ranges[c].last)) {
            auto& [n, surface] = counts[c][ph.key];
            if (n++ == 0) surface = ph.surface;
        }
        for (const auto& [key, v] : counts[c]) chaptersWith[key] += 1;
    }
    std::vector<SuggestedChapter> out;
    std::set<std::string> used;
    for (size_t c = 0; c < ranges.size(); ++c) {
        std::string best, title;
        double bestScore = 0;
        for (const auto& [key, v] : counts[c]) {
            const bool pair = key.find(' ') != std::string::npos;
            if (pair && v.first < 2) continue;  // a pair of words counts once it recurs
            if (used.count(key)) continue;      // no two chapters titled alike
            const double score = v.first * std::log(1.0 + double(ranges.size()) / chaptersWith[key]) * (pair ? 1.5 : 1.0);
            if (score > bestScore) bestScore = score, best = key, title = v.second;
        }
        if (!best.empty()) used.insert(best);
        out.push_back({ranges[c].start, best.empty() ? "Chapter " + std::to_string(c + 1) : titleCase(title), ranges[c].depth});
    }
    return out;
}

namespace edit {

Result addSuggestedChapters(Sequence& s, const std::vector<SuggestedChapter>& chapters, bool replace) {
    if (chapters.empty()) return Result::fail("No chapters to add");
    if (replace) std::erase_if(s.markers, [](const Marker& m) { return m.chapter; });
    for (const SuggestedChapter& c : chapters) addMarker(s, Marker{c.start, 0, c.title, {}, 0, true});
    return {};
}

}  // namespace edit

}  // namespace montage
