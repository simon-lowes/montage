// Montage — finding moments by what is said, by meaning ("where they talk about money" finds "the budget was too
// tight"): transcripts are cut into passages a sentence apart, a few sentences long, which a sentence model embeds
// (media/SpeechSearch.h); a query is embedded the same way and the nearest passages are the moments. The parts that
// need no model live here: the passages, the ranking and the words a passage shares with the query.
#pragma once

#include <string>
#include <vector>

#include "Model.h"
#include "Transcript.h"

namespace montage {

struct SpokenPassage {
    double start = 0, end = 0;  // media seconds
    std::string text;
    size_t firstWord = 0, lastWord = 0;  // in the transcript's words, counted across segments
};

// Passages starting at each sentence: the sentence alone (four words or more), and running on through whole
// sentences to at least `targetWords` words (or the end); at most `maxWords` (a long sentence is cut there).
std::vector<SpokenPassage> spokenPassages(const Transcript& t, int targetWords = 30, int maxWords = 80);

struct SpokenHit {
    Id media = 0;
    double start = 0, end = 0;
    std::string text;
    float score = 0;  // similarity plus a little for words in common with the query
};

// The share (0-1) of the query's content words (stemmed, not stop words) that the passage says.
float sharedWords(const std::string& query, const std::string& passage);

// The best hits, best first: hits under `minScore` dropped, and a hit overlapping a better one of the same media by
// half its length or more left out.
std::vector<SpokenHit> bestSpokenHits(std::vector<SpokenHit> hits, size_t max = 20, float minScore = 0.12f);

}  // namespace montage
