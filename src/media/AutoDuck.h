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
    std::vector<std::string> skipRoles;  // clips of these audio roles are never speech
};

using Spans = std::vector<std::pair<double, double>>;  // [start, end) in timeline seconds, sorted

// Speech on these audio tracks of the sequence, merged across tracks and
// across pauses shorter than the options allow.
Spans dialogueSpans(const Project& p, const Sequence& s, const std::vector<int>& tracks, const DuckOptions& o,
                    std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);

// The speech audio description works around: the Dialogue clips when the
// sequence has any, else every clip but music, effects and the descriptions
// themselves (by role), on every track but `skipTrack`.
Spans descriptionSpeech(const Project& p, const Sequence& s, int skipTrack = -1, std::string* error = nullptr);

// The time the enabled clips on audio track `track` cover, joined across gaps
// shorter than `minPause`: for ducking under clips whose whole length is
// speech (a voiceover or a dub).
Spans clipSpans(const Sequence& s, int track, double minPause);

// Rewrites a clip's volume keyframes so it dips under the spans, from its
// level at its start. A clip no span reaches loses its keyframes and keeps
// that level. Returns whether the clip changed. With another `lane` (a dB
// lane added to the volume), the lane is built afresh from 0 dB and removed
// from a clip no span reaches.
bool duckClip(Clip& c, const Sequence& s, const Spans& spans, const DuckOptions& o, const std::string& lane = "gain_db");

// Dips every other clip under the audio description clips (role Description)
// on its own lane (kDescriptionDuckParam, heard only with the descriptions),
// fading over `fadeSeconds` either side so the dialogue around stays clear.
// Rebuilt each time; the clips' own volume is left alone. Returns how many
// clips changed.
int duckUnderDescriptions(Sequence& s, double amountDb, double fadeSeconds = 0.25);

}  // namespace montage
