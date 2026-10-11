// Montage — chapters from chapter markers (as Premiere, Resolve and Final Cut
// make them): written into exported MP4, MOV and MKV files, and listed as
// YouTube reads them in a video's description ("0:00 Intro").
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct Chapter {
    FrameTime start = 0;  // relative to the start of the range
    FrameTime end = 0;
    std::string title;
};

// The chapter markers inside [in, out) (out < 0: the sequence's end), in order, with times from `in`; each runs
// to the next. When there are chapters but the first does not start at `in`, one named `intro` is put there.
std::vector<Chapter> chaptersOf(const Sequence& s, FrameTime in = 0, FrameTime out = -1, const std::string& intro = "Intro");

// YouTube's chapter list, a line each ("0:00 Intro", "1:05 Setup", H:MM:SS past an hour). `warning` says why YouTube
// may not show them: fewer than three, or one shorter than ten seconds.
std::string youtubeChapters(const Sequence& s, FrameTime in = 0, FrameTime out = -1, std::string* warning = nullptr);

}  // namespace montage
