// Montage — editing by transcript: the words a sequence plays and where,
// and ripple deletions of words, filler words and pauses.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "EditOps.h"
#include "Model.h"
#include "Transcript.h"

namespace montage {

// Every transcribed word heard in the cut, in timeline seconds and in order:
// each word is placed where its clip plays it (trims, speed, reverse) from
// unmuted (or soloed) audio tracks, or from video clips when no audio clip
// has a transcript. The same word from linked or stacked copies counts once.
std::vector<TranscriptWord> sequenceTranscriptWords(const Project& p, const Sequence& seq);
// The language (ISO 639-1) all the sequence's transcribed clips share, "" when they differ or none is known.
std::string sequenceTranscriptLanguage(const Project& p, const Sequence& seq);

using FrameRange = std::pair<FrameTime, FrameTime>;  // [first, end)

// Merges overlapping or touching ranges and drops empty ones.
std::vector<FrameRange> mergeRanges(std::vector<FrameRange> ranges);

// Removes the frame ranges from every editable track and from the caption
// tracks, closing the gaps (sync-locked ripple delete). With `smoothCut`
// frames, each join this leaves between two video clips gets a Smooth Cut
// transition that long, so the jump in the picture morphs across.
edit::Result rippleDeleteRanges(Project& p, Sequence& s, std::vector<FrameRange> ranges, FrameTime smoothCut = 0);

// Removes [a, b) from caption tracks: captions inside go, captions across the
// range are shortened, and later captions move up.
void rippleCaptions(Sequence& s, FrameTime a, FrameTime b);

// Filler words (Descript's and Premiere's filler word removal), in English, Spanish, French, German, Italian,
// Portuguese, Dutch, Swedish, Danish, Norwegian, Polish, Czech, Russian, Turkish, Japanese, Chinese and Korean.
struct FillerOptions {
    std::string language;             // ISO 639-1 of the speech; "" = any of them (only sounds that are never words)
    bool discourse = false;           // also "like", "you know", "I mean", "genre", "halt"... when set off by commas or pauses
    std::vector<std::string> custom;  // the editor's own words or phrases (Project::fillerWords), always
};
// Hesitations: "um", "uh", "euh", "äh", "eh", "えーと", "嗯"... (case and punctuation ignored).
bool isFillerWord(const std::string& word, const std::string& language = {});
// Which words are fillers; a phrase ("you know") marks each of its words.
std::vector<bool> fillerWordMask(const std::vector<TranscriptWord>& words, const FillerOptions& o = {});
// Frame ranges of the filler words (to the next word when it follows closely).
std::vector<FrameRange> fillerWordRanges(const std::vector<TranscriptWord>& words, double fps, const FillerOptions& o = {});
// Silences between words longer than `minPause` seconds, shortened to `keep` seconds.
std::vector<FrameRange> pauseRanges(const std::vector<TranscriptWord>& words, double fps, double minPause = 1.0,
                                    double keep = 0.3);

// Retakes (like Descript's): where a speaker breaks off and starts the same
// words again ("So today we're going to, um, so today we're going to talk
// about..."), the broken-off attempts, from the first attempt to the start of
// the last. A repeat counts when at least `minWords` words match (one of them
// four letters or more) within `maxWords` words, and the attempt before was
// left unfinished: it did not end a sentence, or it held a filler or "sorry",
// "again", "let me". A finished sentence said twice for effect is kept.
// `takes`, if given, gets the (first attempt, kept take) word indices.
std::vector<FrameRange> retakeRanges(const std::vector<TranscriptWord>& words, double fps, int minWords = 3, int maxWords = 30,
                                     std::vector<std::pair<size_t, size_t>>* takes = nullptr);

}  // namespace montage
