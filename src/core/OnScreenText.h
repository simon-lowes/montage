// Montage — turning text read off the picture over time (media/TextReader.h) into captions: a reading held while
// it stays the same is one caption, and readings that differ by a misread letter or two count as the same.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Captions.h"
#include "Model.h"

namespace montage {

// How alike two readings are, 0-1 (1 - edit distance / longer length, on letters and digits, case ignored).
double readingSimilarity(const std::string& a, const std::string& b);

// Captions from readings (media seconds, text; "" where nothing was read) sampled `step` seconds apart: a run of
// alike readings is one caption, shown as the reading seen most often in it, from its first sample to the next
// different one; captions shorter than `minSeconds` are dropped. Timed in frames at `fps`.
std::vector<Caption> captionsFromReadings(const std::vector<std::pair<double, std::string>>& readings, double step, Rational fps,
                                          double minSeconds = 0.4, double sameAbove = 0.8);

}  // namespace montage
