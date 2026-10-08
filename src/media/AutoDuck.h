// Montage — ducking music under dialogue (Premiere's Auto Ducking, Resolve's
// ducking). Wherever someone speaks on the dialogue tracks, the music clips'
// volume dips, fading down just before the speech and back up after it.
// The result is ordinary volume keyframes, so the line on each clip can be
// adjusted by hand afterwards.
#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"

namespace montage {

struct DuckOptions {
    double amountDb = -15;        // how far the music dips
    double fadeDown = 0.3;        // seconds before speech
    double fadeUp = 0.8;          // seconds after speech
    double minPause = 1.0;        // pauses shorter than this stay ducked
    double minSpeech = 0.2;       // shorter sounds are not speech
    double thresholdDb = -40;     // louder than this (dBFS, 50 ms mean square) is speech
    bool useTranscripts = true;   // a transcript's words mark speech where a clip's media has one
};

using Spans = std::vector<std::pair<double, double>>;  // [start, end) in timeline seconds, sorted

// Speech on these audio tracks of the sequence, merged across tracks and
// across pauses shorter than the options allow.
Spans dialogueSpans(const Project& p, const Sequence& s, const std::vector<int>& tracks, const DuckOptions& o,
                    std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);

// Rewrites a clip's volume keyframes so it dips under the spans, from its
// level at its start. A clip no span reaches loses its keyframes and keeps
// that level. Returns whether the clip changed.
bool duckClip(Clip& c, const Sequence& s, const Spans& spans, const DuckOptions& o);

}  // namespace montage
