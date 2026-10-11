// Montage — removing mic bleed from multitrack talk (Auphonic's crossgate; "de-bleed" is Premiere's request #35): with
// a mic on each speaker, every mic also hears the others, more quietly, and the mix sounds roomy and phasey. Wherever a
// track is not the one speaking (well below the loudest mic at that moment, or down at its own noise floor) its volume
// dips, fading out after its speaker stops and back just before they start. Two people talking at once both stay up.
// The result is ordinary volume keyframes on the clips, so each line can be adjusted by hand.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/AutoDuck.h"

namespace montage {

struct BleedOptions {
    double reductionDb = -24;  // how far a mic dips while it is not its speaker's turn
    double marginDb = 10;      // within this of the loudest mic counts as speaking (two at once)
    double floorDb = -50;      // quieter than this (dBFS) is nobody speaking, as is less than 6 dB over the mic's own floor
    double hold = 0.25;        // seconds a mic stays open after its speaker stops
    double fadeDown = 0.15, fadeUp = 0.03;  // seconds
    double minDip = 0.4;       // shorter quiet stretches are left alone
};

// For each of the tracks, in order, where it should dip (timeline seconds, within its clips). Needs two tracks or more.
std::vector<Spans> bleedSpans(const Project& p, const Sequence& s, const std::vector<int>& tracks, const BleedOptions& o,
                              std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);

// Writes the dips as volume keyframes on the tracks' clips (replacing their keyframes, from each clip's level at its
// start). Returns how many clips changed.
int removeMicBleed(Sequence& s, const std::vector<int>& tracks, const std::vector<Spans>& dips, const BleedOptions& o);

}  // namespace montage
