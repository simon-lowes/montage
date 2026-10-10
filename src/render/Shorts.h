// Montage — Make Shorts (CapCut's long video to shorts, Descript's clip finding, OpusClip): a podcast, interview or
// webinar cut into vertical clips ready to post. The transcript gives sentence-aligned windows inside a length range;
// each is scored on how its first sentence hooks a viewer, how lively the stretch is (the Highlights score), how much
// it is about the topic asked for, and how cleanly it starts and ends, with fillers, long silences and crowded
// exchanges counting against it. The best that do not overlap become shorts: a new sequence each, framed for the
// platform round the subject, fillers and pauses cut out, captions in a social look and, if asked, the hook as a title.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "core/TranscriptEdit.h"

namespace montage {

struct ShortsOptions {
    int count = 5;
    double minSeconds = 15, maxSeconds = 60;
    std::string topic;      // what the shorts should be about ("" = anything): its words make a window more wanted
    bool liveliness = true;  // add the Highlights sound and motion score (reads the media)
    FillerOptions fillers;   // what counts as a filler (the transcript's language is used when this has none)
};

struct ShortMoment {
    Id media = 0;
    double in = 0, out = 0;  // media seconds: from just before the first word to just after the last
    size_t firstWord = 0, lastWord = 0;  // in the media's transcript, counted across segments
    double score = 0;
    double hook = 0;      // 0..1, the first sentence's
    std::string hookLine;  // the first sentence
    std::string text;      // everything said
};

// How strongly a sentence makes a viewer stay, 0..1: a question, "you", a number, "the secret", "never", "the best"...
// count for it; starting mid-thought ("and", "so", "because", "it"), with a filler, or rambling counts against it.
double hookScore(const std::string& sentence);

// The best windows of the media's transcripts, best first, never overlapping. Media without a transcript are left;
// empty (with `error`) when none has one or nothing fits the length range.
std::vector<ShortMoment> findShorts(const Project& p, const std::vector<Id>& media, const ShortsOptions& o,
                                    const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                    std::string* error = nullptr);

struct ShortBuild {
    int aspectW = 9, aspectH = 16;
    bool removeFillers = true;
    bool removePauses = true;           // silences over half a second shortened to a quarter
    FrameTime smoothCut = 0;            // frames of Smooth Cut over each join this leaves (0 = straight cuts)
    std::string captionLook = "creator_pop";  // core/Captions.h captionLooks(); "" = no captions
    bool hookTitle = false;             // the hook line as a title over the first seconds
    bool reframe = true;                // keep the subject in frame (else centred)
    int reframeSpeed = 1;
    FillerOptions fillers;
};

// A new sequence named `name` holding the moment, built as asked; added to the project (and its media), not made
// active. Returns its id, or 0 (with `error`).
Id makeShortSequence(Project& p, const ShortMoment& m, const ShortBuild& b, const std::string& name,
                     const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

}  // namespace montage
