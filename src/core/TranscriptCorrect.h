// Montage — correcting transcripts (Premiere's transcript editing, Avid's corrections with the original kept,
// Descript's Correct): speech-to-text misspells names and jargon, and the errors reach captions, search, script cuts
// and B-roll matching. Words are fixed in place, keeping their timing and what was heard; a phrase can be replaced in
// every transcript of the project; and the project's vocabulary both prompts speech-to-text and finds near misses.
#pragma once

#include <string>
#include <vector>

#include "Model.h"
#include "Transcript.h"

namespace montage {

// Words [first, last] of the transcript (counted across its segments) replaced by `text`: the typed words share
// the old words' span (split by their lengths), the first starting and the last ending where the old ones did,
// and the first keeps what was heard for Revert. The segments' text follows. False for an empty range or text.
bool correctWords(Transcript& t, size_t first, size_t last, const std::string& text);
// The correction word `index` belongs to put back as it was heard (timed evenly over its span). False if the word
// was never corrected.
bool revertCorrection(Transcript& t, size_t index);
// Every correction put back. Returns how many.
int revertAllCorrections(Transcript& t);
// Every whole-word occurrence of `find` (case and punctuation ignored, one or more words) in every transcript of
// the project replaced by `replace`, keeping the punctuation that followed. Returns how many.
int replaceInTranscripts(Project& p, const std::string& find, const std::string& replace);

// A word (or words) heard close to a vocabulary term: the near misses speech-to-text makes of names.
struct VocabularySuggestion {
    Id media = 0;
    size_t first = 0, last = 0;  // word indexes in that media's transcript
    std::string heard, term;
};
// Every place in the project's transcripts where words differ from a vocabulary term by a letter or two (up to one
// in three, case and punctuation ignored; terms under four letters are left), best first. Terms of several words
// match as many words.
std::vector<VocabularySuggestion> vocabularySuggestions(const Project& p, const std::vector<std::string>& vocabulary);
// Corrects the suggestions (overlapping ones once), keeping the punctuation after what was heard. Returns how many.
int applyVocabularySuggestions(Project& p, const std::vector<VocabularySuggestion>& suggestions);
// The prompt that tells speech-to-text the terms to expect ("Glossary: Montage, Kokoro.").
std::string vocabularyPrompt(const std::vector<std::string>& vocabulary);

}  // namespace montage
