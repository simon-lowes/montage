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

// "um", "uh", "erm", "hmm"... (case and punctuation ignored).
bool isFillerWord(const std::string& word);
// Frame ranges of the filler words (to the next word when it follows closely).
std::vector<FrameRange> fillerWordRanges(const std::vector<TranscriptWord>& words, double fps);
// Silences between words longer than `minPause` seconds, shortened to `keep` seconds.
std::vector<FrameRange> pauseRanges(const std::vector<TranscriptWord>& words, double fps, double minPause = 1.0,
                                    double keep = 0.3);

}  // namespace montage
