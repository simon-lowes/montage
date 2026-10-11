// Montage — Suggest Chapters (Descript's chapters, YouTube's automatic chapters): where the talk in a cut moves on to
// something new, found from what is said (TextTiling: the words of the sentences either side of each break compared,
// and the breaks where they share least taken), each chapter titled with what it talks about more than the rest do.
#pragma once

#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

struct ChapterOptions {
    double minSeconds = 30;  // the shortest chapter (YouTube asks for 10 s; a chapter under half a minute rarely helps)
    int maxChapters = 0;     // 0 = as many as the talk has
    int blockSentences = 3;  // sentences compared either side of a break
};

struct SuggestedChapter {
    FrameTime start = 0;  // timeline frames; the first is 0
    std::string title;
    double depth = 0;     // how sharply the talk changes there (0 for the first)
};

// The chapters of the sequence's transcribed speech, in order; empty (with `error`) when too little is said, or the
// cut is too short, for two chapters.
std::vector<SuggestedChapter> suggestChapters(const Project& p, const Sequence& s, const ChapterOptions& o = {},
                                              std::string* error = nullptr);

namespace edit {

// The chapters as chapter markers (replacing the chapter markers there were, when `replace`; other markers stay).
Result addSuggestedChapters(Sequence& s, const std::vector<SuggestedChapter>& chapters, bool replace = true);

}  // namespace edit

}  // namespace montage
