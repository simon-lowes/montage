// Montage — the review copy and its page (core/ReviewPage.h): an H.264 copy at a size that streams and plays
// anywhere, timecode and an optional watermark burned in, next to the page reviewers open to leave notes.
#pragma once

#include <string>

#include "core/ReviewPage.h"
#include "render/Exporter.h"

namespace montage {

struct ReviewExportOptions {
    int maxHeight = 1080;      // smaller copies stream more easily; 0 = the sequence's size
    bool timecode = true;      // burned in, so a note can quote it even from a screenshot
    std::string watermark;     // e.g. "Review copy" ("" = none)
    bool markers = true;       // the editor's markers on the page
    std::string note;          // the editor's message to the reviewers
    FrameTime in = -1, out = -1;  // a range (out exclusive); -1 = the whole sequence
};

struct ReviewPackage {
    std::string videoPath;  // "<folder>/<name> - Review.mp4"
    std::string pagePath;   // "<folder>/<name> - Review.html"
    ExportSettings settings;
    ReviewPageInfo page;
};

ReviewPackage reviewPackage(const Sequence& s, const std::string& folder, const ReviewExportOptions& o = {});
// Writes the page (the copy is rendered separately, with `settings`). False with `error` if it cannot be written.
bool writeReviewPage(const ReviewPackage& p, std::string* error = nullptr);

}  // namespace montage
