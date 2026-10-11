#include "SpokenSearch.h"

#include <algorithm>
#include <set>

#include "TextStats.h"

namespace montage {

std::vector<SpokenPassage> spokenPassages(const Transcript& t, int targetWords, int maxWords) {
    std::vector<TranscriptWord> words;
    for (const TranscriptSegment& s : t.segments) words.insert(words.end(), s.words.begin(), s.words.end());
    std::vector<SpokenPassage> out;
    if (words.empty()) return out;
    const std::vector<std::pair<size_t, size_t>> sentences = sentenceSpans(words);
    targetWords = std::max(1, targetWords);
    maxWords = std::max(targetWords, maxWords);
    auto add = [&](size_t first, size_t last) {
        last = std::min(last, first + size_t(maxWords) - 1);
        for (const SpokenPassage& o : out)
            if (o.firstWord == first && o.lastWord == last) return;
        SpokenPassage p;
        p.firstWord = first, p.lastWord = last;
        p.start = words[first].start, p.end = words[last].end;
        for (size_t k = first; k <= last; ++k) p.text += (k > first ? " " : "") + words[k].text;
        out.push_back(std::move(p));
    };
    for (size_t i = 0; i < sentences.size(); ++i) {
        // The sentence alone (a point made in one sentence is not diluted by the next), and with those after it.
        const size_t first = sentences[i].first;
        if (sentences[i].second - first + 1 >= 4) add(first, sentences[i].second);
        size_t last = sentences[i].second;
        for (size_t j = i + 1; j < sentences.size() && int(last - first + 1) < targetWords; ++j) last = sentences[j].second;
        // A window ending where the one before it ends says nothing new (the tail of the talk), unless it is all there is.
        if (!out.empty() && out.back().lastWord == last && out.back().firstWord < first && int(last - first + 1) < targetWords / 2 &&
            sentences[i].second != last)
            continue;
        add(first, last);
    }
    return out;
}

float sharedWords(const std::string& query, const std::string& passage) {
    std::set<std::string> want, have;
    for (const std::string& k : wordKeys(query))
        if (!isStopWord(k)) want.insert(stemWord(k));
    if (want.empty()) return 0;
    for (const std::string& k : wordKeys(passage)) have.insert(stemWord(k));
    int found = 0;
    for (const std::string& w : want) found += have.count(w) > 0;
    return float(found) / float(want.size());
}

std::vector<SpokenHit> bestSpokenHits(std::vector<SpokenHit> hits, size_t max, float minScore) {
    std::stable_sort(hits.begin(), hits.end(), [](const SpokenHit& a, const SpokenHit& b) { return a.score > b.score; });
    std::vector<SpokenHit> out;
    for (SpokenHit& h : hits) {
        if (h.score < minScore || out.size() >= max) break;
        const double len = std::max(1e-6, h.end - h.start);
        const bool covered = std::any_of(out.begin(), out.end(), [&](const SpokenHit& o) {
            return o.media == h.media && std::min(o.end, h.end) - std::max(o.start, h.start) >= 0.5 * len;
        });
        if (!covered) out.push_back(std::move(h));
    }
    return out;
}

}  // namespace montage
