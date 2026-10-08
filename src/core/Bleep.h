// Montage — bleeping words: speech covered by a tone (or silence) where a
// word is said, as broadcasters do for profanity and names. The stretches
// are kept on each clip as a "bleep" effect in source time, so trims and
// moves keep them on the word; the words are masked in the captions too.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "EditOps.h"
#include "Model.h"
#include "Transcript.h"

namespace montage {

using SecondsRange = std::pair<double, double>;

// A Bleep effect's stretches of source, in seconds (merged, sorted).
std::vector<SecondsRange> bleepRanges(const Effect& e);
void setBleepRanges(Effect& e, std::vector<SecondsRange> ranges);

// Bleeps the given words (timeline seconds, as from sequenceTranscriptWords):
// every unmuted audio clip there whose media has a transcript gets those
// stretches of its source, and the words are masked in the captions
// ("f***"). Fails when no clip is under any of them.
edit::Result bleepWords(Project& p, Sequence& s, const std::vector<TranscriptWord>& words, bool maskCaptions = true);

// Common English swear words (case and punctuation ignored); the words of `words` that are.
bool isProfanity(const std::string& word);
std::vector<TranscriptWord> profanity(const std::vector<TranscriptWord>& words);

// "f***": the first letter kept, the rest starred (punctuation around it kept).
std::string maskWord(const std::string& word);

}  // namespace montage
