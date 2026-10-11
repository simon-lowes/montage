// Montage — review and approval without a service (Frame.io in Premiere, Blackmagic Cloud review in Resolve): a review
// copy of the cut goes out with a single HTML page that plays it in any browser, offline, where each reviewer steps
// to a frame (or marks a range) and types a note. The page keeps the notes in the browser and saves them as a small
// JSON file; that file (or several, one per reviewer) comes back and its notes become markers at their frames,
// coloured by reviewer.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct ReviewPageInfo {
    std::string title;       // shown on the page and in the notes file's name
    std::string id;          // keeps each review's notes apart in the browser
    std::string videoFile;   // the review copy, relative to the page
    Rational fps{25, 1};
    bool dropFrame = false;  // timecodes as 29.97 / 59.94 drop frame
    FrameTime offset = 0;    // the sequence frame the video starts at (an exported range)
    FrameTime frames = 0;    // the video's length
    int width = 1920, height = 1080;
    std::vector<Marker> markers;  // the editor's markers, in sequence frames
    std::string note;        // the editor's message to the reviewers ("" = none)
};

// The page for a review copy of `s` (frames `in` to `out` exclusive; -1 = whole), with its markers when asked.
ReviewPageInfo reviewPageInfo(const Sequence& s, const std::string& videoFile, FrameTime in = -1, FrameTime out = -1,
                              bool withMarkers = true);
// The self-contained page: no scripts, styles or fonts from anywhere else.
std::string reviewPageHtml(const ReviewPageInfo& info);

struct ReviewNote {
    FrameTime frame = 0;  // in the sequence
    FrameTime duration = 0;
    std::string author, text;
    bool done = false;
};
// A notes file saved by the page: its notes, their frames converted to `fps` when the review copy had another rate.
// False (with `error`) if it is not one.
bool parseReviewNotes(const std::string& json, Rational fps, std::vector<ReviewNote>& out, std::string* title = nullptr,
                      std::string* error = nullptr);
// True if `text` looks like a notes file (so marker imports can take one).
bool isReviewNotes(const std::string& text);
// Markers for notes: named after the reviewer (a tick in front once resolved), the note as the comment, each
// reviewer in their own colour.
std::vector<Marker> reviewNotesToMarkers(const std::vector<ReviewNote>& notes);

}  // namespace montage
