// Montage — review and approval without a service (Frame.io in Premiere, Blackmagic Cloud review in Resolve): a review
// copy of the cut goes out with a single HTML page that plays it in any browser, offline, where each reviewer steps
// to a frame (or marks a range), draws over the picture (pen, arrow, box, ellipse) and types a note; notes take
// replies and a status (open, in progress, done). A second copy of the same cut is version 2, and its page compares
// it with the version before, side by side or with a wipe. The page keeps the notes in the browser and saves them as
// a small JSON file; that file (or several, one per reviewer) comes back and its notes become markers at their frames,
// coloured by reviewer, carrying their drawings (shown over the Program monitor); imported again, the same notes
// bring their markers up to date.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Model.h"

namespace montage {

// The version before, which the page can compare with.
struct ReviewVersion {
    int version = 1;
    std::string videoFile;  // relative to the page
    Rational fps{25, 1};
    FrameTime offset = 0, frames = 0;
};

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
    int version = 1;
    std::optional<ReviewVersion> previous;
};

// What a page written by reviewPageHtml holds (to compare a new version with it); nothing if it is not one.
std::optional<ReviewPageInfo> readReviewPage(const std::string& html);

// The page for a review copy of `s` (frames `in` to `out` exclusive; -1 = whole), with its markers when asked.
ReviewPageInfo reviewPageInfo(const Sequence& s, const std::string& videoFile, FrameTime in = -1, FrameTime out = -1,
                              bool withMarkers = true);
// The self-contained page: no scripts, styles or fonts from anywhere else.
std::string reviewPageHtml(const ReviewPageInfo& info);

// A stroke drawn over the picture: its points in 0..1 of the frame (x to the right, y down); an arrow, box or ellipse
// is its first and last point.
struct ReviewStroke {
    std::string tool = "pen";      // pen, arrow, box, ellipse
    std::string color = "#ff3b30";  // #rrggbb
    double width = 0.005;          // of the frame's width
    std::vector<std::array<double, 2>> points;
};
// A drawing as stored on a marker (JSON), and back; strokes that are not understood are left out, and a drawing is
// kept to 200 strokes of 4000 points at most.
std::string reviewDrawingJson(const std::vector<ReviewStroke>& strokes);
std::vector<ReviewStroke> parseReviewDrawing(const std::string& json);

struct ReviewReply {
    std::string author, text, created;
};

struct ReviewNote {
    FrameTime frame = 0;  // in the sequence
    FrameTime duration = 0;
    std::string author, text;  // (a note may be a drawing alone, without text)
    bool done = false;          // status is "done"
    std::string status = "open";  // open, progress, done
    int version = 0;              // the review copy it was written on (0: not said)
    std::string id;               // the page's id for it
    std::vector<ReviewReply> replies;
    std::string drawing;  // reviewDrawingJson ("" none)
};
// A notes file saved by the page: its notes, their frames converted to `fps` when the review copy had another rate.
// False (with `error`) if it is not one.
bool parseReviewNotes(const std::string& json, Rational fps, std::vector<ReviewNote>& out, std::string* title = nullptr,
                      std::string* error = nullptr);
// True if `text` looks like a notes file (so marker imports can take one).
bool isReviewNotes(const std::string& text);
// Markers for notes: named after the reviewer (a tick in front once done, a half circle while in progress, the
// version after a later one's), the note and its replies as the comment, the drawing and the note's id kept, each
// reviewer in their own colour.
std::vector<Marker> reviewNotesToMarkers(const std::vector<ReviewNote>& notes);

}  // namespace montage
