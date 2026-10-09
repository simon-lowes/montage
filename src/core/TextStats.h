// Montage — plain-text helpers for what is said: sentence ends, word keys, common words and rough stems, shared by
// the tools that read transcripts for meaning (Make Shorts, Suggest Chapters).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Transcript.h"

namespace montage {

// Lower case, letters, digits and apostrophes (curly ones made straight): "Here's," -> "here's".
std::string wordKey(const std::string& word);
// The keys of the words of a text.
std::vector<std::string> wordKeys(const std::string& text);
// Words too common to say what something is about ("the", "you", "really"), and anything under three letters.
bool isStopWord(const std::string& key);
// "stories" and "story", "editing" and "edit" alike, roughly (English).
std::string stemWord(std::string key);
// Whether the word ends a sentence: . ? ! … and the CJK forms, past closing quotes and brackets; not "Mr." or "e.g.".
bool endsSentence(const std::string& word);
bool endsQuestion(const std::string& word);
// The sentences of a run of words, as [first, last] indexes: to a word ending one, or to a silence of `pause` seconds.
std::vector<std::pair<size_t, size_t>> sentenceSpans(const std::vector<TranscriptWord>& words, double pause = 1.2);

}  // namespace montage
